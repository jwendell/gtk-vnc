/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 or
 * later as published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include "config.h"

#if WITH_UCONTEXT
#include "continuation.h"
#else
#include <glib.h>
#endif

struct coroutine
{
	size_t stack_size;
	void *(*entry)(void *);
	int (*release)(struct coroutine *);

	/* read-only */
	int exited;

	/* private */
	struct coroutine *caller;
	void *data;

#if WITH_UCONTEXT
	struct continuation cc;
#else
	GThread *thread;
	gboolean runnable;
#endif
};

int coroutine_init(struct coroutine *co);

int coroutine_release(struct coroutine *co);

void *coroutine_swap(struct coroutine *from, struct coroutine *to, void *arg);

struct coroutine *coroutine_self(void);

void *coroutine_yieldto(struct coroutine *to, void *arg);

void *coroutine_yield(void *arg);

#endif
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
