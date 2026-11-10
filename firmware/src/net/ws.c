/*
 * WebSocket connection to the Central System (OCPP-J transport).
 *
 * - URL: <CFG _ServerUrl>/<CFG _ChargePointId>, e.g. ws://10.0.2.2:9000/ocpp/CP001
 * - Subprotocol header "Sec-WebSocket-Protocol: ocpp1.6" (OCPP-J 1.6 section 3.1)
 * - Zephyr's websocket client does not answer PING by itself: we send the PONG.
 * - Only "ws://" (no TLS). "wss://" is refused with a clear log line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/logging/log.h>
#include "net/ws.h"

LOG_MODULE_REGISTER(ws, LOG_LEVEL_INF);

#define CONNECT_TIMEOUT_MS 5000

static int tcp_fd = -1;
static int ws_fd = -1;
static uint8_t http_tmp[1024];
static uint8_t ctrl_buf[128]; /* payload of PING frames */

static const char *const extra_headers[] = {
	"Sec-WebSocket-Protocol: ocpp1.6\r\n",
	NULL,
};

int ws_parse_url(const char *url, const char *cp_id, struct ws_url *out)
{
	const char *p, *slash, *colon;
	size_t hlen;

	if (strncmp(url, "wss://", 6) == 0) {
		LOG_ERR("wss:// (TLS) is not supported by this build, use ws://");
		return -ENOTSUP;
	}
	if (strncmp(url, "ws://", 5) != 0) {
		LOG_ERR("server URL must start with ws:// : %s", url);
		return -EINVAL;
	}
	p = url + 5;
	slash = strchr(p, '/');
	if (!slash) {
		slash = p + strlen(p);
	}
	colon = memchr(p, ':', slash - p);
	hlen = (colon ? colon : slash) - p;
	if (hlen == 0 || hlen >= sizeof(out->host)) {
		return -EINVAL;
	}
	memcpy(out->host, p, hlen);
	out->host[hlen] = '\0';
	if (colon) {
		size_t plen = slash - colon - 1;

		if (plen == 0 || plen >= sizeof(out->port)) {
			return -EINVAL;
		}
		memcpy(out->port, colon + 1, plen);
		out->port[plen] = '\0';
	} else {
		strcpy(out->port, "80");
	}
	/* path + "/" + cp_id, without a double slash */
	size_t base = strlen(slash);

	while (base > 0 && slash[base - 1] == '/') {
		base--;
	}
	if (snprintf(out->path, sizeof(out->path), "%.*s/%s", (int)base, slash, cp_id) >=
	    (int)sizeof(out->path)) {
		return -EINVAL;
	}
	snprintf(out->host_hdr, sizeof(out->host_hdr), "%s:%s", out->host, out->port);
	return 0;
}

static void close_all(void)
{
	if (ws_fd >= 0) {
		websocket_disconnect(ws_fd); /* also closes tcp_fd */
	} else if (tcp_fd >= 0) {
		zsock_close(tcp_fd);
	}
	ws_fd = -1;
	tcp_fd = -1;
}

int ws_connect(const char *url, const char *cp_id)
{
	struct ws_url u;
	struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res = NULL;
	struct websocket_request req = {0};
	int rc;

	close_all();
	rc = ws_parse_url(url, cp_id, &u);
	if (rc) {
		return rc;
	}
	rc = zsock_getaddrinfo(u.host, u.port, &hints, &res);
	if (rc || !res) {
		LOG_WRN("cannot resolve %s (%d)", u.host, rc);
		return -EHOSTUNREACH;
	}
	tcp_fd = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TCP);
	if (tcp_fd < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}
	rc = zsock_connect(tcp_fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (rc < 0) {
		rc = -errno;
		LOG_WRN("TCP connect to %s failed (%d)", u.host_hdr, rc);
		close_all();
		return rc;
	}

	req.host = u.host_hdr;
	req.url = u.path;
	req.optional_headers = (const char **)extra_headers;
	req.tmp_buf = http_tmp;
	req.tmp_buf_len = sizeof(http_tmp);

	ws_fd = websocket_connect(tcp_fd, &req, CONNECT_TIMEOUT_MS, NULL);
	if (ws_fd < 0) {
		rc = ws_fd;
		ws_fd = -1;
		LOG_WRN("WebSocket handshake with ws://%s%s failed (%d)", u.host_hdr, u.path, rc);
		close_all();
		return rc;
	}
	LOG_INF("connected to ws://%s%s", u.host_hdr, u.path);
	return 0;
}

bool ws_is_up(void)
{
	return ws_fd >= 0;
}

void ws_close(void)
{
	if (ws_fd >= 0 || tcp_fd >= 0) {
		LOG_INF("connection closed");
	}
	close_all();
}

int ws_send_text(const char *msg, size_t len)
{
	int rc;

	if (ws_fd < 0) {
		return -ENOTCONN;
	}
	rc = websocket_send_msg(ws_fd, (const uint8_t *)msg, len, WEBSOCKET_OPCODE_DATA_TEXT, true,
				true, 2000);
	if (rc < 0) {
		LOG_WRN("send failed (%d)", rc);
		ws_close();
		return rc;
	}
	return 0;
}

int ws_recv_text(char *buf, size_t len, int timeout_ms)
{
	size_t used = 0;
	uint32_t type = 0;
	uint64_t remaining = 0;
	int rc;

	if (ws_fd < 0) {
		return -ENOTCONN;
	}
	do {
		uint8_t *dst = used < len - 1 ? (uint8_t *)buf + used : ctrl_buf;
		size_t room = used < len - 1 ? len - 1 - used : sizeof(ctrl_buf);

		rc = websocket_recv_msg(ws_fd, dst, room, &type, &remaining,
					used == 0 ? timeout_ms : 2000);
		if (rc == -EAGAIN && used == 0) {
			return 0; /* nothing arrived */
		}
		if (rc < 0) {
			LOG_WRN("receive failed (%d)", rc);
			ws_close();
			return -ENOTCONN;
		}
		if (type & WEBSOCKET_FLAG_CLOSE) {
			LOG_INF("server closed the WebSocket");
			ws_close();
			return -ENOTCONN;
		}
		if (type & WEBSOCKET_FLAG_PING) {
			/* PING payload landed in dst; echo it back as PONG */
			websocket_send_msg(ws_fd, dst, rc, WEBSOCKET_OPCODE_PONG, true, true, 1000);
			if (remaining == 0) {
				return 0;
			}
			continue;
		}
		if (type & WEBSOCKET_FLAG_PONG) {
			return 0;
		}
		if (dst == (uint8_t *)buf + used) {
			used += rc;
		} else {
			LOG_ERR("message larger than %u bytes dropped", (unsigned)len);
			used = len; /* mark as overflow, keep draining */
		}
	} while (remaining > 0);

	if (used >= len) {
		return -EMSGSIZE;
	}
	buf[used] = '\0';
	return (int)used;
}
