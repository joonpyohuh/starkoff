/*
 * OCPP 1.6J charge point engine (runs in the main thread as a simple loop).
 *
 * Every ~50 ms:
 *   1. connection manager: (re)connect with growing back-off, or stay down if
 *      power is off / network cable pulled
 *   2. receive one WebSocket message and dispatch it (CALL / CALLRESULT / CALLERROR)
 *   3. handle simulated hardware events
 *   4. timers: BootNotification retry, Heartbeat, CALL timeout
 *   5. report connector status changes
 *   6. send the next queued request if nothing is in flight
 *
 * OCPP-J framing (OCPP-J 1.6 section 4):
 *   CALL       [2, "<id>", "<Action>", {payload}]
 *   CALLRESULT [3, "<id>", {payload}]
 *   CALLERROR  [4, "<id>", "<errorCode>", "<description>", {details}]
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include "ocpp/ocpp.h"
#include "ocpp/json.h"
#include "ocpp/clock.h"
#include "ocpp/config.h"
#include "ocpp/connector.h"
#include "net/ws.h"
#include "sim/sim.h"
#include "ctrl/ctrl.h"

LOG_MODULE_REGISTER(ocpp, LOG_LEVEL_INF);

#define FW_VERSION          "0.3.0"
#define CALL_TIMEOUT_MS     30000  /* no answer -> treat connection as broken */
#define BOOT_RETRY_DEFAULT  30     /* s, when the server gives interval 0 */
#define BACKOFF_MIN_S       5
#define BACKOFF_MAX_S       60
#define LOOP_MS             50

#define OUTQ_LEN            16
#define OUT_PAYLOAD_MAX     1024
#define RX_MAX              4096
#define TX_MAX              4096
#define LOG_MSG_MAX         300

/* ---------------- state ---------------- */

static enum boot_state boot = BOOT_NOT_SENT;
static int64_t boot_retry_at;      /* uptime ms: earliest next BootNotification */
static int64_t next_hb_at;         /* uptime ms: next Heartbeat */
/* First attempt ~2 s after boot: by then QEMU's NAT has learned our MAC (link.c),
 * so the first SYN is answered instead of being retransmitted for several seconds. */
static int64_t next_connect_at = 2000;
static int backoff_s = BACKOFF_MIN_S;
static bool dead;                  /* power is off: charger logic stopped */
static enum cp_status reported[CONN_MAX + 1];

struct out_msg {
	char action[32];
	char payload[OUT_PAYLOAD_MAX];
	bool tx_related;   /* transaction message: kept across reconnects (step 4) */
};
static struct out_msg outq[OUTQ_LEN];
static int outq_head, outq_count;

static struct {
	bool active;
	bool from_queue;   /* true: queue head; false: BootNotification sent directly */
	char id[40];
	char action[32];
	int64_t sent_at;
} inflight;

static uint32_t msg_counter;
static uint32_t msg_prefix;
static char rxbuf[RX_MAX];
static char txbuf[TX_MAX];
static char paybuf[OUT_PAYLOAD_MAX];
static struct jp jp;

static const char *boot_str(enum boot_state b)
{
	static const char *const s[] = { "NotSent", "Sent", "Accepted", "Pending", "Rejected" };

	return s[b];
}

enum boot_state ocpp_boot_state(void)
{
	return boot;
}

/* ---------------- logging & sending ---------------- */

static void log_msg(const char *dir, const char *m)
{
	LOG_INF("%s %.*s%s", dir, LOG_MSG_MAX, m, strlen(m) > LOG_MSG_MAX ? "..." : "");
}

static int send_frame(const char *frame)
{
	log_msg(">>", frame);
	return ws_send_text(frame, strlen(frame));
}

static void new_id(char *out, size_t len)
{
	snprintf(out, len, "%08x-%u", msg_prefix, ++msg_counter);
}

static int send_call(const char *action, const char *payload, bool from_queue)
{
	struct jw w;

	new_id(inflight.id, sizeof(inflight.id));
	jw_init(&w, txbuf, sizeof(txbuf));
	jw_printf(&w, "[2,\"%s\",\"%s\",%s]", inflight.id, action, payload);
	if (w.err) {
		LOG_ERR("%s too large, dropped", action);
		return -ENOMEM;
	}
	strncpy(inflight.action, action, sizeof(inflight.action) - 1);
	inflight.active = true;
	inflight.from_queue = from_queue;
	inflight.sent_at = k_uptime_get();
	return send_frame(txbuf);
}

static void send_result(const char *id, const char *payload)
{
	struct jw w;

	jw_init(&w, txbuf, sizeof(txbuf));
	jw_raw(&w, "[3,");
	jw_str(&w, id);
	jw_printf(&w, ",%s]", payload);
	if (!w.err) {
		send_frame(txbuf);
	}
}

static void send_error(const char *id, const char *code, const char *desc)
{
	struct jw w;

	jw_init(&w, txbuf, sizeof(txbuf));
	jw_raw(&w, "[4,");
	jw_str(&w, id);
	jw_raw(&w, ",");
	jw_str(&w, code);
	jw_raw(&w, ",");
	jw_str(&w, desc);
	jw_raw(&w, ",{}]");
	if (!w.err) {
		send_frame(txbuf);
	}
}

/* ---------------- outgoing queue ---------------- */

int ocpp_enqueue(const char *action, const char *payload, bool tx_related)
{
	struct out_msg *m;

	if (outq_count == OUTQ_LEN) {
		LOG_ERR("outgoing queue full, %s dropped", action);
		return -ENOMEM;
	}
	if (strlen(payload) >= OUT_PAYLOAD_MAX) {
		LOG_ERR("%s payload too large, dropped", action);
		return -EMSGSIZE;
	}
	m = &outq[(outq_head + outq_count) % OUTQ_LEN];
	strncpy(m->action, action, sizeof(m->action) - 1);
	m->action[sizeof(m->action) - 1] = '\0';
	strcpy(m->payload, payload);
	m->tx_related = tx_related;
	outq_count++;
	return 0;
}

static bool queued(const char *action)
{
	for (int i = 0; i < outq_count; i++) {
		if (strcmp(outq[(outq_head + i) % OUTQ_LEN].action, action) == 0) {
			return true;
		}
	}
	return false;
}

static void outq_pop(void)
{
	if (outq_count > 0) {
		outq_head = (outq_head + 1) % OUTQ_LEN;
		outq_count--;
	}
}

/* After a disconnect: keep only transaction messages (they must be delivered).
 * Compacts the ring in place (no temporary copy: a copy of the queue would be
 * ~17 KB, far more than the thread stack). */
static void outq_drop_non_tx(void)
{
	int n = 0;

	for (int i = 0; i < outq_count; i++) {
		int from = (outq_head + i) % OUTQ_LEN;
		int to = (outq_head + n) % OUTQ_LEN;

		if (outq[from].tx_related) {
			if (from != to) {
				outq[to] = outq[from];
			}
			n++;
		}
	}
	outq_count = n;
}

/* ---------------- messages we send ---------------- */

static void send_boot(void)
{
	struct jw w;
	bool first = true;

	jw_init(&w, paybuf, sizeof(paybuf));
	jw_raw(&w, "{");
	jw_kv_str(&w, &first, "chargePointVendor", "TakeHomeLab");
	jw_kv_str(&w, &first, "chargePointModel", "ZephyrQemuCP");
	jw_kv_str(&w, &first, "chargePointSerialNumber", cfg_get(CFG_CP_ID));
	jw_kv_str(&w, &first, "firmwareVersion", FW_VERSION);
	jw_raw(&w, "}");
	boot = BOOT_SENT;
	send_call("BootNotification", paybuf, false);
}

static void enqueue_status(int c)
{
	struct jw w;
	bool first = true;
	char ts[CLOCK_ISO_LEN];
	enum cp_status s = conn_status(c);

	clock_now_iso(ts, sizeof(ts));
	jw_init(&w, paybuf, sizeof(paybuf));
	jw_raw(&w, "{");
	jw_kv_int(&w, &first, "connectorId", c);
	jw_kv_str(&w, &first, "errorCode", conn_error_code(c));
	jw_kv_str(&w, &first, "status", conn_status_str(s));
	jw_kv_str(&w, &first, "timestamp", ts);
	jw_raw(&w, "}");
	if (ocpp_enqueue("StatusNotification", paybuf, false) == 0) {
		reported[c] = s;
	}
}

/* Report every connector again (after Accepted / after a reconnect). */
static void report_all_status(void)
{
	for (int c = 0; c <= CONN_MAX; c++) {
		reported[c] = ST_UNKNOWN;
	}
}

static void heartbeat_reschedule(void)
{
	int s = cfg_get_int("HeartbeatInterval", 300);

	next_hb_at = s > 0 ? k_uptime_get() + (int64_t)s * 1000 : INT64_MAX;
}

/* ---------------- incoming CALL handlers ---------------- */

static void add_config_key(struct jw *w, bool *first_item, const struct cfg_item *it)
{
	bool first = true;

	if (!*first_item) {
		jw_raw(w, ",");
	}
	*first_item = false;
	jw_raw(w, "{");
	jw_kv_str(w, &first, "key", it->key);
	jw_kv_raw(w, &first, "readonly", (it->flags & CFG_RO) ? "true" : "false");
	jw_kv_str(w, &first, "value", it->val);
	jw_raw(w, "}");
}

/* All keys with values are ~2 KB: bigger than a normal payload, smaller than one frame. */
static char cfg_resp[TX_MAX - 128];

static void h_get_configuration(const char *id, int pl)
{
	struct jw w;
	int keys = jp_get(&jp, pl, "key");
	bool first = true, first_unknown = true;
	static char unknown[OUT_PAYLOAD_MAX];
	struct jw wu;

	jw_init(&w, cfg_resp, sizeof(cfg_resp));
	jw_init(&wu, unknown, sizeof(unknown));
	jw_raw(&w, "{\"configurationKey\":[");
	if (keys < 0 || jp.tok[keys].size == 0) {
		/* no key list = all keys */
		for (size_t i = 0; i < cfg_count(); i++) {
			if (!(cfg_at(i)->flags & CFG_HIDDEN)) {
				add_config_key(&w, &first, cfg_at(i));
			}
		}
	} else if (jp_is_type(&jp, keys, JSMN_ARRAY)) {
		int t = keys + 1;

		for (int k = 0; k < jp.tok[keys].size; k++, t = jp_skip(&jp, t)) {
			char name[64];
			const struct cfg_item *it;

			if (!jp_str(&jp, t, name, sizeof(name))) {
				continue;
			}
			it = cfg_find(name);
			if (it && !(it->flags & CFG_HIDDEN)) {
				add_config_key(&w, &first, it);
			} else {
				jw_raw(&wu, first_unknown ? "" : ",");
				jw_str(&wu, name);
				first_unknown = false;
			}
		}
	} else {
		send_error(id, "TypeConstraintViolation", "key must be an array");
		return;
	}
	jw_raw(&w, "]");
	if (!first_unknown) {
		jw_raw(&w, ",\"unknownKey\":[");
		jw_raw(&w, unknown);
		jw_raw(&w, "]");
	}
	jw_raw(&w, "}");
	if (w.err || wu.err) {
		send_error(id, "InternalError", "response too large");
		return;
	}
	send_result(id, cfg_resp);
}

static void h_change_configuration(const char *id, int pl)
{
	char key[64], val[CFG_VAL_MAX];
	static const char *const res_str[] = {
		"Accepted", "Rejected", "RebootRequired", "NotSupported",
	};
	enum cfg_set_result r;

	if (!jp_get_str(&jp, pl, "key", key, sizeof(key)) ||
	    !jp_get_str(&jp, pl, "value", val, sizeof(val))) {
		send_error(id, "FormationViolation", "key and value (strings) are required");
		return;
	}
	r = cfg_set(key, val, true);
	snprintf(paybuf, sizeof(paybuf), "{\"status\":\"%s\"}", res_str[r]);
	send_result(id, paybuf);
	if (r == CFG_ACCEPTED && strcasecmp(key, "HeartbeatInterval") == 0) {
		heartbeat_reschedule();
	}
}

struct call_handler {
	const char *action;
	void (*fn)(const char *id, int payload_tok);
};

static const struct call_handler handlers[] = {
	{ "GetConfiguration", h_get_configuration },
	{ "ChangeConfiguration", h_change_configuration },
};

static void handle_call(const char *id, const char *action, int pl)
{
	if (boot == BOOT_REJECTED || boot == BOOT_SENT || boot == BOOT_NOT_SENT) {
		/* Spec 4.2: while Rejected the CP SHALL NOT respond. Before any answer to
		 * BootNotification we are not registered either, so we stay silent too. */
		LOG_WRN("ignoring %s while boot state is %s", action, boot_str(boot));
		return;
	}
	if (!jp_is_type(&jp, pl, JSMN_OBJECT)) {
		send_error(id, "FormationViolation", "payload must be an object");
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(handlers); i++) {
		if (strcmp(handlers[i].action, action) == 0) {
			handlers[i].fn(id, pl);
			return;
		}
	}
	send_error(id, "NotImplemented", "action not supported by this charge point");
}

/* ---------------- incoming CALLRESULT / CALLERROR ---------------- */

static void on_boot_conf(int pl)
{
	char status[16], ts[40];
	int interval = 0;

	jp_get_str(&jp, pl, "status", status, sizeof(status));
	jp_get_int(&jp, pl, "interval", &interval);
	if (jp_get_str(&jp, pl, "currentTime", ts, sizeof(ts))) {
		clock_sync_iso(ts);
	}
	if (strcmp(status, "Accepted") == 0) {
		boot = BOOT_ACCEPTED;
		if (interval > 0) {
			char v[12];

			snprintf(v, sizeof(v), "%d", interval);
			cfg_set("HeartbeatInterval", v, false);
		}
		heartbeat_reschedule();
		report_all_status();
		LOG_INF("registered (Accepted), heartbeat every %d s",
			cfg_get_int("HeartbeatInterval", 0));
		return;
	}
	boot = strcmp(status, "Pending") == 0 ? BOOT_PENDING : BOOT_REJECTED;
	if (interval <= 0) {
		interval = BOOT_RETRY_DEFAULT;
	}
	boot_retry_at = k_uptime_get() + (int64_t)interval * 1000;
	LOG_INF("BootNotification %s, retry in %d s", status, interval);
}

static void handle_result(const char *id, int pl)
{
	if (!inflight.active || strcmp(id, inflight.id) != 0) {
		LOG_WRN("CALLRESULT for unknown id %s ignored", id);
		return;
	}
	inflight.active = false;
	if (inflight.from_queue) {
		outq_pop();
	}
	if (strcmp(inflight.action, "BootNotification") == 0) {
		on_boot_conf(pl);
	} else if (strcmp(inflight.action, "Heartbeat") == 0) {
		char ts[40];

		if (jp_get_str(&jp, pl, "currentTime", ts, sizeof(ts))) {
			clock_sync_iso(ts);
		}
	}
}

static void handle_error(const char *id, int code_tok)
{
	char code[40] = "";

	if (!inflight.active || strcmp(id, inflight.id) != 0) {
		return;
	}
	jp_str(&jp, code_tok, code, sizeof(code));
	LOG_WRN("%s answered with CALLERROR %s", inflight.action, code);
	inflight.active = false;
	if (strcmp(inflight.action, "BootNotification") == 0) {
		boot = BOOT_REJECTED; /* not registered; try again later */
		boot_retry_at = k_uptime_get() + BOOT_RETRY_DEFAULT * 1000;
		return;
	}
	/* Non-transaction messages are not retried. Transaction messages: step 4. */
	if (inflight.from_queue) {
		outq_pop();
	}
}

static void dispatch(char *msg, int len)
{
	char id[40], action[40];
	int type;

	log_msg("<<", msg);
	if (jp_parse(&jp, msg, len) < 1 || !jp_is_type(&jp, 0, JSMN_ARRAY) ||
	    jp.tok[0].size < 3 || !jp_int(&jp, 1, &type) || !jp_str(&jp, 2, id, sizeof(id))) {
		LOG_WRN("not an OCPP-J message, ignored");
		return;
	}
	switch (type) {
	case 2: /* CALL */
		if (jp.tok[0].size != 4 || !jp_str(&jp, 3, action, sizeof(action))) {
			send_error(id, "ProtocolError", "CALL must be [2,id,action,payload]");
			return;
		}
		handle_call(id, action, jp_skip(&jp, 3));
		break;
	case 3: /* CALLRESULT */
		handle_result(id, 3);
		break;
	case 4: /* CALLERROR */
		handle_error(id, 3);
		break;
	default:
		LOG_WRN("unknown message type %d", type);
	}
}

/* ---------------- connection management ---------------- */

static void on_connected(void)
{
	backoff_s = BACKOFF_MIN_S;
	inflight.active = false;
	if (boot == BOOT_ACCEPTED) {
		/* Reconnect without reboot: no new BootNotification (spec 4.2),
		 * just tell the server the current state. */
		heartbeat_reschedule();
		report_all_status();
	}
}

static void on_disconnected(void)
{
	if (boot == BOOT_SENT) {
		boot = BOOT_NOT_SENT; /* answer was lost with the old connection */
	}
	inflight.active = false;
	outq_drop_non_tx();
	next_connect_at = k_uptime_get() + (int64_t)backoff_s * 1000;
	LOG_INF("offline, reconnect in %d s", backoff_s);
	backoff_s = MIN(backoff_s * 2, BACKOFF_MAX_S);
}

/*
 * The connection can drop in many places (send/receive error, CALL timeout,
 * power off, net_down). We notice the up->down change only here, so the
 * cleanup in on_disconnected() always runs exactly once.
 */
static void connection_manager(void)
{
	static bool was_up;

	if ((dead || !sim_link_up()) && ws_is_up()) {
		ws_close();
	}
	if (was_up && !ws_is_up()) {
		was_up = false;
		on_disconnected();
	}
	if (dead || !sim_link_up() || ws_is_up() || k_uptime_get() < next_connect_at) {
		return;
	}
	if (ws_connect(cfg_get(CFG_SERVER_URL), cfg_get(CFG_CP_ID)) == 0) {
		was_up = true;
		on_connected();
	} else {
		on_disconnected(); /* schedules the next attempt with back-off */
	}
}

/* ---------------- hardware events ---------------- */

static void handle_hw_event(const struct sim_evt *e)
{
	switch (e->type) {
	case SIM_EVT_POWER_OFF:
		/* No backup power (BackupPower=false): everything just stops.
		 * BackupPower=true handling comes with the resilience step. */
		LOG_WRN("mains power lost: charger stops");
		dead = true;
		ws_close();
		break;
	case SIM_EVT_POWER_ON:
		LOG_WRN("mains power back: rebooting");
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case SIM_EVT_LINK_UP:
		next_connect_at = 0; /* try now */
		backoff_s = BACKOFF_MIN_S;
		break;
	default:
		/* plug/card/fault: status is re-evaluated below; transactions in step 4 */
		LOG_INF("hw event %d on connector %d %s", e->type, e->conn, e->text);
		break;
	}
}

/* ---------------- main loop ---------------- */

static void timers(void)
{
	int64_t now = k_uptime_get();

	if (!ws_is_up()) {
		return;
	}
	if (inflight.active && now - inflight.sent_at > CALL_TIMEOUT_MS) {
		LOG_WRN("no answer to %s within %d s: connection considered broken",
			inflight.action, CALL_TIMEOUT_MS / 1000);
		ws_close(); /* connection_manager() does the cleanup */
		return;
	}
	if (!inflight.active && (boot == BOOT_NOT_SENT ||
	    ((boot == BOOT_PENDING || boot == BOOT_REJECTED) && now >= boot_retry_at))) {
		send_boot();
		return;
	}
	if (boot == BOOT_ACCEPTED && now >= next_hb_at) {
		if (!queued("Heartbeat")) {
			ocpp_enqueue("Heartbeat", "{}", false);
		}
		heartbeat_reschedule();
	}
}

static void report_status_changes(void)
{
	if (boot != BOOT_ACCEPTED) {
		return;
	}
	for (int c = 0; c <= CONN_MAX; c++) {
		if (conn_status(c) != reported[c]) {
			enqueue_status(c);
		}
	}
}

static void send_next(void)
{
	if (!ws_is_up() || boot != BOOT_ACCEPTED || inflight.active || outq_count == 0) {
		return;
	}
	struct out_msg *m = &outq[outq_head];

	send_call(m->action, m->payload, true);
}

static size_t status_hook(char *buf, size_t len)
{
	char ts[CLOCK_ISO_LEN];
	size_t n;

	clock_now_iso(ts, sizeof(ts));
	n = snprintf(buf, len, "id=%s boot=%s ws=%s clock=%s%s queue=%d", cfg_get(CFG_CP_ID),
		     boot_str(boot), ws_is_up() ? "up" : "down", ts,
		     clock_valid() ? "" : "(unsynced)", outq_count);
	for (int c = 1; c <= CONN_MAX && n < len; c++) {
		n += snprintf(buf + n, len - n, " c%d.status=%s", c, conn_status_str(conn_status(c)));
	}
	return MIN(n, len);
}

void ocpp_run(void)
{
	struct sim_evt e;

	msg_prefix = sys_rand32_get();
	conn_init();
	report_all_status();
	ctrl_set_status_hook(status_hook);
	LOG_INF("OCPP 1.6J charge point %s, server %s", cfg_get(CFG_CP_ID), cfg_get(CFG_SERVER_URL));

	while (1) {
		int n;

		if (!dead) {
			sim_alive_kick();
		}
		connection_manager();

		if (ws_is_up()) {
			n = ws_recv_text(rxbuf, sizeof(rxbuf), LOOP_MS);
			if (n > 0) {
				dispatch(rxbuf, n);
			}
		} else {
			k_msleep(LOOP_MS);
		}

		while (sim_evt_get(&e, K_NO_WAIT) == 0) {
			handle_hw_event(&e);
		}
		if (dead) {
			continue;
		}
		timers();
		report_status_changes();
		send_next();
	}
}
