// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__GWFTP_H
#define GWFTP__GWFTP_H

#include "ev_epoll.h"
#include "gw_stack.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <linux/types.h>
#include <netinet/in.h>

#define pr_err(fmt, ...)	fprintf(stderr, "err  : " fmt "\n", ##__VA_ARGS__)
#define pr_dbg(fmt, ...)	fprintf(stderr, "dbg  : " fmt "\n", ##__VA_ARGS__)
#define pr_info(fmt, ...)	fprintf(stdout, "info : " fmt "\n", ##__VA_ARGS__)

#ifndef likely
#define likely(x)	__builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x)	__builtin_expect(!!(x), 0)
#endif

#ifndef __hot
#define __hot		__attribute__((__hot__))
#endif

#ifndef __cold
#define __cold		__attribute__((__cold__))
#endif

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#ifndef offsetof
#define offsetof(type, member)	__builtin_offsetof(type, member)
#endif

typedef uint8_t u8;

#define GWFTP_PATH_MAX 4096
#define GWFTP_HANDSHAKE_MAGIC 0xaabbccdd

enum {
	GWFTP_PKT_TYPE_HANDSHAKE	= 0x01,
	GWFTP_PKT_TYPE_HANDSHAKE_RESP	= 0x02,
	GWFTP_PKT_TYPE_CMD		= 0x03,
	GWFTP_PKT_TYPE_CMD_RESP		= 0x04,
	GWFTP_PKT_TYPE_CLOSE		= 0x05,
};

struct gwftp_pkt_hdr {
	u8	type;
	u8	resv;
	__be16	len;
} __packed;

struct gwftp_pkt_handshake {
	__be32	magic;
	u8	major;
	u8	minor;
	u8	patch;
	u8	extra[29];
} __packed;

struct gwftp_pkt_handshake_res {
	__be32	magic;
	u8	status;
	u8	major;
	u8	minor;
	u8	patch;
	u8	extra[29];
	u8	resv[3];
} __packed;

enum {
	GWFTP_CMD_LS		= 0x01,
	GWFTP_CMD_CD		= 0x02,
	GWFTP_CMD_DOWNLOAD	= 0x02,
	GWFTP_CMD_UPLOAD	= 0x03,
	GWFTP_CMD_PWD		= 0x04,
	GWFTP_CMD_RM		= 0x05,
	GWFTP_CMD_MV		= 0x06,
	GWFTP_CMD_CP		= 0x07,
	GWFTP_CMD_MKDIR		= 0x08,
	GWFTP_CMD_RMDIR		= 0x09,
};

enum {
	GWFTP_CMD_FLAGS_RECURSIVE	= (1ull << 0ull),
	GWFTP_CMD_FLAGS_FORCE		= (1ull << 1ull),
};

struct gwftp_pkt_cmd {
	__be64	id;
	__be64	flags;
	__be16	arg_len;
	u8	cmd;
	u8	arg[GWFTP_PATH_MAX];
} __packed;

struct gwftp_pkt_cmd_res {
	__be64	id;
	u8	is_end_of_res;
	u8	cmd;
} __packed;

struct gwftp_pkt {
	union {
		struct {
			struct gwftp_pkt_hdr	hdr;

			union {
				struct gwftp_pkt_handshake	hs;
				struct gwftp_pkt_handshake_res	hs_res;
				struct gwftp_pkt_cmd		cmd;
				struct gwftp_pkt_cmd_res	cmd_res;
			};
		};

		char	raw[8192];
	};
} __packed;

static inline size_t prep_pkt(struct gwftp_pkt *pkt, u8 type, size_t len)
{
	assert(len <= UINT16_MAX);

	pkt->hdr.type = type;
	pkt->hdr.resv = 0;
	pkt->hdr.len = htons(len);
	return sizeof(pkt->hdr) + len;
}

static inline size_t prep_pkt_handshake(struct gwftp_pkt *pkt, u8 major,
					u8 minor, u8 patch, const char *extra)
{
	pkt->hs.magic = htonl(GWFTP_HANDSHAKE_MAGIC);
	pkt->hs.major = major;
	pkt->hs.minor = minor;
	pkt->hs.patch = patch;
	strncpy((char *)pkt->hs.extra, extra, sizeof(pkt->hs.extra));
	return prep_pkt(pkt, GWFTP_PKT_TYPE_HANDSHAKE, sizeof(pkt->hs));
}

static inline size_t prep_pkt_handshake_res(struct gwftp_pkt *pkt, u8 status,
					    u8 major, u8 minor, u8 patch,
					    const char *extra)
{
	pkt->hs_res.magic = htonl(GWFTP_HANDSHAKE_MAGIC);
	pkt->hs_res.status = status;
	pkt->hs_res.major = major;
	pkt->hs_res.minor = minor;
	pkt->hs_res.patch = patch;
	strncpy((char *)pkt->hs_res.extra, extra, sizeof(pkt->hs_res.extra));
	return prep_pkt(pkt, GWFTP_PKT_TYPE_HANDSHAKE_RESP, sizeof(pkt->hs_res));
}

static inline size_t prep_pkt_cmd(struct gwftp_pkt *pkt, u8 cmd, __be64 id,
				  __be64 flags, const char *arg)
{
	size_t len = strlen(arg);
	size_t pkt_len = offsetof(struct gwftp_pkt_cmd, arg) + len;

	pkt->cmd.id = id;
	pkt->cmd.flags = flags;
	pkt->cmd.arg_len = htons(len);
	pkt->cmd.cmd = cmd;
	strncpy((char *)pkt->cmd.arg, arg, sizeof(pkt->cmd.arg));
	return prep_pkt(pkt, GWFTP_PKT_TYPE_CMD, pkt_len);
}

static inline size_t prep_pkt_cmd_res(struct gwftp_pkt *pkt, u8 cmd, __be64 id,
				      u8 is_end_of_res)
{
	pkt->cmd_res.id = id;
	pkt->cmd_res.cmd = cmd;
	pkt->cmd_res.is_end_of_res = is_end_of_res;
	return prep_pkt(pkt, GWFTP_PKT_TYPE_CMD_RESP, sizeof(pkt->cmd_res));
}

enum {
	GWFTP_EVENT_EPOLL	= 0x01,
	GWFTP_EVENT_IO_URING	= 0x02,
};

struct gwftp_server_cfg {
	char		*root_dir;
	char		*bind_addr;
	uint16_t	bind_port;
	uint8_t		event;
};

enum {
	GWFTP_SRV_CL_STATE_INIT		= 0x00,
	GWFTP_SRV_CL_STATE_HANDSHAKE	= 0x01,
	GWFTP_SRV_CL_STATE_ESTABLISHED	= 0x02,
};

struct gwftp_client {
	int				fd;
	uint32_t			ep_mask;
	uint8_t				state;
	struct sockaddr_storage		addr;
	size_t				rx_len;
	size_t				tx_len;
	struct gwftp_pkt		rx_pkt;
	struct gwftp_pkt		tx_pkt;
};

struct gwftp_server_ctx {
	volatile bool			should_stop;
	int				tcp_fd;

	uint32_t			max_clients;
	struct gwftp_client		*clients;
	struct gw_stack			cl_stack;

	union {
		struct gwftp_srv_ev_epoll	*ev_epoll;
	};

	struct gwftp_server_cfg		cfg;
};

struct gwftp_client_cfg {
	char		*server_addr;
	uint16_t	server_port;
	uint8_t		event;
};

enum {
	GWFTP_CL_STATE_INIT		= 0x00,
	GWFTP_CL_STATE_HANDSHAKE	= 0x01,
	GWFTP_CL_STATE_ESTABLISHED	= 0x02,
};

struct gwftp_client_ctx {
	volatile bool			should_stop;
	uint8_t				state;
	int				tcp_fd;

	union {
		struct gwftp_cli_ev_epoll	*ev_epoll;
	};

	size_t				rx_len;
	size_t				tx_len;
	struct gwftp_pkt		rx_pkt;
	struct gwftp_pkt		tx_pkt;
	struct gwftp_client_cfg		cfg;
};


int gwftp_server_evaluate_client_packet(struct gwftp_server_ctx *ctx,
					struct gwftp_client *cl);

struct gwftp_client *gwftp_server_get_client_slot(struct gwftp_server_ctx *ctx);
int gwftp_server_put_client_slot(struct gwftp_server_ctx *ctx,
				 struct gwftp_client *cl);

#endif /* #ifndef GWFTP__GWFTP_H */
