// SPDX-License-Identifier: GPL-2.0-only

#include "ev_epoll.h"
#include "gwftp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/epoll.h>
#include <unistd.h>
#include <sys/socket.h>

static const uint32_t epoll_max_events = 128;

#define EPL_EV_MASK (0xffffull << 48ull)
#define EPL_GET_EV(ev) ((ev) & EPL_EV_MASK)
#define EPL_CLEAR_EV(ev) ((ev) & ~EPL_EV_MASK)

enum {
	SRV_EV_ACCEPT		= (0x0001ull << 48ull),
	SRV_EV_RECV		= (0x0002ull << 48ull),
};

static int epoll_add(int ep_fd, int fd, uint32_t events, union epoll_data data)
{
	struct epoll_event ev = {
		.events = events,
		.data = data,
	};
	int err;

	err = epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &ev);
	if (err < 0)
		return -errno;

	return 0;
}

static int epoll_del(int ep_fd, int fd)
{
	int err;

	err = epoll_ctl(ep_fd, EPOLL_CTL_DEL, fd, NULL);
	if (err < 0)
		return -errno;

	return 0;
}

static int epoll_mod(int ep_fd, int fd, uint32_t events, union epoll_data data)
{
	struct epoll_event ev = {
		.events = events,
		.data = data,
	};
	int err;

	err = epoll_ctl(ep_fd, EPOLL_CTL_MOD, fd, &ev);
	if (err < 0)
		return -errno;

	return 0;
}

static int server_accept_client(struct gwftp_server_ctx *ctx)
{
	static const int flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	struct gwftp_client *cl;
	union epoll_data data;
	uint32_t idx;
	int fd, err;

	fd = accept4(ctx->tcp_fd, (struct sockaddr *)&addr, &addrlen, flags);
	if (fd < 0) {
		err = -errno;
		if (err == -EAGAIN || err == -EINTR)
			return 0;

		pr_err("Failed to accept client: %s\n", strerror(-err));
		return err;
	}

	err = gwftp_stack_pop(&ctx->clients, &idx);
	if (err) {
		close(fd);
		pr_err("Failed to pop client from stack: %s\n", strerror(-err));
		return err;
	}

	cl = &ctx->clients[idx];
	cl->fd = fd;
	cl->addr = addr;
	cl->rx_len = 0;
	cl->tx_len = 0;

	data.u64 = 0;
	data.ptr = cl;
	data.u64 |= SRV_EV_RECV;
	err = epoll_add(ctx->ev_epoll->ep_fd, fd, EPOLLIN, data);
	if (err) {
		close(fd);
		gwftp_stack_push(&ctx->clients, idx);
		pr_err("Failed to add client to epoll: %s\n", strerror(-err));
		return err;
	}

	return 0;
}

static int server_handle_event(struct gwftp_server_ctx *ctx,
			       struct epoll_event *ee)
{
	uint64_t ev = EPL_GET_EV(ee->data.u64);

	switch (ev) {
	case SRV_EV_ACCEPT:
		return server_accept_client(ctx);
	}

	return 0;
}

int server_poll_events(struct gwftp_server_ctx *ctx)
{
	struct gwftp_ev_epoll *ep = ctx->ev_epoll;
	int ret;

	ret = epoll_wait(ep->ep_fd, ep->events, ep->max_events, ep->timeout);
	if (ret < 0) {
		ret = -errno;
		if (ret == -EINTR)
			return 0;

		pr_err("Failed to wait for events: %s\n", strerror(-ret));
		return ret;
	}

	return ret;
}

static int server_handle_events(struct gwftp_server_ctx *ctx, int nr_events)
{
	struct gwftp_ev_epoll *ep = ctx->ev_epoll;
	struct epoll_event *events = ep->events;
	int i, ret = 0;

	for (i = 0; i < nr_events; i++) {
		ret = server_handle_event(ctx, &events[i]);
		if (ret)
			break;
	}

	return ret;
}

static int server_init_epoll(struct gwftp_server_ctx *ctx)
{
	struct gwftp_ev_epoll *ep = malloc(sizeof(*ep));
	union epoll_data data;
	uint32_t max;
	int err;

	if (!ep)
		return -ENOMEM;

	ep->events = NULL;
	ep->ep_fd = epoll_create(128);
	if (ep->ep_fd < 0) {
		err = -errno;
		free(ep);
		return err;
	}

	data.u64 = SRV_EV_ACCEPT;
	err = epoll_add(ep->ep_fd, ctx->tcp_fd, EPOLLIN, data);
	if (err) {
		close(ep->ep_fd);
		free(ep);
		return err;
	}

	if (ctx->max_clients < epoll_max_events)
		max = ctx->max_clients;
	else
		max = epoll_max_events;

	ep->events = malloc(max * sizeof(*ep->events));
	if (!ep->events) {
		close(ep->ep_fd);
		free(ep);
		return -ENOMEM;
	}

	ep->max_events = max;
	ep->timeout = -1;
	ctx->ev_epoll = ep;

	return 0;
}

static void server_free_epoll(struct gwftp_server_ctx *ctx)
{
	struct gwftp_ev_epoll *ep = ctx->ev_epoll;

	if (!ep)
		return;

	if (ep->events)
		free(ep->events);

	if (ep->ep_fd >= 0)
		close(ep->ep_fd);

	free(ep);
}

int gwftp_server_run_ev_epoll(struct gwftp_server_ctx *ctx)
{
	int ret;

	ret = server_init_epoll(ctx);
	if (ret)
		return ret;

	while (!ctx->should_stop) {
		ret = server_poll_events(ctx);
		if (ret)
			break;

		ret = server_handle_events(ctx, ret);
		if (ret)
			break;
	}

	server_free_epoll(ctx);
	return ret;
}
