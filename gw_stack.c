// SPDX-License-Identifier: GPL-2.0-only

#include "gw_stack.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

int gwftp_stack_init(struct gw_stack *st, uint32_t size)
{
	int err;

	err = pthread_mutex_init(&st->mutex, NULL);
	if (err)
		return -err;

	st->data = malloc(size * sizeof(uint32_t));
	if (!st->data)
		return -ENOMEM;

	st->sp = 0;
	st->bp = size;
	return 0;
}

int __gwftp_stack_push(struct gw_stack *st, uint32_t val)
{
	if (st->sp == st->bp)
		return -EAGAIN;

	st->data[st->sp++] = val;
	return 0;
}

int __gwftp_stack_pop(struct gw_stack *st, uint32_t *val)
{
	if (st->sp == 0)
		return -EAGAIN;

	*val = st->data[--st->sp];
	return 0;
}

int gwftp_stack_push(struct gw_stack *st, uint32_t val)
{
	int err;
	pthread_mutex_lock(&st->mutex);
	err = __gwftp_stack_push(st, val);
	pthread_mutex_unlock(&st->mutex);
	return err;
}

int gwftp_stack_pop(struct gw_stack *st, uint32_t *val)
{
	int err;
	pthread_mutex_lock(&st->mutex);
	err = __gwftp_stack_pop(st, val);
	pthread_mutex_unlock(&st->mutex);
	return err;
}

void gwftp_stack_free(struct gw_stack *st)
{
	free(st->data);
	pthread_mutex_destroy(&st->mutex);
	memset(st, 0, sizeof(*st));
}
