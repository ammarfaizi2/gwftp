// SPDX-License-Identifier: GPL-2.0-only
#ifndef GWFTP__VALIDATOR_H
#define GWFTP__VALIDATOR_H

#include "gwftp.h"

int gwftp_server_validate_cl_pkt_hdr(struct gwftp_client *cl);
int gwftp_server_validate_cl_pkt_body(struct gwftp_client *cl);

#endif /* #ifndef GWFTP__VALIDATOR_H */
