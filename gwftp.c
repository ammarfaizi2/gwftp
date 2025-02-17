// SPDX-License-Identifier: GPL-2.0-only

#include "gwftp.h"
#include "validator.h"

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
#include <poll.h>

static const uint16_t gwftp_default_port = 9921;
static const uint32_t gwftp_max_clients = 512;

static const char server_short_options[] = "a:p:r:e:hv";
static const struct option server_options[] = {
	{ "bind-addr",		required_argument,	NULL, 'a' },
	{ "bind-port",		required_argument,	NULL, 'p' },
	{ "root-dir",		required_argument,	NULL, 'r' },
	{ "event",		required_argument,	NULL, 'e' },
	{ "help",		no_argument,		NULL, 'h' },
	{ "version",		no_argument,		NULL, 'v' },
	{ NULL, 		0,			NULL, 0 }
};

static const char client_short_options[] = "a:p:e:hv";
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
		pr_err("Failed to resolve address: %s", gai_strerror(err));
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
		pr_err("Unsupported address family: %d", ss->ss_family);
		return -EAFNOSUPPORT;
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

	(void)signum;
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
	sa.sa_handler = SIG_IGN;
	err |= sigaction(SIGPIPE, &sa, NULL);
	if (err) {
		s_ctx = NULL;
		pr_err("Failed to setup signal handler: %s", strerror(errno));
		return -EOPNOTSUPP;
	}

	return 0;
}

static int server_init_client_slots(struct gwftp_server_ctx *ctx)
{
	uint32_t i;
	int err;

	ctx->max_clients = gwftp_max_clients;
	err = gwftp_stack_init(&ctx->cl_stack, ctx->max_clients);
	if (err) {
		pr_err("Failed to initialize stack: %s", strerror(-err));
		return err;
	}

	ctx->clients = calloc(gwftp_max_clients, sizeof(*ctx->clients));
	if (!ctx->clients) {
		gwftp_stack_free(&ctx->cl_stack);
		pr_err("Failed to allocate memory");
		return -ENOMEM;
	}

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

	memset(&addr, 0, sizeof(addr));
	err = resolve_addr(&addr, ctx->cfg.bind_addr, ctx->cfg.bind_port);
	if (err) {
		pr_err("Failed to resolve address: %s", strerror(-err));
		return err;
	}

	switch (addr.ss_family) {
	case AF_INET:
		addr_len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		addr_len = sizeof(struct sockaddr_in6);
		break;
	}

	if (ctx->cfg.event == GWFTP_EVENT_EPOLL)
		type |= SOCK_NONBLOCK;

	fd = socket(addr.ss_family, type, 0);
	if (fd < 0) {
		err = -errno;
		pr_err("Failed to create socket: %s", strerror(-err));
		return err;
	}

#ifdef SO_REUSEADDR
	err = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &err, sizeof(err));
#endif

	err = bind(fd, (struct sockaddr *)&addr, addr_len);
	if (err) {
		err = -errno;
		pr_err("Failed to bind socket: %s", strerror(-err));
		goto out_err;
	}

	err = listen(fd, 1024);
	if (err) {
		err = -errno;
		pr_err("Failed to listen on socket: %s", strerror(-err));
		goto out_err;
	}

	ctx->tcp_fd = fd;

	pr_info("Listening on %s:%hu...", ctx->cfg.bind_addr,
		ctx->cfg.bind_port);
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
		pr_err("Failed to change directory: %s", strerror(-err));
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
				pr_err("Failed to allocate memory");
				return -ENOMEM;
			}
			break;
		
		case 'p':
			cfg->bind_port = atoi(optarg);
			break;
		
		case 'r':
			cfg->root_dir = strdup(optarg);
			if (!cfg->root_dir) {
				pr_err("Failed to allocate memory");
				return -ENOMEM;
			}
			break;

		case 'e':
			if (!strcmp(optarg, "epoll")) {
				cfg->event = GWFTP_EVENT_EPOLL;
			} else if (!strcmp(optarg, "io_uring")) {
				cfg->event = GWFTP_EVENT_IO_URING;
			} else {
				pr_err("Unsupported event: %s", optarg);
				return -EINVAL;
			}
			break;

		case 'h':
			show_server_help(app);
			return -EINVAL;

		case 'v':
			printf("gwftp v%s\n", GWFTP_VERSION);
			exit(0);
			__builtin_unreachable();
		
		default:
			pr_err("Unknown option: %s", argv[optind - 1]);
			show_server_help(app);
			return -EINVAL;
		}
	}

	if (!cfg->bind_addr) {
		cfg->bind_addr = strdup("::");
		if (!cfg->bind_addr) {
			pr_err("Failed to allocate memory");
			return -ENOMEM;
		}
	}

	if (!cfg->root_dir) {
		cfg->root_dir = strdup(".");
		if (!cfg->root_dir) {
			pr_err("Failed to allocate memory");
			return -ENOMEM;
		}
	}

	return 0;
}

__cold
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
		pr_err("Unsupported event: io_uring");
		err = -EOPNOTSUPP;
		break;
	default:
		pr_err("Unsupported event: %d", ctx.cfg.event);
		err = -EOPNOTSUPP;
		break;
	}
out:
	server_free_ctx(&ctx);
	return err;
}

static struct gwftp_client_ctx *c_ctx;

static void client_sig_handler(int signum)
{
	if (c_ctx && !c_ctx->should_stop)
		c_ctx->should_stop = true;

	(void)signum;
}

static int client_setup_sigaction(struct gwftp_client_ctx *ctx)
{
	struct sigaction sa = {
		.sa_handler = client_sig_handler,
		.sa_flags = 0,
	};
	int err = 0;

	c_ctx = ctx;
	err |= sigaction(SIGINT, &sa, NULL);
	err |= sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	err |= sigaction(SIGPIPE, &sa, NULL);
	if (err) {
		c_ctx = NULL;
		pr_err("Failed to setup signal handler: %s", strerror(errno));
		return -EOPNOTSUPP;
	}

	return 0;
}

static int client_init_sock(struct gwftp_client_ctx *ctx)
{
	int err, fd, type = SOCK_STREAM;
	struct sockaddr_storage addr;
	socklen_t addr_len = 0;

	memset(&addr, 0, sizeof(addr));
	err = resolve_addr(&addr, ctx->cfg.server_addr, ctx->cfg.server_port);
	if (err) {
		pr_err("Failed to resolve address: %s", strerror(-err));
		return err;
	}

	switch (addr.ss_family) {
	case AF_INET:
		addr_len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		addr_len = sizeof(struct sockaddr_in6);
		break;
	}

	if (ctx->cfg.event == GWFTP_EVENT_EPOLL)
		type |= SOCK_NONBLOCK;

	fd = socket(addr.ss_family, type, 0);
	if (fd < 0) {
		err = -errno;
		pr_err("Failed to create socket: %s", strerror(-err));
		return err;
	}

	pr_info("Connecting to %s:%hu...", ctx->cfg.server_addr,
		ctx->cfg.server_port);
	err = connect(fd, (struct sockaddr *)&addr, addr_len);
	if (err) {
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };

		err = -errno;
		if (err != -EINPROGRESS) {
			pr_err("Failed to connect to server: %s", strerror(-err));
			close(fd);
			return err;
		}

		err = poll(&pfd, 1, 10000);
		if (err <= 0) {
			if (err == 0)
				err = -ETIMEDOUT;

			pr_err("Failed to connect to server: %s", strerror(-err));
			close(fd);
			return err;
		}
	}

	ctx->tcp_fd = fd;
	pr_info("Connected to %s:%hu...", ctx->cfg.server_addr,
		ctx->cfg.server_port);
	return 0;
}

static void client_free_sock(struct gwftp_client_ctx *ctx)
{
	if (ctx->tcp_fd >= 0) {
		close(ctx->tcp_fd);
		ctx->tcp_fd = -1;
	}
}

static int client_init_ctx(struct gwftp_client_ctx *ctx)
{
	int err;

	ctx->should_stop = false;
	ctx->tcp_fd = -1;
	ctx->last_cmd_id = 0;

	err = client_setup_sigaction(ctx);
	if (err)
		return err;

	err = client_init_sock(ctx);
	if (err)
		return err;

	return 0;
}

static void client_free_cfg(struct gwftp_client_cfg *cfg)
{
	free(cfg->server_addr);
}

static void client_free_ctx(struct gwftp_client_ctx *ctx)
{
	client_free_cfg(&ctx->cfg);
	client_free_sock(ctx);
}

static void show_client_help(const char *app)
{
	printf("Usage: %s client [options]\n", app);
	printf("Options:\n");
	printf("  -a, --server-addr=ADDR\tServer address\n");
	printf("  -p, --server-port=PORT\tServer port\n");
	printf("  -e, --event=EVENT\tEvent type (epoll, io_uring)\n");
	printf("  -h, --help\t\tDisplay this help message\n");
	printf("  -v, --version\t\tDisplay version\n");
}

static int client_parse_args(int argc, char *argv[], const char *app,
			     struct gwftp_client_cfg *cfg)
{
	int idx;

	cfg->server_port = gwftp_default_port;
	cfg->event = GWFTP_EVENT_EPOLL;

	while (1) {
		int c = getopt_long(argc, argv, client_short_options,
				    client_options, &idx);
		if (c == -1)
			break;

		switch (c) {
		case 'a':
			cfg->server_addr = strdup(optarg);
			if (!cfg->server_addr) {
				pr_err("Failed to allocate memory");
				return -ENOMEM;
			}
			break;

		case 'p':
			cfg->server_port = atoi(optarg);
			break;

		case 'e':
			if (!strcmp(optarg, "epoll")) {
				cfg->event = GWFTP_EVENT_EPOLL;
			} else if (!strcmp(optarg, "io_uring")) {
				cfg->event = GWFTP_EVENT_IO_URING;
			} else {
				pr_err("Unsupported event: %s", optarg);
				return -EINVAL;
			}
			break;

		case 'h':
			show_client_help(app);
			return -EINVAL;

		case 'v':
			printf("gwftp v%s\n", GWFTP_VERSION);
			exit(0);
			__builtin_unreachable();

		default:
			pr_err("Unknown option: %s", argv[optind - 1]);
			show_client_help(app);
			return -EINVAL;
		}
	}

	if (!cfg->server_addr) {
		pr_err("Server address is required");
		return -EINVAL;
	}

	return 0;
}

__cold
static int gwftp_client_run(int argc, char *argv[], const char *app)
{
	struct gwftp_client_ctx ctx;
	int err;

	memset(&ctx, 0, sizeof(ctx));
	err = client_parse_args(argc, argv, app, &ctx.cfg);
	if (err) {
		client_free_cfg(&ctx.cfg);
		return err;
	}

	err = client_init_ctx(&ctx);
	if (err)
		goto out;

	switch (ctx.cfg.event) {
	case GWFTP_EVENT_EPOLL:
		err = gwftp_client_run_ev_epoll(&ctx);
		break;
	case GWFTP_EVENT_IO_URING:
		pr_err("Unsupported event: io_uring");
		err = -EOPNOTSUPP;
		break;
	default:
		pr_err("Unsupported event: %d", ctx.cfg.event);
		err = -EOPNOTSUPP;
		break;
	}

out:
	client_free_ctx(&ctx);
	return err;
}

static int gwftp_server_handle_handshake(struct gwftp_client *cl)
{
	cl->state = GWFTP_SRV_CL_STATE_ESTABLISHED;
	cl->tx_len = prep_pkt_handshake_res(&cl->tx_pkt, 0,
					    GWFTP_VERSION_MAJOR,
					    GWFTP_VERSION_MINOR,
					    GWFTP_VERSION_PATCH,
					    GWFTP_VERSION_EXTRA);

	pr_dbg("Handshake completed!");
	return 0;
}

static int gwftp_server_consume_cl_packet(struct gwftp_server_ctx *ctx,
					  struct gwftp_client *cl)
{
	struct gwftp_pkt *pkt = &cl->rx_pkt;

	switch (pkt->hdr.type) {
	case GWFTP_PKT_TYPE_HANDSHAKE:
		assert(cl->state == GWFTP_SRV_CL_STATE_HANDSHAKE);
		return gwftp_server_handle_handshake(cl);
	default:
		pr_dbg("Unsupported packet type: 0x%02x", pkt->hdr.type);
		return -EINVAL;
	}
	return 0;

	(void)ctx;
}

__hot
int gwftp_server_evaluate_client_packet(struct gwftp_server_ctx *ctx,
					struct gwftp_client *cl)
{
	size_t len, expected_len;
	struct gwftp_pkt *pkt;
	int ret;

	pkt = &cl->rx_pkt;
	len = cl->rx_len;
	if (unlikely(len < sizeof(pkt->hdr)))
		return -EAGAIN;

	pkt->hdr.len = ntohs(pkt->hdr.len);
	ret = gwftp_server_validate_cl_pkt_hdr(cl);
	if (unlikely(ret))
		goto out;

	expected_len = sizeof(pkt->hdr) + pkt->hdr.len;
	if (len < expected_len) {
		ret = -EAGAIN;
		goto out;
	}

	ret = gwftp_server_validate_cl_pkt_body(cl);
	if (unlikely(ret))
		goto out;

	ret = gwftp_server_consume_cl_packet(ctx, cl);
	if (likely(!ret)) {
		cl->rx_len -= expected_len;
		memmove(cl->rx_pkt.raw, cl->rx_pkt.raw + expected_len, cl->rx_len);
		return 0;
	}

out:
	pkt->hdr.len = htons(pkt->hdr.len);
	return ret;
}

__hot
struct gwftp_client *gwftp_server_get_client_slot(struct gwftp_server_ctx *ctx)
{
	struct gwftp_client *cl;
	uint32_t idx;
	int err;

	err = gwftp_stack_pop(&ctx->cl_stack, &idx);
	if (err)
		return NULL;

	cl = &ctx->clients[idx];
	cl->fd = -1;
	cl->state = GWFTP_SRV_CL_STATE_HANDSHAKE;
	cl->rx_len = 0;
	cl->tx_len = 0;
	return cl;
}

__hot
int gwftp_server_put_client_slot(struct gwftp_server_ctx *ctx,
				 struct gwftp_client *cl)
{
	uint32_t idx = cl - ctx->clients;

	cl->fd = -1;
	cl->state = GWFTP_SRV_CL_STATE_INIT;
	cl->rx_len = 0;
	cl->tx_len = 0;
	return gwftp_stack_push(&ctx->cl_stack, idx);
}

static bool my_isspace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void trim_and_move(char *buf, size_t len)
{
	size_t i, j;

	for (i = 0; i < len; i++) {
		if (!my_isspace(buf[i]))
			break;
	}

	for (j = len - 1; j > i; j--) {
		if (!my_isspace(buf[j]))
			break;
	}

	memmove(buf, buf + i, j - i + 1);
	buf[j - i + 1] = '\0';
}

static ssize_t read_cmd_line(char *buf, size_t max_len)
{
	char *p;

	p = fgets(buf, max_len, stdin);
	if (!p) {
		putchar('\n');
		pr_info("Exiting...");
		return -EIO;
	}

	trim_and_move(buf, strlen(buf));
	return strlen(buf);
}

static int evaluate_cmd(struct gwftp_client_ctx *ctx)
{
	char *cmd = ctx->sh_buf;
	char *arg = strchr(cmd, ' ');

	if (arg) {
		*arg = '\0';
		arg++;
	}

	if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit") || !strcmp(cmd, "q")) {
		pr_info("Exiting...");
		ctx->should_stop = true;
		return 0;
	}

	if (!strcmp(cmd, "clear")) {
		printf("\ec");
		printf("\033[H\033[J");
		return 0;
	}

	if (!strcmp(cmd, "ls")) {
		ctx->tx_len = prep_pkt_cmd(&ctx->tx_pkt, GWFTP_CMD_LS,
					   ++ctx->last_cmd_id, 0, arg);
		return 0;
	}

	if (!strcmp(cmd, "help")) {
		printf("Commands:\n");
		printf("  clear\t\tClear the screen\n");
		printf("  exit\t\tExit the program\n");
		printf("  quit\t\tExit the program\n");
		printf("  ls\t\tList files in the current server directory\n");
		printf("  help\t\tDisplay this help message\n");
		return 0;
	}

	printf("Unknown command: %s\n", cmd);
	printf("Type 'help' for a list of commands\n");
	return 0;
}

static int gwftp_client_shell(struct gwftp_client_ctx *ctx)
{
	ssize_t ret;

	while (!ctx->should_stop) {
		printf("gwftp > ");
		fflush(stdout);

		ret = read_cmd_line(ctx->sh_buf, sizeof(ctx->sh_buf));
		if (ret < 0)
			break;

		ctx->sh_len = (size_t)ret;
		if (!ctx->sh_len)
			continue;

		ret = evaluate_cmd(ctx);
		if (ret)
			break;
	}

	return ret;
}

static int gwftp_client_handle_handshake(struct gwftp_client_ctx *ctx)
{
	int ret;

	ctx->state = GWFTP_CL_STATE_ESTABLISHED;
	pr_info("Connection established!");
	ret = gwftp_client_shell(ctx);
	return (ret < 0) ? ret : 0;
}

static int gwftp_client_consume_sr_packet(struct gwftp_client_ctx *ctx)
{
	struct gwftp_pkt *pkt = &ctx->rx_pkt;

	switch (pkt->hdr.type) {
	case GWFTP_PKT_TYPE_HANDSHAKE_RES:
		assert(ctx->state == GWFTP_CL_STATE_HANDSHAKE);
		return gwftp_client_handle_handshake(ctx);
	default:
		pr_dbg("Unsupported packet type: 0x%02x", pkt->hdr.type);
		return -EINVAL;
	}
	return 0;
}

__hot
int gwftp_client_evaluate_server_packet(struct gwftp_client_ctx *ctx)
{
	struct gwftp_pkt *pkt = &ctx->rx_pkt;
	size_t len, expected_len;
	int ret;

	len = ctx->rx_len;
	if (unlikely(len < sizeof(pkt->hdr)))
		return -EAGAIN;

	pkt->hdr.len = ntohs(pkt->hdr.len);
	ret = gwftp_client_validate_sr_pkt_hdr(ctx);
	if (unlikely(ret))
		goto out;

	expected_len = sizeof(pkt->hdr) + pkt->hdr.len;
	if (len < expected_len) {
		ret = -EAGAIN;
		goto out;
	}

	ret = gwftp_client_validate_sr_pkt_body(ctx);
	if (unlikely(ret))
		goto out;

	ret = gwftp_client_consume_sr_packet(ctx);
	if (likely(!ret)) {
		ctx->rx_len -= expected_len;
		memmove(ctx->rx_pkt.raw, ctx->rx_pkt.raw + expected_len, ctx->rx_len);
		return 0;
	}

out:
	pkt->hdr.len = htons(pkt->hdr.len);
	return ret;
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
