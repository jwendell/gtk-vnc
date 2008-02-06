/*
 * Copyright (C) 2007  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 or
 * later as published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>

static GCond *run_cond;
static GMutex *run_lock;
static struct coroutine *current;
static struct coroutine leader;

static void coroutine_system_init(void)
{
	if (!g_thread_supported())
		g_thread_init(NULL);

	run_cond = g_cond_new();
	run_lock = g_mutex_new();

	/* The thread that creates the first coroutine is the system coroutine
	 * so let's fill out a structure for it */
	leader.entry = NULL;
	leader.release = NULL;
	leader.stack_size = 0;
	leader.exited = 0;
	leader.thread = g_thread_self();
	leader.runnable = TRUE; /* we're the one running right now */
	leader.caller = NULL;
	leader.data = NULL;

	current = &leader;
}

static gpointer coroutine_thread(gpointer opaque)
{
	struct coroutine *co = opaque;

	g_mutex_lock(run_lock);
	while (!co->runnable)
		g_cond_wait(run_cond, run_lock);

	current = co;
	co->data = co->entry(co->data);
	co->exited = 1;

	co->caller->runnable = TRUE;
	g_cond_broadcast(run_cond);
	g_mutex_unlock(run_lock);

	return NULL;
}

int coroutine_init(struct coroutine *co)
{
	if (run_cond == NULL)
		coroutine_system_init();
	
	co->thread = g_thread_create_full(coroutine_thread, co, co->stack_size,
					  FALSE, TRUE,
					  G_THREAD_PRIORITY_NORMAL,
					  NULL);
	if (co->thread == NULL)
		return -1;

	co->exited = 0;
	co->runnable = FALSE;
	co->caller = NULL;

	return 0;
}

int coroutine_release(struct coroutine *co)
{
	return 0;
}

void *coroutine_swap(struct coroutine *from, struct coroutine *to, void *arg)
{
	from->runnable = FALSE;
	to->runnable = TRUE;
	to->data = arg;
	to->caller = from;
	g_cond_broadcast(run_cond);
	g_mutex_unlock(run_lock);

	g_mutex_lock(run_lock);
	while (!from->runnable)
		g_cond_wait(run_cond, run_lock);

	current = from;

	return from->data;
}

struct coroutine *coroutine_self(void)
{
	return current;
}

void *coroutine_yieldto(struct coroutine *to, void *arg)
{
	if (to->caller) {
		fprintf(stderr, "Co-routine is re-entering itself\n");
		abort();
	}
	return coroutine_swap(coroutine_self(), to, arg);
}

void *coroutine_yield(void *arg)
{
	struct coroutine *to = coroutine_self()->caller;
	if (!to) {
		fprintf(stderr, "Co-routine is yielding to no one\n");
		abort();
	}
	coroutine_self()->caller = NULL;
	return coroutine_swap(coroutine_self(), to, arg);
}

