// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__GWFTP_H
#define GWFTP__GWFTP_H

#include "ev_epoll.h"
#include "gw_stack.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include <linux/types.h>
#include <netinet/in.h>

#define pr_err(fmt, ...) fprintf(stderr, "Error: " fmt "\n", ##__VA_ARGS__)

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

typedef uint8_t u8;

#define GWFTP_PATH_MAX 4096

struct gwftp_pkt_hdr {
	u8	type;
	u8	resv;
	__be16	len;
} __packed;

struct gwftp_pkt_handshake {
	u8	major;
	u8	minor;
	u8	patch;
	u8	extra[29];
} __packed;

struct gwftp_pkt_handshake_res {
	u8	status;
	u8	resv[3];
	u8	major;
	u8	minor;
	u8	patch;
	u8	extra[29];
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

enum {
	GWFTP_PKT_TYPE_HANDSHAKE	= 0x01,
	GWFTP_PKT_TYPE_HANDSHAKE_RESP	= 0x02,
	GWFTP_PKT_TYPE_CMD		= 0x03,
	GWFTP_PKT_TYPE_CMD_RESP		= 0x04,
	GWFTP_PKT_TYPE_CLOSE		= 0x05,
};

struct gwftp_pkt {
	struct gwftp_pkt_hdr	hdr;

	union {
		struct gwftp_pkt_handshake	hs;
		struct gwftp_pkt_handshake_res	hs_res;
		struct gwftp_pkt_cmd		cmd;
		struct gwftp_pkt_cmd_res	cmd_res;
	};
} __packed;

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

struct gwftp_client {
	int				fd;
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
		struct gwftp_ev_epoll	*ev_epoll;
	};

	struct gwftp_server_cfg		cfg;
};

#endif /* #ifndef GWFTP__GWFTP_H */
