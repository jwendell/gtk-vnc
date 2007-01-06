/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include <sys/types.h>
#include <sys/mman.h>

#include "coroutine.h"

int coroutine_release(struct coroutine *co)
{
	return cc_release(&co->cc);
}

static int _coroutine_release(struct continuation *cc)
{
	struct coroutine *co = container_of(cc, struct coroutine, cc);

	if (co->release) {
		int ret = co->release(co);
		if (ret < 0)
			return ret;
	}

	return munmap(cc->stack, cc->stack_size);
}

static void coroutine_trampoline(struct continuation *cc)
{
	struct coroutine *co = container_of(cc, struct coroutine, cc);
	co->data = co->entry(co->data);
}

int coroutine_init(struct coroutine *co)
{
	if (co->stack_size == 0)
		co->stack_size = 16 << 20;

	co->cc.stack_size = co->stack_size;
	co->cc.stack = mmap(0, co->stack_size,
			    PROT_READ | PROT_WRITE | PROT_EXEC,
			    MAP_SHARED | MAP_ANONYMOUS | MAP_GROWSDOWN,
			    -1, 0);
	if (co->cc.stack == MAP_FAILED)
		return -1;
	co->cc.entry = coroutine_trampoline;
	co->cc.release = _coroutine_release;
	co->exited = 0;

	return cc_init(&co->cc);
}

#if 0
static __thread struct coroutine system;
static __thread struct coroutine *current;
#else
static struct coroutine system;
static struct coroutine *current;
#endif

struct coroutine *coroutine_self(void)
{
	if (current == 0)
		current = &system;
	return current;
}

void *coroutine_swap(struct coroutine *from, struct coroutine *to, void *arg)
{
	int ret;

	to->caller = from;
	to->data = arg;
	current = to;
	ret = cc_swap(&from->cc, &to->cc);
	if (ret == 0)
		return from->data;
	else if (ret == 1) {
		to->exited = 1;
		return to->data;
	}

	return 0;
}

void *yieldto(struct coroutine *to, void *arg)
{
	return coroutine_swap(coroutine_self(), to, arg);
}

void *yield(void *arg)
{
	return yieldto(coroutine_self()->caller, arg);
}
