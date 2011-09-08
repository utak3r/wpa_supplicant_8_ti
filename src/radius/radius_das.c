/*
 * RADIUS Dynamic Authorization Server (DAS) (RFC 5176)
 * Copyright (c) 2012, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"
#include <net/if.h>

#include "utils/common.h"
#include "utils/eloop.h"
#include "utils/ip_addr.h"
#include "radius.h"
#include "radius_das.h"


extern int wpa_debug_level;


struct radius_das_data {
	int sock;
	u8 *shared_secret;
	size_t shared_secret_len;
	struct hostapd_ip_addr client_addr;
};


static void radius_das_receive(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct radius_das_data *das = eloop_ctx;
	u8 buf[1500];
	union {
		struct sockaddr_storage ss;
		struct sockaddr_in sin;
#ifdef CONFIG_IPV6
		struct sockaddr_in6 sin6;
#endif /* CONFIG_IPV6 */
	} from;
	char abuf[50];
	int from_port = 0;
	socklen_t fromlen;
	int len;
	struct radius_msg *msg, *reply = NULL;
	struct radius_hdr *hdr;
	struct wpabuf *rbuf;

	fromlen = sizeof(from);
	len = recvfrom(sock, buf, sizeof(buf), 0,
		       (struct sockaddr *) &from.ss, &fromlen);
	if (len < 0) {
		wpa_printf(MSG_ERROR, "DAS: recvfrom: %s", strerror(errno));
		return;
	}

	os_strlcpy(abuf, inet_ntoa(from.sin.sin_addr), sizeof(abuf));
	from_port = ntohs(from.sin.sin_port);

	wpa_printf(MSG_DEBUG, "DAS: Received %d bytes from %s:%d",
		   len, abuf, from_port);
	if (das->client_addr.u.v4.s_addr != from.sin.sin_addr.s_addr) {
		wpa_printf(MSG_DEBUG, "DAS: Drop message from unknown client");
		return;
	}

	msg = radius_msg_parse(buf, len);
	if (msg == NULL) {
		wpa_printf(MSG_DEBUG, "DAS: Parsing incoming RADIUS packet "
			   "from %s:%d failed", abuf, from_port);
		return;
	}

	if (wpa_debug_level <= MSG_MSGDUMP)
		radius_msg_dump(msg);

	if (radius_msg_verify_das_req(msg, das->shared_secret,
				       das->shared_secret_len)) {
		wpa_printf(MSG_DEBUG, "DAS: Invalid authenticator in packet "
			   "from %s:%d - drop", abuf, from_port);
		goto fail;
	}

	hdr = radius_msg_get_hdr(msg);

	switch (hdr->code) {
	case RADIUS_CODE_DISCONNECT_REQUEST:
		/* TODO */
		reply = radius_msg_new(RADIUS_CODE_DISCONNECT_NAK,
				       hdr->identifier);
		if (reply == NULL)
			break;

		radius_msg_add_attr_int32(reply, RADIUS_ATTR_ERROR_CAUSE, 405);
		break;
	case RADIUS_CODE_COA_REQUEST:
		/* TODO */
		reply = radius_msg_new(RADIUS_CODE_COA_NAK,
				       hdr->identifier);
		if (reply == NULL)
			break;

		/* Unsupported Service */
		radius_msg_add_attr_int32(reply, RADIUS_ATTR_ERROR_CAUSE, 405);
		break;
	default:
		wpa_printf(MSG_DEBUG, "DAS: Unexpected RADIUS code %u in "
			   "packet from %s:%d",
			   hdr->code, abuf, from_port);
	}

	if (reply) {
		int res;

		wpa_printf(MSG_DEBUG, "DAS: Reply to %s:%d", abuf, from_port);

		if (radius_msg_finish_das_resp(reply, das->shared_secret,
					       das->shared_secret_len, hdr) <
		    0) {
			wpa_printf(MSG_DEBUG, "DAS: Failed to add "
				   "Message-Authenticator attribute");
		}

		if (wpa_debug_level <= MSG_MSGDUMP)
			radius_msg_dump(reply);

		rbuf = radius_msg_get_buf(reply);
		res = sendto(das->sock, wpabuf_head(rbuf),
			     wpabuf_len(rbuf), 0,
			     (struct sockaddr *) &from.ss, fromlen);
		if (res < 0) {
			wpa_printf(MSG_ERROR, "DAS: sendto(to %s:%d): %s",
				   abuf, from_port, strerror(errno));
		}
	}

fail:
	radius_msg_free(msg);
	radius_msg_free(reply);
}


static int radius_das_open_socket(int port)
{
	int s;
	struct sockaddr_in addr;

	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}

	os_memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		close(s);
		return -1;
	}

	return s;
}


struct radius_das_data *
radius_das_init(struct radius_das_conf *conf)
{
	struct radius_das_data *das;

	if (conf->port == 0 || conf->shared_secret == NULL ||
	    conf->client_addr == NULL)
		return NULL;

	das = os_zalloc(sizeof(*das));
	if (das == NULL)
		return NULL;

	os_memcpy(&das->client_addr, conf->client_addr,
		  sizeof(das->client_addr));

	das->shared_secret = os_malloc(conf->shared_secret_len);
	if (das->shared_secret == NULL) {
		radius_das_deinit(das);
		return NULL;
	}
	os_memcpy(das->shared_secret, conf->shared_secret,
		  conf->shared_secret_len);
	das->shared_secret_len = conf->shared_secret_len;

	das->sock = radius_das_open_socket(conf->port);
	if (das->sock < 0) {
		wpa_printf(MSG_ERROR, "Failed to open UDP socket for RADIUS "
			   "DAS");
		radius_das_deinit(das);
		return NULL;
	}

	if (eloop_register_read_sock(das->sock, radius_das_receive, das, NULL))
	{
		radius_das_deinit(das);
		return NULL;
	}

	return das;
}


void radius_das_deinit(struct radius_das_data *das)
{
	if (das == NULL)
		return;

	if (das->sock >= 0) {
		eloop_unregister_read_sock(das->sock);
		close(das->sock);
	}

	os_free(das->shared_secret);
	os_free(das);
}