#include <Python.h>
#include <pythread.h>


#if PY_MAJOR_VERSION == 3
# include "structs3.h"
#else
# error atfork_lock_release only supports python 3
#endif

#ifndef _POSIX_THREADS
# error atfork_lock_release requires posix threads to be available
#endif

#ifndef WITH_THREAD
# error atfork_lock_release will have no effect since threading is not enabled
#endif

#include <pthread.h>


/* Forward declaration of module object */
static struct PyModuleDef atfork_lock_release_module;
extern PyTypeObject PyTextIOWrapper_Type;


/* Helper macros */

// If semaphores are available, we use them directly. See thread_pthread.h
// for further details.
#if (defined(_POSIX_SEMAPHORES) && !defined(HAVE_BROKEN_POSIX_SEMAPHORES) && \
     defined(HAVE_SEM_TIMEDWAIT))
#include <semaphore.h>
#include <errno.h>
int _sem_lock_acquired(void *lock) {
    sem_t *sem = (sem_t*) lock;

    int status;

    do {
        // When using raw semaphores, we need to check if we can lock it ourselves.
        status = sem_trywait(sem);
        if (status == 0) {
            // Lock successful, free it again (otherwise we are causing
            // a deadlock ourselves)
            do {
                if (sem_post(sem) == 0) {
                    // Semaphore released, ready to go.
                    return 0;
                }

                // Check for EINTR again, we definitely need to release this
                // semaphore.
            } while (errno == EINTR);

            perror("sem_post");

            // We were unable to release the semaphore, so we pretend is was
            // locked from the beginning. According to the documentation there
            // should be no way of this happening, though.
            return 1;
        }

        if (errno == EAGAIN) {
            // Already locked
            return 1;
        }

        // sem_trywait might have returned EINTR, we will just try again.
    } while (status == -1 && errno == EINTR);

    perror("sem_trywait");
    return 0;
}
# define LOCK_ACQUIRED(_lock) (_sem_lock_acquired(_lock))
#else
// First field of the pthread lock structure is a char. In order to access it
// we cast the lock to char* and dereference afterwards.
# define LOCK_ACQUIRED(_lock) (*((char*)(_lock)))
#endif


/* Module state */

/**
 * Linked list node for atfork callbacks
 */
typedef struct _atfork_callback {
    PyObject *callback;
    struct _atfork_callback *next;
} atfork_callback;

/**
 * Linked list node for watchables (e.g. file streams)
 */
typedef struct _atfork_watchable {
    PyObject *item;
    struct _atfork_watchable *next;
} atfork_watchable;

typedef struct {
    atfork_callback *callback_pre_fork;
    atfork_callback *callback_after_fork_parent;
    atfork_callback *callback_after_fork_child;

    atfork_watchable *watchlist;

    int hooks_registered;
    int hooks_enabled;
} module_state;

#define MODULE_STATE(mod) ((module_state*)PyModule_GetState(mod))


/* Callback machinery */

/**
 * Template method for adding atfork callbacks
 * @param args Argument tuple provided to the function
 * @param result Pointer to a result pointer which will store the created callback object
 * @return
 */
static int create_callback(PyObject *args, atfork_callback **result) {
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

    if (create_callback(args, &callback)) {
        return NULL;
    }

    callback->next = modstate->callback_pre_fork;
    modstate->callback_pre_fork = callback;

    // Return the function to allow usage as decorator
    PyObject *func = callback->callback;
    Py_INCREF(func);
    return func;
}

/**
 * Add a callback for the parent process after fork
 */
static PyObject *add_post_fork_parent_callback(PyObject *self, PyObject *args, PyObject *Py_UNUSED(kwargs)) {
    atfork_callback *callback;
    module_state *modstate = MODULE_STATE(self);

    if (create_callback(args, &callback)) {
        return NULL;
    }

    callback->next = modstate->callback_after_fork_parent;
    modstate->callback_after_fork_parent = callback;

    // Return the function to allow usage as decorator
    PyObject *func = callback->callback;
    Py_INCREF(func);
    return func;
}

/**
 * Add a callback for the child process after fork
 */
static PyObject *add_post_fork_child_callback(PyObject *self, PyObject *args, PyObject *Py_UNUSED(kwargs)) {
    atfork_callback *callback;
    module_state *modstate = MODULE_STATE(self);

    if (create_callback(args, &callback)) {
        return NULL;
    }

    callback->next = modstate->callback_after_fork_child;
    modstate->callback_after_fork_child = callback;

    // Return the function to allow usage as decorator
    PyObject *func = callback->callback;
    Py_INCREF(func);
    return func;
}

/**
 * Execute callbacks starting with `callback`
 * @param callback First item of the linked list
 */
static void run_callbacks(atfork_callback *callback) {
    PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;

    while (callback != NULL) {
        PyObject *args = PyTuple_New(0);
        PyObject *result = PyObject_Call(callback->callback, args, NULL);
        Py_DECREF(args);
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


/* Utility */

/**
 * Obtain the lock instance from a `TextIOWrapper` object.
 */
PyThread_type_lock get_lock_from_textiowrapper(PyObject *object) {
    return ((buffered*) ((textio*) object)->buffer)->lock;
}

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
int get_io_locks(PyThread_type_lock *stdout_lock, PyThread_type_lock *stderr_lock) {
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

    if (Py_TYPE(sys_stdout) != &PyTextIOWrapper_Type) {
        PyErr_Warn(PyExc_RuntimeWarning, "sys.stdout is not text based.");
        return 1;
    }

    if (Py_TYPE(sys_stderr) != &PyTextIOWrapper_Type) {
        PyErr_Warn(PyExc_RuntimeWarning, "sys.stderr is not text based.");
        return 1;
    }

    // sys.std{out,err} is wrapped in `_io::TextIOWrapper` which contains a pointer
    // to the buffer object itself.
    *stdout_lock = get_lock_from_textiowrapper(sys_stdout);
    *stderr_lock = get_lock_from_textiowrapper(sys_stderr);

    return 0;
}


/* atfork handlers */

/**
 * pre-fork hook
 * 
 * Obtains and checks the io locks for possible deadlocks. If one of the io-
 * streams is locked pre-fork, a warning is written to stderr.
 */
void _pre_fork(void) {
    PyObject *module = PyState_FindModule(&atfork_lock_release_module);
    if (module == NULL)
        return;
    module_state *modstate = MODULE_STATE(module);

    if (! modstate->hooks_enabled)
        return;

    PyThread_type_lock stdout_lock, stderr_lock = NULL;
    if (! get_io_locks(&stdout_lock, &stderr_lock)) {
        if (stdout_lock == NULL) {
            PyErr_WarnEx(PyExc_RuntimeWarning, "unable to obtain stdout lock", 1);
            return;
        }
        if (stderr_lock == NULL) {
            PyErr_WarnEx(PyExc_RuntimeWarning, "unable to obtain stderr lock", 1);
            return;
        }

        if (LOCK_ACQUIRED(stdout_lock)) {
            fprintf(stderr, "possible deadlock for sys.stdout\n");
        }

        if (LOCK_ACQUIRED(stderr_lock)) {
            fprintf(stderr, "possible deadlock for sys.stderr\n");
        }
    }

    atfork_watchable *current = modstate->watchlist;
    while (current != NULL) {
        PyThread_type_lock lock = get_lock_from_textiowrapper(current->item);
        if (LOCK_ACQUIRED(lock)) {
            fprintf(stderr, "possible deadlock for file descriptor\n");
        }
        current = current->next;
    }

    run_callbacks(modstate->callback_pre_fork);
}

/**
 * after-fork hook for the parent process
 *
 * Currently no-op as the parent locks should be freed automatically.
 */
void _after_fork_parent(void) {
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
void _after_fork_child(void) {
    PyObject *module = PyState_FindModule(&atfork_lock_release_module);
    if (module == NULL)
        return;
    module_state *modstate = MODULE_STATE(module);

    if (! modstate->hooks_enabled)
        return;

    PyThread_type_lock stdout_lock, stderr_lock = NULL;
    if (! get_io_locks(&stdout_lock, &stderr_lock)) {
        if (stdout_lock == NULL) {
            PyErr_WarnEx(PyExc_RuntimeWarning, "unable to obtain stdout lock", 1);
            return;
        }
        if (stderr_lock == NULL) {
            PyErr_WarnEx(PyExc_RuntimeWarning, "unable to obtain stderr lock", 1);
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
    }

    atfork_watchable *current = modstate->watchlist;
    while (current != NULL) {
        PyThread_type_lock lock = get_lock_from_textiowrapper(current->item);
        if (LOCK_ACQUIRED(lock)) {
            fprintf(stderr, "deadlock for file descriptor\n");
            PyThread_release_lock(lock);
        }
        current = current->next;
    }

    run_callbacks(modstate->callback_after_fork_child);
}


/* Module method definitions */

/**
 * Register the module's atfork hooks.
 */
PyObject* register_hooks(PyObject *self, PyObject *Py_UNUSED(args)) {
    int status;
    module_state *modstate = MODULE_STATE(self);

    if (! modstate->hooks_registered) {
        status = pthread_atfork(_pre_fork, _after_fork_parent, _after_fork_child);
        if (status != 0) {
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
PyObject* deregister_hooks(PyObject *self, PyObject *Py_UNUSED(args)) {
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

/**
 * Add a `TextIOWrapper` instance to the watchlist.
 */
PyObject* watch_textiowrapper(PyObject *self, PyObject *args, PyObject *Py_UNUSED(kwargs)) {
    module_state *modstate = MODULE_STATE(self);

    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "function takes exactly 1 argument");
        return NULL;
    }

    PyObject *item = PyTuple_GetItem(args, 0);

    if (Py_TYPE(item) != &PyTextIOWrapper_Type) {
        PyErr_SetString(PyExc_ValueError, "expecting object wrapped in TextIOWrapper");
        return NULL;
    }

    atfork_watchable *watchable = PyMem_Malloc(sizeof(atfork_watchable));
    if (watchable == NULL) {
        return PyErr_NoMemory();
    }

    watchable->item = item;
    watchable->next = modstate->watchlist;
    modstate->watchlist = watchable;

    Py_INCREF(item);

    Py_RETURN_NONE;
}


/* Module definition */

/**
 * Module cleanup
 */
static int atfork_clear(PyObject *self) {
    module_state *modstate = MODULE_STATE(self);

    clear_callbacks(modstate->callback_pre_fork);
    clear_callbacks(modstate->callback_after_fork_parent);
    clear_callbacks(modstate->callback_after_fork_child);

    modstate->callback_pre_fork = modstate->callback_after_fork_parent = modstate->callback_after_fork_child = NULL;

    while (modstate->watchlist != NULL) {
        atfork_watchable *item = modstate->watchlist;
        modstate->watchlist = item->next;

        Py_DECREF(item->item);
        PyMem_Free(item);
    }

    modstate->watchlist = NULL;

    return 0;
}

/**
 * Module cleanup
 */
static void atfork_free(PyObject *self) {
    atfork_clear(self);
}

static struct PyMethodDef atfork_lock_release_methods[] = {
        { "register", (PyCFunction) register_hooks, METH_NOARGS, "register atfork handlers" },
        { "deregister", (PyCFunction) deregister_hooks, METH_NOARGS, "disable atfork handlers" },
        { "pre_fork", (PyCFunction) add_pre_fork_callback, METH_VARARGS, "" },
        { "after_fork_parent", (PyCFunction) add_post_fork_parent_callback, METH_VARARGS, "" },
        { "after_fork_child", (PyCFunction) add_post_fork_child_callback, METH_VARARGS, "" },
        { "watch", (PyCFunction) watch_textiowrapper, METH_VARARGS, "" },
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
    modstate->watchlist = NULL;
    modstate->hooks_enabled = modstate->hooks_registered = 0;

    return module;
}
