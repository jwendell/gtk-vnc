/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

/*
 * qemu-module.c -- QEMU GTK Widget Python Bindings
 *
 *  Copyright (C) 2006 Anthony Liguori <anthony@codemonkey.ws>
 *
 * This file is subject to the terms and conditions of the GNU Lesser General
 * Public License.  See the file COPYING.lib in the main directory of this
 * archive for more details.
 */

#include <pygobject.h>
 
void gtkvnc_register_classes (PyObject *d); 
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
 
    gtkvnc_register_classes (d);
 
    if (PyErr_Occurred ()) {
        Py_FatalError ("can't initialise module vnc");
    }
}
