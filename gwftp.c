// SPDX-License-Identifier: GPL-2.0-only

#include "gwftp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>

static const uint16_t gwftp_default_port = 9921;
static const uint32_t gwftp_max_clients = 512;

static const char server_short_options[] = "a:p:w:e:h";
static const struct option server_options[] = {
	{ "bind-addr",		required_argument,	NULL, 'a' },
	{ "bind-port",		required_argument,	NULL, 'p' },
	{ "root-dir",		required_argument,	NULL, 'r' },
	{ "event",		required_argument,	NULL, 'e' },
	{ "help",		no_argument,		NULL, 'h' },
	{ "version",		no_argument,		NULL, 'v' },
	{ NULL, 		0,			NULL, 0 }
};

static const char client_short_options[] = "a:p:e:h";
static const struct option client_options[] = {
	{ "server-addr",	required_argument,	NULL, 'a' },
	{ "server-port",	required_argument,	NULL, 'p' },
	{ "event",		required_argument,	NULL, 'e' },
	{ "help",		no_argument,		NULL, 'h' },
	{ "version",		no_argument,		NULL, 'v' },
	{ NULL,			0,			NULL, 0 }
};

static int resolve_addr(struct sockaddr_storage *ss, const char *addr,
			uint16_t port)
{
	struct addrinfo hints, *res;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	err = getaddrinfo(addr, NULL, &hints, &res);
	if (err) {
		pr_err("Failed to resolve address: %s\n", gai_strerror(err));
		return -err;
	}

	memcpy(ss, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	switch (ss->ss_family) {
	case AF_INET:
		((struct sockaddr_in *)ss)->sin_port = htons(port);
		break;
	case AF_INET6:
		((struct sockaddr_in6 *)ss)->sin6_port = htons(port);
		break;
	default:
		pr_err("Unsupported address family: %d\n", ss->ss_family);
		return -EINVAL;
	}

	return 0;
}

static struct gwftp_server_ctx *s_ctx;

static void server_sig_handler(int signum)
{
	if (s_ctx && !s_ctx->should_stop) {
		putchar('\n');
		s_ctx->should_stop = true;
	}
}

static int server_setup_sigaction(struct gwftp_server_ctx *ctx)
{
	struct sigaction sa = {
		.sa_handler = server_sig_handler,
		.sa_flags = 0,
	};
	int err = 0;

	s_ctx = ctx;
	err |= sigaction(SIGINT, &sa, NULL);
	err |= sigaction(SIGTERM, &sa, NULL);
	err |= sigaction(SIGQUIT, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	err |= sigaction(SIGPIPE, &sa, NULL);
	if (err) {
		s_ctx = NULL;
		pr_err("Failed to setup signal handler: %s\n", strerror(errno));
		return -EOPNOTSUPP;
	}

	return 0;
}

static int server_init_client_slots(struct gwftp_server_ctx *ctx)
{
	uint32_t i;
	int err;

	err = gwftp_stack_init(&ctx->cl_stack, ctx->max_clients);
	if (err) {
		pr_err("Failed to initialize stack: %s\n", strerror(-err));
		return err;
	}

	ctx->clients = calloc(gwftp_max_clients, sizeof(*ctx->clients));
	if (!ctx->clients) {
		gwftp_stack_free(&ctx->cl_stack);
		pr_err("Failed to allocate memory\n");
		return -ENOMEM;
	}

	ctx->max_clients = gwftp_max_clients;

	i = ctx->max_clients;
	while (i--)
		__gwftp_stack_push(&ctx->cl_stack, i);

	return 0;
}

static void server_free_client_slots(struct gwftp_server_ctx *ctx)
{
	if (ctx->clients) {
		free(ctx->clients);
		ctx->clients = NULL;
	}

	gwftp_stack_free(&ctx->cl_stack);
}

static int server_init_sock(struct gwftp_server_ctx *ctx)
{
	int err, fd, type = SOCK_STREAM;
	struct sockaddr_storage addr;
	socklen_t addr_len = 0;

	if (ctx->cfg.event == GWFTP_EVENT_EPOLL)
		type |= SOCK_NONBLOCK;

	fd = socket(AF_INET6, type, 0);
	if (fd < 0) {
		err = -errno;
		pr_err("Failed to create socket: %s\n", strerror(-err));
		return err;
	}

	memset(&addr, 0, sizeof(addr));
	err = resolve_addr(&addr, ctx->cfg.bind_addr, ctx->cfg.bind_port);
	if (err)
		goto out_err;

	switch (addr.ss_family) {
	case AF_INET:
		addr_len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		addr_len = sizeof(struct sockaddr_in6);
		break;
	}

	err = bind(fd, (struct sockaddr *)&addr, addr_len);
	if (err) {
		err = -errno;
		pr_err("Failed to bind socket: %s\n", strerror(-err));
		goto out_err;
	}

	err = listen(fd, 1024);
	if (err) {
		err = -errno;
		pr_err("Failed to listen on socket: %s\n", strerror(-err));
		goto out_err;
	}

	ctx->tcp_fd = fd;
	return 0;

out_err:
	close(fd);
	return err;
}

static void server_free_sock(struct gwftp_server_ctx *ctx)
{
	if (ctx->tcp_fd >= 0) {
		close(ctx->tcp_fd);
		ctx->tcp_fd = -1;
	}
}

static int server_init_ctx(struct gwftp_server_ctx *ctx)
{
	int err;

	ctx->should_stop = false;
	ctx->tcp_fd = -1;

	err = server_setup_sigaction(ctx);
	if (err)
		return err;

	err = chdir(ctx->cfg.root_dir);
	if (err) {
		err = -errno;
		pr_err("Failed to change directory: %s\n", strerror(-err));
		return err;
	}

	err = server_init_client_slots(ctx);
	if (err)
		return err;

	err = server_init_sock(ctx);
	if (err) {
		server_free_client_slots(ctx);
		return err;
	}

	return 0;
}

static void server_free_cfg(struct gwftp_server_cfg *cfg)
{
	free(cfg->root_dir);
	free(cfg->bind_addr);
}

static void server_free_ctx(struct gwftp_server_ctx *ctx)
{
	server_free_cfg(&ctx->cfg);
	server_free_client_slots(ctx);
	server_free_sock(ctx);
}

static void show_server_help(const char *app)
{
	printf("Usage: %s server [options]\n", app);
	printf("Options:\n");
	printf("  -a, --bind-addr=ADDR\t\tBind address\n");
	printf("  -p, --bind-port=PORT\t\tBind port\n");
	printf("  -r, --root-dir=DIR\t\tRoot directory\n");
	printf("  -e, --event=EVENT\t\tEvent type (epoll, io_uring)\n");
	printf("  -h, --help\t\t\tDisplay this help message\n");
	printf("  -v, --version\t\t\tDisplay version\n");
}

static int server_parse_args(int argc, char *argv[],const char *app, 
			     struct gwftp_server_cfg *cfg)
{
	int idx;

	cfg->bind_port = gwftp_default_port;
	cfg->event = GWFTP_EVENT_EPOLL;

	while (1) {
		int c = getopt_long(argc, argv, server_short_options,
				    server_options, &idx);
		if (c == -1)
			break;

		switch (c) {
		case 'a':
			cfg->bind_addr = strdup(optarg);
			if (!cfg->bind_addr) {
				pr_err("Failed to allocate memory\n");
				return -ENOMEM;
			}
			break;
		
		case 'p':
			cfg->bind_port = atoi(optarg);
			break;
		
		case 'r':
			cfg->root_dir = strdup(optarg);
			if (!cfg->root_dir) {
				pr_err("Failed to allocate memory\n");
				return -ENOMEM;
			}
			break;

		case 'e':
			if (!strcmp(optarg, "epoll")) {
				cfg->event = GWFTP_EVENT_EPOLL;
			} else if (!strcmp(optarg, "io_uring")) {
				cfg->event = GWFTP_EVENT_IO_URING;
			} else {
				pr_err("Unsupported event: %s\n", optarg);
				return -EINVAL;
			}
			break;

		case 'h':
			show_server_help(app);
			return -EINVAL;

		case 'v':
			printf("gwftp v0.1\n");
			exit(0);
			__builtin_unreachable();
		
		default:
			pr_err("Unknown option: %s\n", argv[optind - 1]);
			show_server_help(app);
			return -EINVAL;
		}
	}

	if (!cfg->bind_addr) {
		cfg->bind_addr = strdup("::");
		if (!cfg->bind_addr) {
			pr_err("Failed to allocate memory\n");
			return -ENOMEM;
		}
	}

	if (!cfg->root_dir) {
		cfg->root_dir = strdup(".");
		if (!cfg->root_dir) {
			pr_err("Failed to allocate memory\n");
			return -ENOMEM;
		}
	}

	return 0;
}

static int gwftp_server_run(int argc, char *argv[], const char *app)
{
	struct gwftp_server_ctx ctx;
	int err;

	err = server_parse_args(argc, argv, app, &ctx.cfg);
	if (err) {
		server_free_cfg(&ctx.cfg);
		return err;
	}

	err = server_init_ctx(&ctx);
	if (err)
		goto out;

	switch (ctx.cfg.event) {
	case GWFTP_EVENT_EPOLL:
		err = gwftp_server_run_ev_epoll(&ctx);
		break;
	case GWFTP_EVENT_IO_URING:
		pr_err("Unsupported event: io_uring\n");
		err = -EOPNOTSUPP;
		break;
	default:
		pr_err("Unsupported event: %d\n", ctx.cfg.event);
		err = -EOPNOTSUPP;
		break;
	}
out:
	server_free_ctx(&ctx);
	return err;
}

static int gwftp_client_run(int argc, char *argv[], const char *app)
{
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 1) {
		printf("Usage: %s [server|client] [options]\n", argv[0]);
		return EINVAL;
	}

	if (!strcmp(argv[1], "server"))
		return -gwftp_server_run(argc - 1, argv + 1, argv[0]);

	if (!strcmp(argv[1], "client"))
		return -gwftp_client_run(argc - 1, argv + 1, argv[0]);

	printf("Unknown mode: %s\n", argv[1]);
	printf("Usage: %s [server|client] [options]\n", argv[0]);
	return EINVAL;
}
