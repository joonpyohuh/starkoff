/* WebSocket transport to the Central System. See ws.c. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct ws_url {
	char host[128];
	char port[8];
	char path[256];
	char host_hdr[140];
};

int ws_parse_url(const char *url, const char *cp_id, struct ws_url *out);
int ws_connect(const char *url, const char *cp_id); /* blocking, ~5 s max for handshake */
bool ws_is_up(void);
void ws_close(void);
int ws_send_text(const char *msg, size_t len);
/* >0: length of a text message in buf (NUL-terminated), 0: nothing, <0: disconnected / too big */
int ws_recv_text(char *buf, size_t len, int timeout_ms);
