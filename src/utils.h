/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 or
 * later as published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#ifndef _UTILS_H
#define _UTILS_H

#include <glib.h>

extern gboolean debug_enabled;

#define GVNC_DEBUG(fmt, ...) do { if (G_UNLIKELY(debug_enabled)) g_debug(fmt, ## __VA_ARGS__); } while (0)

#endif
