/**
 * bufferedio struct copied from cpython/Modules/_io/bufferdio.c as the private
 * structure cannot be accessed from within our module.
 *
 * Note that this structure needs to be adapted in case the struct changes in
 * upstream.
 */

#include <Python.h>
#include <pythread.h>

typedef off_t Py_off_t;

typedef struct {
    PyObject_HEAD

    PyObject *raw;
    int ok;    /* Initialized? */
    int detached;
    int readable;
    int writable;
    char finalizing;

    /* True if this is a vanilla Buffered object (rather than a user derived
       class) *and* the raw stream is a vanilla FileIO object. */
    int fast_closed_checks;

    /* Absolute position inside the raw stream (-1 if unknown). */
    Py_off_t abs_pos;

    /* A static buffer of size `buffer_size` */
    char *buffer;
    /* Current logical position in the buffer. */
    Py_off_t pos;
    /* Position of the raw stream in the buffer. */
    Py_off_t raw_pos;

    /* Just after the last buffered byte in the buffer, or -1 if the buffer
       isn't ready for reading. */
    Py_off_t read_end;

    /* Just after the last byte actually written */
    Py_off_t write_pos;
    /* Just after the last byte waiting to be written, or -1 if the buffer
       isn't ready for writing. */
    Py_off_t write_end;

    PyThread_type_lock lock;
    volatile unsigned long owner;

    Py_ssize_t buffer_size;
    Py_ssize_t buffer_mask;

    PyObject *dict;
    PyObject *weakreflist;
} buffered;

typedef struct
{
    PyObject_HEAD
    int ok; /* initialized? */
    int detached;
    Py_ssize_t chunk_size;
    PyObject *buffer;

    /* other fields are not needed */
} textio;
