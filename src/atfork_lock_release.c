#include <Python.h>
#include "bufferedio_structs.h"


#ifndef _POSIX_THREADS
# error atfork_lock_release requires posix threads to be available
#endif

#ifndef WITH_THREAD
# error atfork_lock_release will have no effect since threading is not enabled
#endif

#include <pthread.h>


/* Helper macros */

#define CHECK_STATUS(name)  if (status != 0) { fprintf(stderr, \
    "%s: %s\n", name, strerror(status)); error = 1; }

#define CHECK_HOOKS_ENABLED() if (! _hooks_enabled) { return; }

// First field of the pthread lock structure is a char. In order to access it
// we cast the lock to char* and dereference afterwards.
#define LOCK_ACQUIRED(_lock) (*((char*)(_lock)))


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

int _hooks_registered = 0;
int _hooks_enabled = 0;

/**
 * pre-fork hook
 * 
 * Obtains and checks the io locks for possible deadlocks. If one of the io-
 * streams is locked pre-fork, a warning is written to stderr.
 */
void _pre_fork() {
    CHECK_HOOKS_ENABLED();

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
}

/**
 * after-fork hook for the parent process
 *
 * Currently no-op as the parent locks should be freed automatically.
 */
void _after_fork_parent() {
    CHECK_HOOKS_ENABLED();
}

/**
 * after-fork hook for the child process
 *
 * Checks for locked io streams which have deadlocked and releases their
 * locks.
 */
void _after_fork_child() {
    CHECK_HOOKS_ENABLED();

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
}


/* Method definitions */

/**
 * Register the module's atfork hooks.
 */
PyObject* _register_hooks(PyObject *Py_UNUSED(ignored), PyObject *Py_UNUSED(args)) {
    int status, error = 0;

    if (!_hooks_registered) {
        status = pthread_atfork(_pre_fork, _after_fork_parent, _after_fork_child);
        CHECK_STATUS("pthread_atfork");

        if (error) {
            PyErr_Format(PyExc_RuntimeError,
                         "Unable to register atfork hooks: %s",
                         strerror(status));
            return NULL;
        }

        _hooks_registered = 1;
    }

    _hooks_enabled = 1;

    Py_RETURN_NONE;
}


/**
 * Deregister the module's atfork hooks.
 */
PyObject* _deregister_hooks(PyObject *Py_UNUSED(ignored), PyObject *Py_UNUSED(args)) {
    if (! _hooks_registered) {
        PyErr_SetString(PyExc_RuntimeError, "hooks are not registered yet");
        return NULL;
    }

    // The pthread library does not provide a way for us to deregister the
    // hooks, so we have to use a flag instead.

    _hooks_enabled = 0;

    Py_RETURN_NONE;
}


/* Module definition */

static struct PyMethodDef atfork_lock_release_methods[] = {
        { "register", (PyCFunction) _register_hooks, METH_NOARGS, "register atfork handlers"},
        { "deregister", (PyCFunction) _deregister_hooks, METH_NOARGS, "disable atfork handlers"},
        { NULL }
};


static struct PyModuleDef atfork_lock_release_module = {
        PyModuleDef_HEAD_INIT,
        "atfork_lock_release",
        NULL,
        -1,
        atfork_lock_release_methods,
        NULL,
        NULL,
        NULL,
        NULL
};


PyMODINIT_FUNC PyInit_atfork_lock_release(void) {
    PyObject *module = NULL;

    module = PyModule_Create(&atfork_lock_release_module);

    if (! module) {
        return NULL;
    }

    return module;
}
