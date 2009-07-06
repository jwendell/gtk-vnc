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

#ifndef _GTK_VNC_KEYCODES
#define _GTK_VNC_KEYCODES

/* All keycodes from 0 to 0xFF correspond to the hardware keycodes generated
 * by a US101 PC keyboard with the following encoding:
 *
 * 0) Sequences of XX are replaced with XX
 * 1) Sequences of 0xe0 XX are replaces with XX | 0x80
 * 2) All other keys are defined below
 */

#define VKC_PAUSE	0x100

#endif
