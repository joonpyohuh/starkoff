/* Control interface: UART shell "cp ..." and TCP port (CONFIG_CTRL_TCP_PORT). */
#pragma once
#include <stddef.h>

/* The charger logic registers this to add OCPP state to the "status" line. */
typedef size_t (*ctrl_status_hook_t)(char *buf, size_t len);

void ctrl_set_status_hook(ctrl_status_hook_t hook);
void ctrl_start(void);
/* Executes one command line (modified in place) and writes a one-line reply. */
void ctrl_exec(char *line, char *out, size_t len);
