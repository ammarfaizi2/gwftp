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

#define EPL_EV_MASK		(0xffffull << 48ull)
#define EPL_GET_EV(ev)		((ev) & EPL_EV_MASK)
#define EPL_CLEAR_EV(ev)	((ev) & ~EPL_EV_MASK)

enum {
	SRV_EV_ACCEPT	= (0x0001ull << 48ull),
	SRV_EV_CLIENT	= (0x0002ull << 48ull),
};

enum {
	CLI_EV_SERVER	= (0x0001ull << 48ull),
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

static int handle_accept_err(int err, struct gwftp_server_ctx *ctx)
{
	if (err == -EAGAIN || err == -EINTR)
		return 0;

	if (err == -EMFILE || err == -ENFILE) {
		struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;
		union epoll_data data;

		/*
		 * Aiee... we run out of file descriptors.
		 *
		 * Wait for at least one client to disconnect before
		 * accepting new clients.
		 */
		pr_info("Run out of file descriptors! Disabling accept event...");
		data.u64 = SRV_EV_ACCEPT;
		err = epoll_mod(ep->ep_fd, ctx->tcp_fd, EPOLLIN, data);
		if (err) {
			pr_err("Failed to disable accept event: %s\n", strerror(-err));
			return err;
		}

		ep->is_accept_disabled = true;
		return 0;
	}

	pr_err("Failed to accept client: %s\n", strerror(-err));
	return err;
}

static int server_handle_accept(struct gwftp_server_ctx *ctx)
{
	static const int flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	struct gwftp_client *cl;
	union epoll_data data;
	uint32_t idx;
	int fd, err;

	fd = accept4(ctx->tcp_fd, (struct sockaddr *)&addr, &addrlen, flags);
	if (fd < 0)
		return handle_accept_err(-errno, ctx);

	err = gwftp_stack_pop(&ctx->cl_stack, &idx);
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
	data.u64 |= SRV_EV_CLIENT;
	err = epoll_add(ep->ep_fd, fd, EPOLLIN, data);
	if (err) {
		close(fd);
		gwftp_stack_push(&ctx->cl_stack, idx);
		pr_err("Failed to add client to epoll: %s\n", strerror(-err));
		return err;
	}

	return 0;
}

static int close_client(struct gwftp_server_ctx *ctx, struct gwftp_client *cl)
{
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;
	int err;

	err = epoll_del(ep->ep_fd, cl->fd);
	if (err)
		pr_err("Failed to remove client from epoll: %s\n", strerror(-err));

	pr_dbg("Closing client (fd=%d)", cl->fd);
	close(cl->fd);
	cl->fd = -1;

	err = gwftp_stack_push(&ctx->cl_stack, cl - ctx->clients);
	if (err)
		pr_err("Failed to push client to stack: %s\n", strerror(-err));

	if (ep->is_accept_disabled) {
		/*
		 * We ran out of file descriptors and disabled accept event.
		 * Re-enable it now as we have just freed one.
		 */
		union epoll_data data;

		data.u64 = SRV_EV_ACCEPT;
		err = epoll_mod(ep->ep_fd, ctx->tcp_fd, EPOLLIN, data);
		if (err)
			pr_err("Failed to enable accept event: %s\n", strerror(-err));

		ep->is_accept_disabled = false;
		pr_info("Re-enabled accept event!");
	}

	/*
	 * Stop iterating over epoll events. They may be stale!
	 * We must recall epoll_wait() to get fresh events.
	 */
	ep->break_epoll_iter = true;
	return err;
}

static int server_handle_recv(struct gwftp_server_ctx *ctx, struct gwftp_client *cl)
{
	ssize_t ret;
	size_t len;
	char *buf;

	len = sizeof(cl->rx_pkt) - cl->rx_len;
	buf = (char *)&cl->rx_pkt + cl->rx_len;
	ret = recv(cl->fd, buf, len, MSG_DONTWAIT);
	if (ret < 0) {

		ret = -errno;
		if (ret != -EAGAIN && ret != -EINTR) {
			close_client(ctx, cl);
			pr_err("Failed to receive data: %s\n", strerror(-ret));
		}

		return 0;
	}

	if (!ret) {
		close_client(ctx, cl);
		return 0;
	}

	return 0;
}

static int server_handle_event(struct gwftp_server_ctx *ctx,
			       struct epoll_event *ee)
{
	uint64_t ev = EPL_GET_EV(ee->data.u64);
	uint64_t ev_data = EPL_CLEAR_EV(ee->data.u64);

	ee->data.u64 = ev_data;
	switch (ev) {
	case SRV_EV_ACCEPT:
		return server_handle_accept(ctx);
	case SRV_EV_CLIENT:
		return server_handle_recv(ctx, ee->data.ptr);
	}

	return 0;
}

int server_poll_events(struct gwftp_server_ctx *ctx)
{
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;
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
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;
	struct epoll_event *events = ep->events;
	int i, ret = 0;

	for (i = 0; i < nr_events; i++) {
		ret = server_handle_event(ctx, &events[i]);
		if (ret)
			break;

		if (ep->break_epoll_iter) {
			ep->break_epoll_iter = false;
			break;
		}
	}

	return ret;
}

static int server_init_epoll(struct gwftp_server_ctx *ctx)
{
	struct gwftp_srv_ev_epoll *ep = malloc(sizeof(*ep));
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
	ep->is_accept_disabled = false;
	ctx->ev_epoll = ep;

	return 0;
}

static void server_free_epoll(struct gwftp_server_ctx *ctx)
{
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;

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

static int client_poll_events(struct gwftp_client_ctx *ctx)
{
	struct gwftp_cli_ev_epoll *ep = ctx->ev_epoll;
	int ret;

	ret = epoll_wait(ep->ep_fd, ep->events, 2, ep->timeout);
	if (ret < 0) {
		ret = -errno;
		if (ret == -EINTR)
			return 0;

		pr_err("Failed to wait for events: %s\n", strerror(-ret));
		return ret;
	}

	return ret;
}

static int server_handle_server_recv(struct gwftp_client_ctx *ctx)
{
	ssize_t ret;
	size_t len;
	char *buf;

	buf = (char *)&ctx->rx_pkt + ctx->rx_len;
	len = sizeof(ctx->rx_pkt) - ctx->rx_len;
	ret = recv(ctx->tcp_fd, buf, len, MSG_DONTWAIT);
	if (ret < 0) {
		ret = -errno;
		if (ret != -EAGAIN && ret != -EINTR) {
			pr_err("Failed to receive data: %s\n", strerror(-ret));
			return ret;
		}

		return 0;
	}

	if (!ret) {
		pr_err("Server disconnected!");
		ctx->should_stop = true;
		return -ECONNRESET;
	}

	return 0;
}

static int server_handle_server_send(struct gwftp_client_ctx *ctx)
{
	return 0;
}

static int client_handle_server(struct gwftp_client_ctx *ctx, uint32_t events)
{
	int ret;

	if (events & (EPOLLERR | EPOLLHUP)) {
		pr_err("Server disconnected!");
		ctx->should_stop = true;
		return -ECONNRESET;
	}

	if (events & EPOLLIN) {
		ret = server_handle_server_recv(ctx);
		if (ret)
			return ret;
	}

	if (events & EPOLLOUT) {
		ret = server_handle_server_send(ctx);
		if (ret)
			return ret;
	}

	return 0;
}

static int client_handle_event(struct gwftp_client_ctx *ctx,
			       struct epoll_event *ee)
{
	uint64_t ev = EPL_GET_EV(ee->data.u64);
	uint64_t ev_data = EPL_CLEAR_EV(ee->data.u64);

	ee->data.u64 = ev_data;
	switch (ev) {
	case CLI_EV_SERVER:
		return client_handle_server(ctx, ee->events);
	}

	return 0;
}

static int client_handle_events(struct gwftp_client_ctx *ctx, int nr_events)
{
	struct gwftp_cli_ev_epoll *ep = ctx->ev_epoll;
	struct epoll_event *events = ep->events;
	int i, ret = 0;

	for (i = 0; i < nr_events; i++) {
		ret = client_handle_event(ctx, &events[i]);
		if (ret)
			break;
	}

	return ret;
}

static int client_send_handshake(struct gwftp_client_ctx *ctx)
{
	struct gwftp_pkt *tx = &ctx->tx_pkt;
	ssize_t ret;
	size_t len;

	len = prep_pkt_handshake(tx, GWFTP_VERSION_MAJOR, GWFTP_VERSION_MINOR,
				 GWFTP_VERSION_PATCH, GWFTP_VERSION_EXTRA);

	ret = send(ctx->tcp_fd, tx, len, 0);
	if (ret < 0) {
		ret = -errno;
		pr_err("Failed to send handshake: %s\n", strerror(-ret));
		return ret;
	}

	if ((size_t)ret != len) {
		pr_err("Failed to send complete handshake: %s\n", strerror(-EIO));
		return -EIO;
	}

	ctx->state = GWFTP_CL_STATE_HANDSHAKE;
	return 0;
}

static int client_init_epoll(struct gwftp_client_ctx *ctx)
{
	struct gwftp_cli_ev_epoll *ep = malloc(sizeof(*ep));
	union epoll_data data;
	int err;

	if (!ep)
		return -ENOMEM;

	ep->ep_fd = epoll_create(2);
	if (ep->ep_fd < 0) {
		err = -errno;
		free(ep);
		return err;
	}

	data.u64 = CLI_EV_SERVER;
	err = epoll_add(ep->ep_fd, ctx->tcp_fd, EPOLLIN, data);
	if (err) {
		close(ep->ep_fd);
		free(ep);
		return err;
	}

	ep->timeout = -1;
	ctx->ev_epoll = ep;

	return 0;
}

static void client_free_epoll(struct gwftp_client_ctx *ctx)
{
	struct gwftp_cli_ev_epoll *ep = ctx->ev_epoll;

	if (!ep)
		return;

	if (ep->ep_fd >= 0)
		close(ep->ep_fd);

	free(ep);
}

int gwftp_client_run_ev_epoll(struct gwftp_client_ctx *ctx)
{
	int ret;

	ctx->state = GWFTP_CL_STATE_INIT;
	ret = client_send_handshake(ctx);
	if (ret)
		return ret;

	ret = client_init_epoll(ctx);
	if (ret)
		return ret;

	while (!ctx->should_stop) {
		ret = client_poll_events(ctx);
		if (ret)
			break;

		ret = client_handle_events(ctx, ret);
		if (ret)
			break;
	}

	client_free_epoll(ctx);
	return ret;
}
