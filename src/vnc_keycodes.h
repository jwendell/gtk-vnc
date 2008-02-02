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
