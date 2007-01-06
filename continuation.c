/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include "continuation.h"

int cc_init(struct continuation *cc)
{
	if (getcontext(&cc->uc) == -1)
		return -1;

	cc->uc.uc_link = &cc->last;
	cc->uc.uc_stack.ss_sp = cc->stack;
	cc->uc.uc_stack.ss_size = cc->stack_size;
	cc->uc.uc_stack.ss_flags = 0;

	makecontext(&cc->uc, (void *)cc->entry, 1, cc);

	return 0;
}

int cc_release(struct continuation *cc)
{
	if (cc->release)
		return cc->release(cc);

	return 0;
}

int cc_swap(struct continuation *from, struct continuation *to)
{
	to->exited = 0;
	if (getcontext(&to->last) == -1)
		return -1;
	else if (to->exited == 0)
		to->exited = 1;
	else if (to->exited == 1)
		return 1;

	swapcontext(&from->uc, &to->uc);

	return 0;
}
