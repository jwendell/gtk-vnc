/*
 * GTK VNC Widget
 *  
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
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
