
#include "validator.h"

#include <errno.h>

static int server_validate_cl_pkt_hdr_handshake(struct gwftp_pkt *pkt,
						struct gwftp_client *cl)
{
	struct gwftp_pkt_hdr *hdr = &pkt->hdr;

	if (unlikely(cl->state != GWFTP_SRV_CL_STATE_HANDSHAKE)) {
		pr_dbg("Handshake packet received in invalid state: %u",
		       cl->state);
		return -EBADMSG;
	}

	if (unlikely(hdr->len != sizeof(struct gwftp_pkt_handshake))) {
		pr_dbg("Invalid handshake packet length: %u, expected %zu",
		       hdr->len, sizeof(struct gwftp_pkt_handshake));
		return -EBADMSG;
	}

	return 0;
}

__hot
int gwftp_server_validate_cl_pkt_hdr(struct gwftp_client *cl)
{
	struct gwftp_pkt *pkt = &cl->rx_pkt;
	int ret;

	if (unlikely(pkt->hdr.resv)) {
		pr_err("Invalid reserved field: %u, expected 0", pkt->hdr.resv);
		return -EBADMSG;
	}

	switch (pkt->hdr.type) {
	case GWFTP_PKT_TYPE_HANDSHAKE:
		ret = server_validate_cl_pkt_hdr_handshake(pkt, cl);
		break;
	default:
		pr_dbg("Unsupported packet type: %u", pkt->hdr.type);
		ret = -EBADMSG;
		break;
	}

	return ret;
}

static int server_validate_cl_pkt_body_handshake(struct gwftp_pkt *pkt)
{
	struct gwftp_pkt_handshake *hs = &pkt->hs;

	if (unlikely(ntohl(hs->magic) != GWFTP_HANDSHAKE_MAGIC)) {
		pr_dbg("Invalid GWFTP magic: 0x%08x, expected 0x%08x", hs->magic,
			GWFTP_HANDSHAKE_MAGIC);
		return -EBADMSG;
	}

	hs->extra[sizeof(hs->extra) - 1] = '\0';
	if (unlikely(hs->major != GWFTP_VERSION_MAJOR))
		goto out_not_supported;
	if (unlikely(hs->minor != GWFTP_VERSION_MINOR))
		goto out_not_supported;

	return 0;

out_not_supported:
	pr_dbg("Unsupported GWFTP version: %u.%u.%u-%s", hs->major, hs->minor,
	       hs->patch, hs->extra);
	return -ENOTSUP;
}

__hot
int gwftp_server_validate_cl_pkt_body(struct gwftp_client *cl)
{
	struct gwftp_pkt *pkt = &cl->rx_pkt;
	int ret;

	switch (pkt->hdr.type) {
	case GWFTP_PKT_TYPE_HANDSHAKE:
		ret = server_validate_cl_pkt_body_handshake(pkt);
		break;
	default:
		pr_dbg("Unsupported packet type: %u", pkt->hdr.type);
		ret = -EBADMSG;
		break;
	}

	(void)cl;
	return ret;
}
