// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__EV_EPOLL_H
#define GWFTP__EV_EPOLL_H

#include <sys/epoll.h>
#include <stdbool.h>

struct gwftp_srv_ev_epoll {
	int			ep_fd;
	int			ev_fd;
	int			timeout;
	uint32_t		max_events;
	struct epoll_event	*events;
	bool			is_accept_disabled;
	bool			break_epoll_iter;
};

struct gwftp_cli_ev_epoll {
	int			ep_fd;
	int			ev_fd;
	int			timeout;
	uint32_t		max_events;
	struct epoll_event	events[2];
};

struct gwftp_server_ctx;
struct gwftp_client_ctx;

int gwftp_server_run_ev_epoll(struct gwftp_server_ctx *ctx);
int gwftp_client_run_ev_epoll(struct gwftp_client_ctx *ctx);

#endif /* #ifndef GWFTP__EV_EPOLL_H */
