// SPDX-License-Identifier: GPL-2.0-only

#include "ev_epoll.h"
#include "gwftp.h"
#include "validator.h"

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
		err = epoll_mod(ep->eo.ep_fd, ctx->tcp_fd, EPOLLIN, data);
		if (err) {
			pr_err("Failed to disable accept event: %s\n",
				strerror(-err));
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
	int fd, err;

	fd = accept4(ctx->tcp_fd, (struct sockaddr *)&addr, &addrlen, flags);
	if (fd < 0)
		return handle_accept_err(-errno, ctx);

	cl = gwftp_server_get_client_slot(ctx);
	if (!cl) {
		close(fd);
		return handle_accept_err(-ENFILE, ctx);
	}

	cl->fd = fd;
	cl->addr = addr;
	cl->ep_mask = EPOLLIN;

	data.u64 = 0;
	data.ptr = cl;
	data.u64 |= SRV_EV_CLIENT;
	err = epoll_add(ep->eo.ep_fd, fd, cl->ep_mask, data);
	if (err) {
		close(fd);
		gwftp_server_put_client_slot(ctx, cl);
		pr_err("Failed to add client to epoll: %s\n", strerror(-err));
		return err;
	}

	return 0;
}

static int close_client(struct gwftp_server_ctx *ctx, struct gwftp_client *cl)
{
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;
	int err;

	err = epoll_del(ep->eo.ep_fd, cl->fd);
	if (err)
		pr_err("Failed to remove client from epoll: %s\n",
			strerror(-err));

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
		err = epoll_mod(ep->eo.ep_fd, ctx->tcp_fd, EPOLLIN, data);
		if (err)
			pr_err("Failed to enable accept event: %s\n",
				strerror(-err));

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

static int server_transmit_packet(struct gwftp_server_ctx *ctx,
				  struct gwftp_client *cl)
{
	struct gwftp_srv_ev_epoll *ep;
	union epoll_data data;
	ssize_t ret;
	size_t len;
	char *buf;

repeat:
	if (!cl->tx_len)
		goto out;

	buf = cl->tx_pkt.raw;
	len = cl->tx_len;
	ret = send(cl->fd, buf, len, MSG_DONTWAIT);
	if (ret < 0) {
		ret = -errno;
		if (ret != -EAGAIN && ret != -EINTR) {
			pr_err("Failed to send data: %s\n", strerror(-ret));
			return ret;
		}

		if (cl->ep_mask != EPOLLOUT) {
			cl->ep_mask = EPOLLOUT;
			goto out_epl_mod;
		}
		return 0;
	}

	if (!ret)
		return -ECONNRESET;

	cl->tx_len -= (size_t)ret;
	if (cl->tx_len) {
		memmove(buf, buf + ret, cl->tx_len);
		goto repeat;
	}

out:
	if (cl->ep_mask & EPOLLOUT) {
		cl->ep_mask = EPOLLIN;
		goto out_epl_mod;
	}

	return 0;

out_epl_mod:
	ep = ctx->ev_epoll;
	data.u64 = 0;
	data.ptr = cl;
	data.u64 |= SRV_EV_CLIENT;
	ep->break_epoll_iter = true;
	return epoll_mod(ep->eo.ep_fd, cl->fd, cl->ep_mask, data);
}

static ssize_t do_recv(int fd, char *buf, size_t *len, size_t max_len)
{
	ssize_t ret;

	ret = recv(fd, buf + *len, max_len - *len, MSG_DONTWAIT);
	if (ret < 0) {
		ret = -errno;
		if (ret != -EAGAIN && ret != -EINTR) {
			pr_err("Failed to receive data: %s\n", strerror(-ret));
			return ret;
		}
		return -EAGAIN;
	}

	if (!ret)
		return -ECONNRESET;

	*len += (size_t)ret;
	return 0;
}

static int server_handle_client_recv(struct gwftp_server_ctx *ctx,
				     struct gwftp_client *cl)
{
	struct gwftp_pkt *rx = &cl->rx_pkt;
	ssize_t ret;

	ret = do_recv(cl->fd, rx->raw, &cl->rx_len, sizeof(*rx));
	if (ret)
		goto out;

	while (1) {
		int serr;

		ret = gwftp_server_evaluate_client_packet(ctx, cl);
		if (cl->tx_len) {
			serr = server_transmit_packet(ctx, cl);
			if (serr)
				return serr;
		}

		if (ret)
			break;
	}

out:
	return (ret == -EAGAIN) ? 0 : ret;
}

static int server_handle_client_send(struct gwftp_server_ctx *ctx,
				     struct gwftp_client *cl)
{
	int serr;

	serr = server_transmit_packet(ctx, cl);
	if (serr)
		return serr;

	return 0;
}

static int server_handle_client(struct gwftp_server_ctx *ctx,
				struct gwftp_client *cl, uint32_t events)
{
	int ret;

	if (events & (EPOLLERR | EPOLLHUP))
		goto out_close;

	if (events & EPOLLIN) {
		ret = server_handle_client_recv(ctx, cl);
		if (ret)
			goto out_close;
	}

	if (events & EPOLLOUT) {
		ret = server_handle_client_send(ctx, cl);
		if (ret)
			goto out_close;
	}

	return 0;

out_close:
	close_client(ctx, cl);
	return 0;
}

static int server_handle_event(struct gwftp_server_ctx *ctx,
			       struct epoll_event *ee)
{
	uint64_t ev = EPL_GET_EV(ee->data.u64);
	ee->data.u64 = EPL_CLEAR_EV(ee->data.u64);

	switch (ev) {
	case SRV_EV_ACCEPT:
		return server_handle_accept(ctx);
	case SRV_EV_CLIENT:
		return server_handle_client(ctx, ee->data.ptr, ee->events);
	}

	return 0;
}

static int poll_events(struct epoll_obj *eo)
{
	int ret;

	ret = epoll_wait(eo->ep_fd, eo->events, eo->max_events, eo->timeout);
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
	struct epoll_event *events = ep->eo.events;
	int i, ret = 0;

	for (i = 0; i < nr_events; i++) {
		ret = server_handle_event(ctx, &events[i]);
		if (unlikely(ret))
			break;

		if (ep->break_epoll_iter) {
			ep->break_epoll_iter = false;
			break;
		}
	}

	return ret;
}

__cold
static int init_epoll_obj(struct epoll_obj *eo, uint32_t max_events)
{
	int ep;

	ep = epoll_create(max_events);
	if (ep < 0) {
		pr_err("Failed to create epoll: %s\n", strerror(-errno));
		return -errno;
	}

	eo->ep_fd = ep;
	eo->max_events = max_events;
	eo->events = calloc(max_events, sizeof(*eo->events));
	if (!eo->events) {
		close(ep);
		return -ENOMEM;
	}

	return 0;
}

static void free_epoll_obj(struct epoll_obj *eo)
{
	if (eo->ep_fd >= 0)
		close(eo->ep_fd);

	if (eo->events)
		free(eo->events);
}

__cold
static int server_init_epoll(struct gwftp_server_ctx *ctx)
{
	struct gwftp_srv_ev_epoll *ep = malloc(sizeof(*ep));
	union epoll_data data;
	uint32_t max;
	int ret;

	if (!ep)
		return -ENOMEM;

	if (ctx->max_clients + 2 < epoll_max_events)
		max = ctx->max_clients + 2;
	else
		max = epoll_max_events;

	ret = init_epoll_obj(&ep->eo, max);
	if (ret) {
		free(ep);
		return ret;
	}

	data.u64 = SRV_EV_ACCEPT;
	ret = epoll_add(ep->eo.ep_fd, ctx->tcp_fd, EPOLLIN, data);
	if (ret) {
		free_epoll_obj(&ep->eo);
		free(ep);
		return ret;
	}

	ep->eo.timeout = -1;
	ep->is_accept_disabled = false;
	ep->break_epoll_iter = false;
	ctx->ev_epoll = ep;
	return 0;
}

static void server_free_epoll(struct gwftp_server_ctx *ctx)
{
	struct gwftp_srv_ev_epoll *ep = ctx->ev_epoll;

	if (!ep)
		return;

	free_epoll_obj(&ep->eo);
	free(ep);
}

__hot
int gwftp_server_run_ev_epoll(struct gwftp_server_ctx *ctx)
{
	int ret;

	ret = server_init_epoll(ctx);
	if (ret)
		return ret;

	while (!ctx->should_stop) {
		ret = poll_events(&ctx->ev_epoll->eo);
		if (unlikely(ret < 0))
			break;

		ret = server_handle_events(ctx, ret);
		if (unlikely(ret))
			break;
	}

	server_free_epoll(ctx);
	return ret;
}

static int client_transmit_packet(struct gwftp_client_ctx *ctx)
{
	struct gwftp_pkt *tx = &ctx->tx_pkt;
	ssize_t ret;
	size_t len;

	len = ctx->tx_len;
	ret = send(ctx->tcp_fd, tx->raw, len, MSG_DONTWAIT);
	if (ret < 0) {
		ret = -errno;
		if (ret != -EAGAIN && ret != -EINTR) {
			pr_err("Failed to send data: %s\n", strerror(-ret));
			return ret;
		}

		return 0;
	}

	if (!ret)
		return -ECONNRESET;

	ctx->tx_len -= (size_t)ret;
	if (ctx->tx_len)
		memmove(tx->raw, tx->raw + ret, ctx->tx_len);

	return 0;
}

static int client_handle_server_recv(struct gwftp_client_ctx *ctx)
{
	struct gwftp_pkt *rx = &ctx->rx_pkt;
	ssize_t ret;

	ret = do_recv(ctx->tcp_fd, rx->raw, &ctx->rx_len, sizeof(*rx));
	if (ret)
		goto out;

	while (1) {
		int serr;

		ret = gwftp_client_evaluate_server_packet(ctx);
		if (ctx->tx_len) {
			serr = client_transmit_packet(ctx);
			if (serr)
				return serr;
		}

		if (ret)
			break;
	}

out:
	return (ret == -EAGAIN) ? 0 : ret;
}

static int client_handle_server_send(struct gwftp_client_ctx *ctx)
{
	return 0;
	(void)ctx;
}

static int client_handle_server(struct gwftp_client_ctx *ctx, uint32_t events)
{
	int ret;

	if (unlikely(events & (EPOLLERR | EPOLLHUP))) {
		pr_err("Server disconnected!");
		ctx->should_stop = true;
		return -ECONNRESET;
	}

	if (events & EPOLLIN) {
		ret = client_handle_server_recv(ctx);
		if (ret)
			return ret;
	}

	if (events & EPOLLOUT) {
		ret = client_handle_server_send(ctx);
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
	struct epoll_event *events = ep->eo.events;
	int i, ret = 0;

	for (i = 0; i < nr_events; i++) {
		ret = client_handle_event(ctx, &events[i]);
		if (unlikely(ret))
			break;
	}

	return ret;
}

__cold
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
		pr_err("Failed to send complete handshake: %s\n",
			strerror(-EIO));
		return -EIO;
	}

	ctx->state = GWFTP_CL_STATE_HANDSHAKE;
	return 0;
}

__cold
static int client_init_epoll(struct gwftp_client_ctx *ctx)
{
	struct gwftp_cli_ev_epoll *ep = malloc(sizeof(*ep));
	union epoll_data data;
	int err;

	if (!ep)
		return -ENOMEM;

	err = init_epoll_obj(&ep->eo, 2);
	if (err) {
		free(ep);
		return err;
	}

	data.u64 = CLI_EV_SERVER;
	err = epoll_add(ep->eo.ep_fd, ctx->tcp_fd, EPOLLIN, data);
	if (err) {
		free_epoll_obj(&ep->eo);
		free(ep);
		return err;
	}

	ep->eo.timeout = -1;
	ctx->ev_epoll = ep;
	return 0;
}

static void client_free_epoll(struct gwftp_client_ctx *ctx)
{
	struct gwftp_cli_ev_epoll *ep = ctx->ev_epoll;

	if (!ep)
		return;

	free_epoll_obj(&ep->eo);
	free(ep);
}

__hot
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
		ret = poll_events(&ctx->ev_epoll->eo);
		if (unlikely(ret < 0))
			break;

		ret = client_handle_events(ctx, ret);
		if (unlikely(ret))
			break;
	}

	client_free_epoll(ctx);
	return ret;
}
