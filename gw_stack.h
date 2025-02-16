// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__GWSTACK_H
#define GWFTP__GWSTACK_H

#include <pthread.h>
#include <stdint.h>

struct gw_stack {
	uint32_t		sp;
	uint32_t		bp;
	uint32_t		*data;
	pthread_mutex_t		mutex;
};

int gwftp_stack_init(struct gw_stack *st, uint32_t size);
int __gwftp_stack_push(struct gw_stack *st, uint32_t val);
int __gwftp_stack_pop(struct gw_stack *st, uint32_t *val);
int gwftp_stack_push(struct gw_stack *st, uint32_t val);
int gwftp_stack_pop(struct gw_stack *st, uint32_t *val);
void gwftp_stack_free(struct gw_stack *st);

#endif /* #ifndef GWFTP__GWSTACK_H */
