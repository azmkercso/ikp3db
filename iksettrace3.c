#include "Python.h"
#include "frameobject.h"
#include "pystate.h"

/**
 * This file is part of the IKPdb Debugger
 * Copyright (c) 2016-2018 by cyril MORISSE, Audaxis
 * Licence: MIT. See LICENCE at repository root
 */

static long debuggerThreadIdent = 0;  // Track debugger thread ident


/* 
 * Redefine needed static functions from Python-3.x.x/Python/sysmodule.c
 */

/*
 * Cached interned string objects used for calling the profile and
 * trace functions.  Initialized by trace_init().
 */
static PyObject *whatstrings[7] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

#if PY_VERSION_HEX >= 0x030B00A1 && PY_VERSION_HEX < 0x030F00A1
// 'struct _frame' for 'typedef _frame PyFrameObject' is not exposed via Python C API since Python 3.11,
// however its members can still be accessed/written directly via Python stdlib calls
// Ikp3d passes the tracer function via an existing but now hidden 'f_trace' member
// The structure is the same for Copying structure here so it can be properly referenced here
// Python 3.11 sources:
// - PyFrameObject typedef: https://github.com/python/cpython/blob/v3.11.12/Include/pytypedefs.h#L22
//.- struct definition: https://github.com/python/cpython/blob/v3.11.12/Include/internal/pycore_frame.h#L15-L26
// - stdlib property getters/setters: https://github.com/python/cpython/blob/v3.11.12/Objects/frameobject.c#L834-L845
// Python 3.12 sources:
// - PyFrameObject typedef: https://github.com/python/cpython/blob/v3.12.10/Include/pytypedefs.h#L22
// - struct definition: https://github.com/python/cpython/blob/v3.12.10/Include/internal/pycore_frame.h#L16-L27
// - stdlib property getters/setters: https://github.com/python/cpython/blob/v3.12.10/Objects/frameobject.c#L869-L881
struct _frame {
    PyObject_HEAD
    PyFrameObject *f_back;
    // 'f_frame' is not used for ikp3db purposes
    // This is originally a struct pointer for '_PyInterpreterFrame' (https://github.com/python/cpython/blob/v3.11.12/Include/internal/pycore_frame.h#L18)
    // Leaving a placeholder 'void' pointer here to keep the correct struct offsets for the other members
    void *f_frame;
    PyObject *f_trace;
    int f_lineno;
    char f_trace_lines;
    char f_trace_opcodes;
# if PY_VERSION_HEX >= 0x030D00A1
    // Python 3.13 struct definition: https://github.com/python/cpython/blob/v3.13.3/Include/internal/pycore_frame.h#L20-L35
    PyObject *f_extra_locals;
    PyObject *f_locals_cache;
#  if PY_VERSION_HEX >= 0x030E00A1
    // Python 3.14 struct definition: https://github.com/python/cpython/blob/3.14/Include/internal/pycore_frame.h#L18-L39
    PyObject *f_overwritten_fast_locals;
#  endif
# else
    char f_fast_as_locals;
# endif
    PyObject *_f_frame_data[1];
};
#endif

static int trace_init(void)
{
    static const char * const whatnames[7] = {
        "call", "exception", "line", "return",
        "c_call", "c_exception", "c_return"
    };
    PyObject *name;
    int i;
    for (i = 0; i < 7; ++i) {
        if (whatstrings[i] == NULL) {
            name = PyUnicode_InternFromString(whatnames[i]);
            if (name == NULL)
                return -1;
            whatstrings[i] = name;
        }
    }
    return 0;
}

static PyObject *
call_trampoline(PyObject* callback,
                PyFrameObject *frame, int what, PyObject *arg)
{
    PyObject *result;
    PyObject *stack[3];
    PyObject *locals;

#if PY_VERSION_HEX >= 0x030B00A1
    // This call is needed to initialise frame local fields
    // The return value is a strong reference, but not used by us
    locals = PyFrame_GetLocals(frame);
    if (locals == NULL) {
        return NULL;
    }
#else
    if (PyFrame_FastToLocalsWithError(frame) < 0) {
        return NULL;
    }
#endif

    stack[0] = (PyObject *)frame;
    stack[1] = whatstrings[what];
    stack[2] = (arg != NULL) ? arg : Py_None;
    /* call the Python-level function */
    result = _PyObject_FastCall(callback, stack, 3);

#if PY_VERSION_HEX >= 0x030B00A1
    // Release the strong reference created by PyFrame_GetLocals
    Py_CLEAR(locals);
#else
    PyFrame_LocalsToFast(frame, 1);
#endif
    if (result == NULL) {
        PyTraceBack_Here(frame);
    }

    return result;
}

static int
trace_trampoline(PyObject *self, PyFrameObject *frame,
                 int what, PyObject *arg)
{
    PyObject *callback;
    PyObject *result;

    if (what == PyTrace_CALL)
        callback = self;
    else
        callback = frame->f_trace;

    if (callback == NULL)
        return 0;

    result = call_trampoline(callback, frame, what, arg);
    if (result == NULL) {
        PyEval_SetTrace(NULL, NULL);
        Py_CLEAR(frame->f_trace);
        return -1;
    }
    if (result != Py_None) {
        Py_XSETREF(frame->f_trace, result);
    }
    else {
        Py_DECREF(result);
    }

    return 0;
}

/* 
 * iksettrace3 'real' functions
 */

/*
* Based on https://docs.python.org/3/whatsnew/3.11.html#whatsnew311-c-api-porting
*/
static inline void
IK_UseTracing(PyThreadState *tstate, int use_tracing)
{
#if PY_VERSION_HEX >= 0x030B00A1
    if (use_tracing) {
        PyThreadState_EnterTracing(tstate);
    } else {
        PyThreadState_LeaveTracing(tstate);
    }
#elif PY_VERSION_HEX >= 0x030A00A1
    tstate->tracing += use_tracing ? 1 : -1;
    tstate->cframe->use_tracing = use_tracing ? 255 : 0;
#else
    tstate->tracing += use_tracing ? 1 : -1;
    tstate->use_tracing = use_tracing;
#endif
}

void
IK_SetTrace(Py_tracefunc func, PyObject *arg)
{
    // Ensure _Py_TracingPossible is correctly set
    PyEval_SetTrace(func, arg);  
    
    // Now iterate over all threads to set tracing
    PyInterpreterState *interp = PyInterpreterState_Head();
    PyThreadState *loopThreadState = PyInterpreterState_ThreadHead(interp);
    while(loopThreadState) {
        if ((unsigned long)loopThreadState->thread_id != (unsigned long)debuggerThreadIdent) {
            PyObject *temp = loopThreadState->c_traceobj;
            Py_XINCREF(arg);
            loopThreadState->c_tracefunc = NULL;
            loopThreadState->c_traceobj = NULL;
            /* Must make sure that profiling is not ignored if 'temp' is freed */
            IK_UseTracing(loopThreadState, loopThreadState->c_profilefunc != NULL);
            Py_XDECREF(temp);
            loopThreadState->c_tracefunc = func;
            loopThreadState->c_traceobj = arg;
            /* Flag that tracing or profiling is turned on */
            IK_UseTracing(loopThreadState, (func != NULL) || (loopThreadState->c_profilefunc != NULL));
        } else {
            PyObject *temp = loopThreadState->c_traceobj;
            loopThreadState->c_tracefunc = NULL;
            loopThreadState->c_traceobj = NULL;
            /* Must make sure that profiling is not ignored if 'temp' is freed */
            IK_UseTracing(loopThreadState, loopThreadState->c_profilefunc != NULL);
            Py_XDECREF(temp);

        };
        loopThreadState = PyThreadState_Next(loopThreadState);
    };
}


static PyObject *
_ik_set_trace_on(PyObject *self, PyObject *args)
{
    PyObject *traceObject = NULL;

    if (trace_init() == -1)
        return NULL;
    
    if (!PyArg_ParseTuple(args, "Ol", &traceObject, &debuggerThreadIdent)) {
        return NULL;
    }    

    IK_SetTrace(trace_trampoline, traceObject);

    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(_ik_set_trace_on_doc,
"_set_trace_on(tracer, thread_id)\n\
\n\
Activate tracing with tracer function, on all threads but the one specified.\n\
See the debugger chapter in the library manual.\n\
This function do not call threading.settrace(), user must do it."
);


static PyObject *
_ik_set_trace_off(PyObject *self)
{
    IK_SetTrace(NULL, NULL);
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(_ik_set_trace_off_doc,
"_set_trace_off()\n\
\n\
Disable tracing on all threads.\n\
See the debugger chapter in the library manual.\n\
This function do not call threading.settrace(), user must do it."
);


static PyMethodDef InoukMethods[] = {
    {"_set_trace_on", _ik_set_trace_on, METH_VARARGS, _ik_set_trace_on_doc},
    {"_set_trace_off", (PyCFunction)_ik_set_trace_off, METH_NOARGS, _ik_set_trace_off_doc},
    {NULL, NULL, 0, NULL}           /* sentinel */
};

PyDoc_STRVAR(iksettrace3_module_doc,
"iksettrace3 module\n\
\n\
Contains 2 functions that allows to enable and disable tracing on all threads (even existing one).\n\
Note that functions in this module do not call threading.settrace() to set trace method on future threads.\n\
Module user must do it.");



static struct PyModuleDef iksettrace3_module = {
   PyModuleDef_HEAD_INIT,
   "iksettrace3",   /* name of module */
   iksettrace3_module_doc, /* module documentation, may be NULL */
   -1,       /* size of per-interpreter state of the module,
                or -1 if the module keeps state in global variables. */
   InoukMethods
};

PyMODINIT_FUNC
PyInit_iksettrace3(void)
{
    return PyModule_Create(&iksettrace3_module);
}
