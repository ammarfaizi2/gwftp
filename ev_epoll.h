// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__EV_EPOLL_H
#define GWFTP__EV_EPOLL_H

#include <sys/epoll.h>

struct gwftp_ev_epoll {
	int			ep_fd;
	int			ev_fd;
	int			timeout;
	uint32_t		max_events;
	struct epoll_event	*events;
};

struct gwftp_server_ctx;

int gwftp_server_run_ev_epoll(struct gwftp_server_ctx *ctx);

#endif /* #ifndef GWFTP__EV_EPOLL_H */
