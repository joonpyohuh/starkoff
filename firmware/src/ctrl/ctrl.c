/*
 * Control interface for the simulated hardware and the configuration.
 *
 * Same text commands on two transports:
 *   - UART shell:      cp plug 1
 *   - TCP port 7000:   plug 1\n         (used by the lab web UI and the automated tests)
 * One line in, one line out ("OK ...", "ERR ...", or the status line).
 *
 * Commands:
 *   status
 *   plug <c> | unplug <c> | ev_disconnect <c> | card <c> <idTag>
 *   power off|on | lock_fail <c> on|off | fault <c> overcurrent|overtemp|overvoltage|off
 *   net_down | net_up
 *   cfg get <key> | cfg set <key> <value> | cfg list | cfg reset
 *   reboot
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include "ctrl/ctrl.h"
#include "sim/sim.h"
#include "ocpp/config.h"

LOG_MODULE_REGISTER(ctrl, LOG_LEVEL_INF);

#define MAX_ARGS    6
#define LINE_MAX    300
#define MAX_CLIENTS 4

static ctrl_status_hook_t status_hook;

static void do_reboot(struct k_work *w)
{
	ARG_UNUSED(w);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, do_reboot);

void ctrl_set_status_hook(ctrl_status_hook_t hook)
{
	status_hook = hook;
}

static int conn_arg(const char *s)
{
	return s ? atoi(s) : 0;
}

static int on_off(const char *s, bool *out)
{
	if (s && strcmp(s, "on") == 0) {
		*out = true;
		return 0;
	}
	if (s && strcmp(s, "off") == 0) {
		*out = false;
		return 0;
	}
	return -EINVAL;
}

static const char *cfg_result_str(enum cfg_set_result r)
{
	switch (r) {
	case CFG_ACCEPTED:
		return "Accepted";
	case CFG_REJECTED:
		return "Rejected";
	case CFG_REBOOT_REQUIRED:
		return "RebootRequired";
	default:
		return "NotSupported";
	}
}

static void cmd_cfg(int argc, char **argv, char *out, size_t len)
{
	const char *sub = argc > 1 ? argv[1] : "";

	if (strcmp(sub, "get") == 0 && argc == 3) {
		const char *v = cfg_get(argv[2]);

		snprintf(out, len, v ? "OK %s=%s" : "ERR unknown key %s", argv[2], v);
	} else if (strcmp(sub, "set") == 0 && argc >= 3) {
		/* value may be empty: "cfg set StopTxnSampledData" */
		enum cfg_set_result r = cfg_set(argv[2], argc > 3 ? argv[3] : "", false);

		snprintf(out, len, "%s %s", r == CFG_ACCEPTED ? "OK" : "ERR", cfg_result_str(r));
	} else if (strcmp(sub, "list") == 0) {
		size_t n = snprintf(out, len, "OK");

		for (size_t i = 0; i < cfg_count() && n < len; i++) {
			n += snprintf(out + n, len - n, " %s=%s", cfg_at(i)->key, cfg_at(i)->val);
		}
	} else if (strcmp(sub, "reset") == 0) {
		cfg_factory_reset();
		snprintf(out, len, "OK defaults restored (reboot to apply url/id)");
	} else {
		snprintf(out, len,
			 "ERR usage: cfg get <k> | cfg set <k> <v> | cfg list | cfg reset");
	}
}

void ctrl_exec(char *line, char *out, size_t len)
{
	char *argv[MAX_ARGS];
	int argc = 0;
	char *save;
	int rc = 0;
	bool b;

	for (char *t = strtok_r(line, " \t\r\n", &save); t && argc < MAX_ARGS;
	     t = strtok_r(NULL, " \t\r\n", &save)) {
		argv[argc++] = t;
	}
	if (argc == 0) {
		snprintf(out, len, "ERR empty");
		return;
	}

	const char *cmd = argv[0];
	int c = argc > 1 ? conn_arg(argv[1]) : 0;

	if (strcmp(cmd, "status") == 0) {
		size_t n = snprintf(out, len, "OK ");

		if (status_hook) {
			n += status_hook(out + n, len - n);
			if (n < len - 1) {
				out[n++] = ' ';
			}
		}
		sim_status(out + n, len - n);
		return;
	} else if (strcmp(cmd, "plug") == 0) {
		rc = sim_plug(c);
	} else if (strcmp(cmd, "unplug") == 0) {
		rc = sim_unplug(c);
		if (rc == -EBUSY) {
			snprintf(out, len, "ERR connector locked");
			return;
		}
	} else if (strcmp(cmd, "ev_disconnect") == 0) {
		rc = sim_ev_disconnect(c);
	} else if (strcmp(cmd, "card") == 0) {
		rc = sim_card(c, argc > 2 ? argv[2] : NULL);
	} else if (strcmp(cmd, "power") == 0) {
		rc = on_off(argc > 1 ? argv[1] : NULL, &b);
		if (rc == 0) {
			rc = sim_power(b);
		}
	} else if (strcmp(cmd, "lock_fail") == 0) {
		rc = on_off(argc > 2 ? argv[2] : NULL, &b);
		if (rc == 0) {
			rc = sim_lock_fail(c, b);
		}
	} else if (strcmp(cmd, "fault") == 0) {
		rc = argc > 2 ? sim_fault(c, argv[2]) : -EINVAL;
	} else if (strcmp(cmd, "noise") == 0) {
		rc = on_off(argc > 2 ? argv[2] : NULL, &b);
		if (rc == 0) {
			rc = sim_noise(c, b);
		}
	} else if (strcmp(cmd, "hang") == 0) {
		sim_hang(); /* main loop stalls; watchdog reboots after WDT_TIMEOUT */
	} else if (strcmp(cmd, "net_down") == 0) {
		rc = sim_link(false);
	} else if (strcmp(cmd, "net_up") == 0) {
		rc = sim_link(true);
	} else if (strcmp(cmd, "cfg") == 0) {
		cmd_cfg(argc, argv, out, len);
		return;
	} else if (strcmp(cmd, "reboot") == 0) {
		/* Reply first, reboot 300 ms later so the client gets the answer. */
		LOG_INF("reboot requested from control interface");
		k_work_schedule(&reboot_work, K_MSEC(300));
		snprintf(out, len, "OK rebooting");
		return;
	} else {
		snprintf(out, len, "ERR unknown command %s", cmd);
		return;
	}
	snprintf(out, len, rc == 0 ? "OK" : "ERR bad arguments");
}

/* ---------------- TCP server (poll on listener + up to MAX_CLIENTS) ---------------- */

struct client {
	int fd;
	char buf[LINE_MAX];
	size_t used;
};

static struct client clients[MAX_CLIENTS];
static char reply[2048];

static void send_all(int fd, const char *s, size_t n)
{
	while (n > 0) {
		ssize_t w = zsock_send(fd, s, n, 0);

		if (w <= 0) {
			return;
		}
		s += w;
		n -= w;
	}
}

static void close_client(struct client *cl)
{
	zsock_close(cl->fd);
	cl->fd = -1;
	cl->used = 0;
}

/* Handle bytes from one client; returns false if the client should be closed. */
static bool client_rx(struct client *cl)
{
	ssize_t n = zsock_recv(cl->fd, cl->buf + cl->used, sizeof(cl->buf) - 1 - cl->used, 0);
	char *nl;

	if (n <= 0) {
		return false;
	}
	cl->used += n;
	cl->buf[cl->used] = '\0';
	while ((nl = strchr(cl->buf, '\n')) != NULL) {
		*nl = '\0';
		ctrl_exec(cl->buf, reply, sizeof(reply) - 1);
		strcat(reply, "\n");
		send_all(cl->fd, reply, strlen(reply));
		size_t rest = cl->used - (nl + 1 - cl->buf);

		memmove(cl->buf, nl + 1, rest + 1);
		cl->used = rest;
	}
	if (cl->used >= sizeof(cl->buf) - 1) {
		send_all(cl->fd, "ERR line too long\n", 18);
		return false;
	}
	return true;
}

static void ctrl_thread(void *a, void *b, void *c)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_CTRL_TCP_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	struct zsock_pollfd fds[MAX_CLIENTS + 1];
	int one = 1;
	int lfd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (lfd < 0) {
		LOG_ERR("control socket failed: %d", errno);
		return;
	}
	zsock_setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (zsock_bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(lfd, 2) < 0) {
		LOG_ERR("control port %d bind/listen failed: %d", CONFIG_CTRL_TCP_PORT, errno);
		return;
	}
	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
	}
	LOG_INF("control port listening on TCP %d", CONFIG_CTRL_TCP_PORT);

	while (1) {
		fds[0].fd = lfd;
		fds[0].events = ZSOCK_POLLIN;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			fds[i + 1].fd = clients[i].fd;
			fds[i + 1].events = ZSOCK_POLLIN;
			fds[i + 1].revents = 0;
		}
		if (zsock_poll(fds, MAX_CLIENTS + 1, -1) < 0) {
			LOG_ERR("poll failed: %d", errno);
			k_msleep(1000);
			continue;
		}
		if (fds[0].revents & ZSOCK_POLLIN) {
			int fd = zsock_accept(lfd, NULL, NULL);
			int slot = -1;

			for (int i = 0; i < MAX_CLIENTS && fd >= 0; i++) {
				if (clients[i].fd < 0) {
					slot = i;
					break;
				}
			}
			if (fd >= 0 && slot < 0) {
				send_all(fd, "ERR too many clients\n", 21);
				zsock_close(fd);
			} else if (fd >= 0) {
				clients[slot].fd = fd;
				clients[slot].used = 0;
			}
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0 && fds[i + 1].revents && !client_rx(&clients[i])) {
				close_client(&clients[i]);
			}
		}
	}
}

K_THREAD_STACK_DEFINE(ctrl_stack, 6144);
static struct k_thread ctrl_tid;

void ctrl_start(void)
{
	k_thread_create(&ctrl_tid, ctrl_stack, K_THREAD_STACK_SIZEOF(ctrl_stack), ctrl_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&ctrl_tid, "ctrl");
}

/* ---------------- UART shell: "cp <command ...>" ---------------- */

static char shell_line[LINE_MAX];
static char shell_reply[2048];

static int cmd_cp(const struct shell *sh, size_t argc, char **argv)
{
	size_t n = 0;

	shell_line[0] = '\0';
	for (size_t i = 1; i < argc && n < sizeof(shell_line) - 2; i++) {
		n += snprintf(shell_line + n, sizeof(shell_line) - n, "%s%s", i > 1 ? " " : "",
			      argv[i]);
	}
	ctrl_exec(shell_line, shell_reply, sizeof(shell_reply));
	shell_print(sh, "%s", shell_reply);
	return 0;
}

SHELL_CMD_ARG_REGISTER(
	cp, NULL,
	"Charge point control: status | plug <c> | unplug <c> | ev_disconnect <c> | "
	"card <c> <idTag> | power on|off | lock_fail <c> on|off | noise <c> on|off | hang | "
	"fault <c> overcurrent|overtemp|overvoltage|off | net_down | net_up | "
	"cfg get|set|list|reset | reboot",
	cmd_cp, 2, 6);
