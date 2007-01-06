/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#ifndef _CONTINUATION_H_
#define _CONTINUATION_H_

#include <ucontext.h>

struct continuation
{
	char *stack;
	size_t stack_size;
	void (*entry)(struct continuation *cc);
	int (*release)(struct continuation *cc);

	/* private */
	ucontext_t uc;
	ucontext_t last;
	int exited;
};

int cc_init(struct continuation *cc);

int cc_release(struct continuation *cc);

/* you can use an uninitialized struct continuation for from if you do not have
   the current continuation handy. */
int cc_swap(struct continuation *from, struct continuation *to);

#define offset_of(type, member) ((unsigned long)(&((type *)0)->member))
#define container_of(obj, type, member) \
        (type *)(((char *)obj) - offset_of(type, member))

#endif
