/* -- THIS FILE IS GENERATED - DO NOT EDIT *//* -*- Mode: C; c-basic-offset: 4 -*- */

#include <Python.h>



#line 3 "vnc.override"
#include <Python.h>
#include "pygobject.h"
#include "vncdisplay.h"
#line 12 "vnc.c"


/* ---------- types from other modules ---------- */
static PyTypeObject *_PyGtkDrawingArea_Type;
#define PyGtkDrawingArea_Type (*_PyGtkDrawingArea_Type)


/* ---------- forward type declarations ---------- */
PyTypeObject G_GNUC_INTERNAL PyVncDisplay_Type;

#line 23 "vnc.c"



/* ----------- VncDisplay ----------- */

static int
_wrap_vnc_display_new(PyGObject *self, PyObject *args, PyObject *kwargs)
{
    static char* kwlist[] = { NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     ":vnc.Display.__init__",
                                     kwlist))
        return -1;

    pygobject_constructv(self, 0, NULL);
    if (!self->obj) {
        PyErr_SetString(
            PyExc_RuntimeError, 
            "could not create vnc.Display object");
        return -1;
    }
    return 0;
}

static PyObject *
_wrap_vnc_display_open(PyGObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = { "fd", NULL };
    int fd;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,"i:VncDisplay.open", kwlist, &fd))
        return NULL;
    
    vnc_display_open(VNC_DISPLAY(self->obj), fd);
    
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
_wrap_vnc_display_set_password(PyGObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = { "password", NULL };
    char *password;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,"s:VncDisplay.set_password", kwlist, &password))
        return NULL;
    
    vnc_display_set_password(VNC_DISPLAY(self->obj), password);
    
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
_wrap_vnc_display_set_use_shm(PyGObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = { "enable", NULL };
    int enable;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,"i:VncDisplay.set_use_shm", kwlist, &enable))
        return NULL;
    
    vnc_display_set_use_shm(VNC_DISPLAY(self->obj), enable);
    
    Py_INCREF(Py_None);
    return Py_None;
}

static const PyMethodDef _PyVncDisplay_methods[] = {
    { "open", (PyCFunction)_wrap_vnc_display_open, METH_VARARGS|METH_KEYWORDS,
      NULL },
    { "set_password", (PyCFunction)_wrap_vnc_display_set_password, METH_VARARGS|METH_KEYWORDS,
      NULL },
    { "set_use_shm", (PyCFunction)_wrap_vnc_display_set_use_shm, METH_VARARGS|METH_KEYWORDS,
      NULL },
    { NULL, NULL, 0, NULL }
};

PyTypeObject G_GNUC_INTERNAL PyVncDisplay_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                                 /* ob_size */
    "vnc.Display",                   /* tp_name */
    sizeof(PyGObject),          /* tp_basicsize */
    0,                                 /* tp_itemsize */
    /* methods */
    (destructor)0,        /* tp_dealloc */
    (printfunc)0,                      /* tp_print */
    (getattrfunc)0,       /* tp_getattr */
    (setattrfunc)0,       /* tp_setattr */
    (cmpfunc)0,           /* tp_compare */
    (reprfunc)0,             /* tp_repr */
    (PyNumberMethods*)0,     /* tp_as_number */
    (PySequenceMethods*)0, /* tp_as_sequence */
    (PyMappingMethods*)0,   /* tp_as_mapping */
    (hashfunc)0,             /* tp_hash */
    (ternaryfunc)0,          /* tp_call */
    (reprfunc)0,              /* tp_str */
    (getattrofunc)0,     /* tp_getattro */
    (setattrofunc)0,     /* tp_setattro */
    (PyBufferProcs*)0,  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,                      /* tp_flags */
    NULL,                        /* Documentation string */
    (traverseproc)0,     /* tp_traverse */
    (inquiry)0,             /* tp_clear */
    (richcmpfunc)0,   /* tp_richcompare */
    offsetof(PyGObject, weakreflist),             /* tp_weaklistoffset */
    (getiterfunc)0,          /* tp_iter */
    (iternextfunc)0,     /* tp_iternext */
    (struct PyMethodDef*)_PyVncDisplay_methods, /* tp_methods */
    (struct PyMemberDef*)0,              /* tp_members */
    (struct PyGetSetDef*)0,  /* tp_getset */
    NULL,                              /* tp_base */
    NULL,                              /* tp_dict */
    (descrgetfunc)0,    /* tp_descr_get */
    (descrsetfunc)0,    /* tp_descr_set */
    offsetof(PyGObject, inst_dict),                 /* tp_dictoffset */
    (initproc)_wrap_vnc_display_new,             /* tp_init */
    (allocfunc)0,           /* tp_alloc */
    (newfunc)0,               /* tp_new */
    (freefunc)0,             /* tp_free */
    (inquiry)0              /* tp_is_gc */
};



/* ----------- functions ----------- */

const PyMethodDef vnc_functions[] = {
    { NULL, NULL, 0, NULL }
};

/* initialise stuff extension classes */
void
vnc_register_classes(PyObject *d)
{
    PyObject *module;

    if ((module = PyImport_ImportModule("gtk")) != NULL) {
        _PyGtkDrawingArea_Type = (PyTypeObject *)PyObject_GetAttrString(module, "DrawingArea");
        if (_PyGtkDrawingArea_Type == NULL) {
            PyErr_SetString(PyExc_ImportError,
                "cannot import name DrawingArea from gtk");
            return ;
        }
    } else {
        PyErr_SetString(PyExc_ImportError,
            "could not import gtk");
        return ;
    }


#line 177 "vnc.c"
    pygobject_register_class(d, "VncDisplay", VNC_TYPE_DISPLAY, &PyVncDisplay_Type, Py_BuildValue("(O)", &PyGtkDrawingArea_Type));
    pyg_set_object_has_new_constructor(VNC_TYPE_DISPLAY);
}
