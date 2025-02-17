// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__VALIDATOR_H
#define GWFTP__VALIDATOR_H

#include "gwftp.h"

int gwftp_server_validate_cl_pkt_hdr(struct gwftp_client *cl);
int gwftp_server_validate_cl_pkt_body(struct gwftp_client *cl);
int gwftp_client_validate_sr_pkt_hdr(struct gwftp_client_ctx *ctx);
int gwftp_client_validate_sr_pkt_body(struct gwftp_client_ctx *ctx);

#endif /* #ifndef GWFTP__VALIDATOR_H */
