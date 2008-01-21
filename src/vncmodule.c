/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 or
 * later as published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include <pygobject.h>
 
void gtkvnc_register_classes (PyObject *d); 
void gtkvnc_add_constants(PyObject *module, const gchar *strip_prefix);
extern PyMethodDef gtkvnc_functions[];
 
DL_EXPORT(void) initgtkvnc(void);

DL_EXPORT(void) initgtkvnc(void)
{
    PyObject *m, *d;
 
    init_pygobject ();
 
    m = Py_InitModule ("gtkvnc", gtkvnc_functions);
    if (PyErr_Occurred())
	Py_FatalError("can't init module");

    d = PyModule_GetDict (m);
    if (PyErr_Occurred())
	Py_FatalError("can't get dict");
 
    gtkvnc_add_constants(m, "VNC_DISPLAY_");
    gtkvnc_register_classes (d);
 
    if (PyErr_Occurred ()) {
        Py_FatalError ("can't initialise module vnc");
    }
}
