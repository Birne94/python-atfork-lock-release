#include <Python.h>
#include "bufferedio_structs.h"


#ifndef _POSIX_THREADS
# error atfork_lock_release requires posix threads to be available
#endif

#ifndef WITH_THREAD
# error atfork_lock_release will have no effect since threading is not enabled
#endif

#include <pthread.h>


/* Forward declaration of module object */
static struct PyModuleDef atfork_lock_release_module;


/* Helper macros */

#define CHECK_STATUS(name)  if (status != 0) { fprintf(stderr, \
    "%s: %s\n", name, strerror(status)); error = 1; }

// First field of the pthread lock structure is a char. In order to access it
// we cast the lock to char* and dereference afterwards.
#define LOCK_ACQUIRED(_lock) (*((char*)(_lock)))


/* Module state */

typedef struct _atfork_callback {
    PyObject *callback;
    struct _atfork_callback *next;
} atfork_callback;

typedef struct {
    atfork_callback *callback_pre_fork;
    atfork_callback *callback_after_fork_parent;
    atfork_callback *callback_after_fork_child;

    int hooks_registered;
    int hooks_enabled;
} module_state;

#define MODULE_STATE(mod) ((module_state*)PyModule_GetState(mod))

/* Callback utils */

/**
 * Template method for adding atfork callbacks
 * @param args Argument tuple provided to the function
 * @param result Pointer to a result pointer which will store the created callback object
 * @return
 */
static int add_callback(PyObject *args, atfork_callback **result) {
    PyObject *callback;
    atfork_callback *af_callback = NULL;

    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "atfork() takes exactly 1 argument");
        return 1;
    }

    callback = PyTuple_GetItem(args, 0);

    if (! PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_ValueError, "atfork() argument must be callable");
        return 1;
    }

    af_callback = PyMem_Malloc(sizeof(atfork_callback));
    if (af_callback == NULL) {
        PyErr_NoMemory();
        return 1;
    }

    af_callback->callback = callback;
    Py_INCREF(callback);

    *result = af_callback;

    return 0;
}

/**
 * Add a callback for the parent process pre fork
 */
static PyObject *add_pre_fork_callback(PyObject *self, PyObject *args, PyObject *Py_UNUSED(kwargs)) {
    atfork_callback *callback;
    module_state *modstate = MODULE_STATE(self);

    if (add_callback(args, &callback)) {
        return NULL;
    }

    callback->next = modstate->callback_pre_fork;
    modstate->callback_pre_fork = callback;

    Py_RETURN_NONE;
}

/**
 * Add a callback for the parent process after fork
 */
static PyObject *add_post_fork_parent_callback(PyObject *self, PyObject *args, PyObject *Py_UNUSED(kwargs)) {
    atfork_callback *callback;
    module_state *modstate = MODULE_STATE(self);

    if (add_callback(args, &callback)) {
        return NULL;
    }

    callback->next = modstate->callback_after_fork_parent;
    modstate->callback_after_fork_parent = callback;

    Py_RETURN_NONE;
}

/**
 * Add a callback for the child process after fork
 */
static PyObject *add_post_fork_child_callback(PyObject *self, PyObject *args, PyObject *Py_UNUSED(kwargs)) {
    atfork_callback *callback;
    module_state *modstate = MODULE_STATE(self);

    if (add_callback(args, &callback)) {
        return NULL;
    }

    callback->next = modstate->callback_after_fork_child;
    modstate->callback_after_fork_child = callback;

    Py_RETURN_NONE;
}

/**
 * Execute callbacks starting with `callback`
 * @param callback First item of the linked list
 */
static void run_callbacks(atfork_callback *callback) {
    PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;

    while (callback != NULL) {
        PyObject *result = PyObject_Call(callback->callback, Py_None, NULL);
        Py_XDECREF(result);

        // NULL indicates an exception being thrown.
        if (result == NULL) {
            if (exc_type) {
                Py_DECREF(exc_type);
                Py_XDECREF(exc_value);
                Py_XDECREF(exc_tb);
            }
            PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
            if (!PyErr_ExceptionMatches(PyExc_SystemExit)) {
                PySys_WriteStderr("Error in atfork handler:\n");
                PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);
                PyErr_Display(exc_type, exc_value, exc_tb);
            }
        }

        callback = callback->next;
    }

    // Restore last exception
    if (exc_type) {
        PyErr_Restore(exc_type, exc_value, exc_tb);
    }
}

/**
 * Free callback data
 *
 * @param callback First item of linked callback list
 */
static void clear_callbacks(atfork_callback *callback) {
    while (callback != NULL) {
        atfork_callback *item = callback;
        callback = callback->next;

        Py_DECREF(item->callback);
        PyMem_Free(item);
    }
}

/**
 * Module cleanup
 */
static int atfork_clear(PyObject *self) {
    module_state *modstate = MODULE_STATE(self);

    clear_callbacks(modstate->callback_pre_fork);
    clear_callbacks(modstate->callback_after_fork_parent);
    clear_callbacks(modstate->callback_after_fork_child);

    modstate->callback_pre_fork = modstate->callback_after_fork_parent = modstate->callback_after_fork_child = NULL;

    return 0;
}

/**
 * Module cleanup
 */
static void atfork_free(PyObject *self) {
    atfork_clear(self);
}


/* Helper functions */

/**
 * Obtain the locks for stdout and stderr
 *
 * If std{out,err} cannot be obtained from the sys-module, an exception
 * is raised and a non-zero value is returned.
 *
 * @param stdout_lock pointer to a `PyThread_type_lock` instance.
 * @param stderr_lock pointer to a `PyThread_type_lock` instance.
 * @return 0 if all locks could be obtained.
 */
int _get_io_locks(PyThread_type_lock *stdout_lock, PyThread_type_lock *stderr_lock) {
    PyObject *sys_stdout = PySys_GetObject("stdout");
    if (sys_stdout == NULL || sys_stdout == Py_None) {
        PyErr_SetString(PyExc_RuntimeError, "unable to obtain sys.stdout");
        return 1;
    }

    PyObject *sys_stderr = PySys_GetObject("stderr");
    if (sys_stderr == NULL || sys_stderr == Py_None) {
        PyErr_SetString(PyExc_RuntimeError, "unable to obtain sys.stderr");
        return 1;
    }

    // sys.std{out,err} is wrapped in `_io::TextIOWrapper` which contains a pointer
    // to the buffer object itself.
    *stdout_lock = ((buffered*) ((textio*) sys_stdout)->buffer)->lock;
    *stderr_lock = ((buffered*) ((textio*) sys_stderr)->buffer)->lock;

    return 0;
}


/* Module locals */

/**
 * pre-fork hook
 * 
 * Obtains and checks the io locks for possible deadlocks. If one of the io-
 * streams is locked pre-fork, a warning is written to stderr.
 */
void _pre_fork() {
    PyObject *module = PyState_FindModule(&atfork_lock_release_module);
    if (module == NULL)
        return;
    module_state *modstate = MODULE_STATE(module);

    if (! modstate->hooks_enabled)
        return;

    PyThread_type_lock stdout_lock, stderr_lock = NULL;
    if (_get_io_locks(&stdout_lock, &stderr_lock)) {
        return;
    }

    if (stdout_lock == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unable to obtain stdout lock");
        return;
    }
    if (stderr_lock == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unable to obtain stderr lock");
        return;
    }

    if (LOCK_ACQUIRED(stdout_lock)) {
        fprintf(stderr, "possible deadlock for sys.stdout\n");
    }

    if (LOCK_ACQUIRED(stderr_lock)) {
        fprintf(stderr, "possible deadlock for sys.stderr\n");
    }

    run_callbacks(modstate->callback_pre_fork);
}

/**
 * after-fork hook for the parent process
 *
 * Currently no-op as the parent locks should be freed automatically.
 */
void _after_fork_parent() {
    PyObject *module = PyState_FindModule(&atfork_lock_release_module);
    if (module == NULL)
        return;
    module_state *modstate = MODULE_STATE(module);

    if (! modstate->hooks_enabled)
        return;

    run_callbacks(modstate->callback_after_fork_parent);
}

/**
 * after-fork hook for the child process
 *
 * Checks for locked io streams which have deadlocked and releases their
 * locks.
 */
void _after_fork_child() {
    PyObject *module = PyState_FindModule(&atfork_lock_release_module);
    if (module == NULL)
        return;
    module_state *modstate = MODULE_STATE(module);

    if (! modstate->hooks_enabled)
        return;

    PyThread_type_lock stdout_lock, stderr_lock = NULL;
    if (_get_io_locks(&stdout_lock, &stderr_lock)) {
        return;
    }

    if (stdout_lock == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unable to obtain stdout lock");
        return;
    }
    if (stderr_lock == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unable to obtain stderr lock");
        return;
    }

    if (LOCK_ACQUIRED(stdout_lock)) {
        // directly write to stderr instead of issuing a warning, as
        // the warning itself might cause a deadlock.
        fprintf(stderr, "deadlock for sys.stdout, releasing\n");
        PyThread_release_lock(stdout_lock);
    }

    if (LOCK_ACQUIRED(stderr_lock)) {
        // directly write to stderr instead of issuing a warning, as
        // the warning itself might cause a deadlock.
        fprintf(stderr, "deadlock for sys.stderr, releasing\n");
        PyThread_release_lock(stderr_lock);
    }

    run_callbacks(modstate->callback_after_fork_child);
}


/* Method definitions */

/**
 * Register the module's atfork hooks.
 */
PyObject* _register_hooks(PyObject *self, PyObject *Py_UNUSED(args)) {
    int status, error = 0;
    module_state *modstate = MODULE_STATE(self);

    if (! modstate->hooks_registered) {
        status = pthread_atfork(_pre_fork, _after_fork_parent, _after_fork_child);
        CHECK_STATUS("pthread_atfork");

        if (error) {
            PyErr_Format(PyExc_RuntimeError,
                         "Unable to register atfork hooks: %s",
                         strerror(status));
            return NULL;
        }

        modstate->hooks_registered = 1;
    }

    modstate->hooks_enabled = 1;

    Py_RETURN_NONE;
}


/**
 * Deregister the module's atfork hooks.
 */
PyObject* _deregister_hooks(PyObject *self, PyObject *Py_UNUSED(args)) {
    module_state *modstate = MODULE_STATE(self);

    if (! modstate->hooks_registered) {
        PyErr_SetString(PyExc_RuntimeError, "hooks are not registered yet");
        return NULL;
    }

    // The pthread library does not provide a way for us to deregister the
    // hooks, so we have to use a flag instead.

    modstate->hooks_enabled = 0;

    Py_RETURN_NONE;
}


/* Module definition */

static struct PyMethodDef atfork_lock_release_methods[] = {
        { "register", (PyCFunction) _register_hooks, METH_NOARGS, "register atfork handlers"},
        { "deregister", (PyCFunction) _deregister_hooks, METH_NOARGS, "disable atfork handlers"},
        { "pre_fork", (PyCFunction) add_pre_fork_callback, METH_VARARGS, ""},
        { "after_fork_parent", (PyCFunction) add_post_fork_parent_callback, METH_VARARGS, ""},
        { "after_fork_child", (PyCFunction) add_post_fork_child_callback, METH_VARARGS, ""},
        { NULL }
};


static struct PyModuleDef atfork_lock_release_module = {
        PyModuleDef_HEAD_INIT,
        "atfork_lock_release",
        NULL,
        sizeof(module_state),
        atfork_lock_release_methods,
        NULL,
        NULL,
        atfork_clear,
        (freefunc) atfork_free
};


PyMODINIT_FUNC PyInit_atfork_lock_release(void) {
    PyObject *module = NULL;

    module = PyModule_Create(&atfork_lock_release_module);

    if (! module) {
        return NULL;
    }

    module_state *modstate = MODULE_STATE(module);
    modstate->callback_pre_fork = modstate->callback_after_fork_parent = modstate->callback_after_fork_child = NULL;
    modstate->hooks_enabled = modstate->hooks_registered = 0;

    return module;
}
