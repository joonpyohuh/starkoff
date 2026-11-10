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
#include "ocpp/ocpp.h"
#include "ctrl/ctrl.h"
#include "net/ws.h"
#include "ocpp/auth.h"
#include "ocpp/clock.h"
#include "ocpp/config.h"
#include "ocpp/connector.h"
#include "ocpp/json.h"
#include "sim/sim.h"
#include "store/store.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_REGISTER(ocpp, LOG_LEVEL_INF);

#define FW_VERSION "0.3.0"
#define CALL_TIMEOUT_MS 30000 /* no answer -> treat connection as broken */
#define BOOT_RETRY_DEFAULT 30 /* s, when the server gives interval 0 */
#define BACKOFF_MIN_S 5
#define BACKOFF_MAX_S 60
#define LOOP_MS 50

#define OUTQ_LEN 16
#define OUT_PAYLOAD_MAX 1024
#define RX_MAX 8192
#define TX_MAX 4096
#define LOG_MSG_MAX 300

/* ---------------- state ---------------- */

static enum boot_state boot = BOOT_NOT_SENT;
static int64_t boot_retry_at; /* uptime ms: earliest next BootNotification */
static int64_t next_hb_at;    /* uptime ms: next Heartbeat */
/* First attempt ~2 s after boot: by then QEMU's NAT has learned our MAC (link.c),
 * so the first SYN is answered instead of being retransmitted for several seconds. */
static int64_t next_connect_at = 2000;
static int backoff_s = BACKOFF_MIN_S;
static int64_t power_down_at;
static bool dead; /* power is off: charger logic stopped */
static enum cp_status reported[CONN_MAX + 1];
static bool reported_conflict;
static enum cp_status stable_status[CONN_MAX + 1];
static int64_t stable_since[CONN_MAX + 1];

struct out_msg {
	char action[32];
	char payload[OUT_PAYLOAD_MAX]; /* may contain %TX% = real transactionId, filled at send */
	bool tx_related;	       /* transaction message: kept across reconnects */
	int conn;		       /* connector the message belongs to (answer routing) */
	char tag[21];		       /* idTag, for Authorize answers */
	int tx_local;		       /* local transaction number (connector.c) */
	int failures;
	int64_t retry_at;
	bool triggered;
};
static bool handling_trigger;
static struct out_msg outq[OUTQ_LEN];
static int outq_head, outq_count;

static struct {
	bool active;
	bool from_queue; /* true: queue head; false: BootNotification sent directly */
	char id[40];
	char action[32];
	int64_t sent_at;
	int conn;
	char tag[21];
	int tx_local;
} inflight;

/* Reset requested by the Central System: reboot once the queue is sent (or at the deadline). */
static struct {
	bool pending;
	int64_t deadline;
} reset_req;

static uint32_t msg_counter;
static uint32_t msg_prefix;
static char rxbuf[RX_MAX];
static char txbuf[TX_MAX];
static char paybuf[OUT_PAYLOAD_MAX];
static char sendbuf[OUT_PAYLOAD_MAX + 16];
static struct jp jp;

/* One A/B journal atomically records queue, transaction ids and the simulated world. */
static struct checkpoint {
	uint32_t version;
	int count;
	size_t conn_bytes;
	struct out_msg messages[OUTQ_LEN];
	uint8_t connectors[2048] __aligned(8);
} snapshot, previous_snapshot;
static bool recovered;
static void checkpoint_save(void)
{
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.version = 1;
	snapshot.conn_bytes =
	    conn_checkpoint_save(snapshot.connectors, sizeof(snapshot.connectors));
	for (int i = 0; i < outq_count; i++) {
		struct out_msg *m = &outq[(outq_head + i) % OUTQ_LEN];
		if (m->tx_related)
			snapshot.messages[snapshot.count++] = *m;
	}
	if (memcmp(&snapshot, &previous_snapshot, sizeof(snapshot)) &&
	    !store_save(STORE_REC_QUEUE, &snapshot, sizeof(snapshot)))
		previous_snapshot = snapshot;
}
static void checkpoint_load(void)
{
	if (store_load(STORE_REC_QUEUE, &snapshot, sizeof(snapshot)) != sizeof(snapshot) ||
	    snapshot.version != 1 || snapshot.count < 0 || snapshot.count > OUTQ_LEN ||
	    snapshot.conn_bytes > sizeof(snapshot.connectors))
		return;
	outq_count = snapshot.count;
	outq_head = 0;
	memcpy(outq, snapshot.messages, sizeof(outq));
	for (int i = 0; i < outq_count; i++)
		outq[i].retry_at = 0;
	conn_checkpoint_load(snapshot.connectors, snapshot.conn_bytes);
	previous_snapshot = snapshot;
	recovered = true;
}

static const char *boot_str(enum boot_state b)
{
	static const char *const s[] = {"NotSent", "Sent", "Accepted", "Pending", "Rejected"};

	return s[b];
}

enum boot_state ocpp_boot_state(void)
{
	return boot;
}

/* ---------------- logging & sending ---------------- */

static void log_msg(const char *dir, const char *m)
{
	/* Zephyr deferred logging copies the full string even with a printf precision. */
	char preview[LOG_MSG_MAX + 1];
	size_t n = MIN(strlen(m), LOG_MSG_MAX);
	memcpy(preview, m, n);
	preview[n] = 0;
	LOG_INF("%s %s%s", dir, preview, strlen(m) > n ? "..." : "");
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

bool ocpp_online(void)
{
	return ws_is_up() && boot == BOOT_ACCEPTED;
}

/* ---------------- outgoing queue ---------------- */

int ocpp_enqueue(const char *action, const char *payload, bool tx_related)
{
	return ocpp_enqueue_ex(action, payload, tx_related, 0, NULL, 0);
}

int ocpp_enqueue_ex(const char *action, const char *payload, bool tx_related, int conn,
		    const char *tag, int tx_local)
{
	struct out_msg *m;

	if (outq_count == OUTQ_LEN ||
	    (outq_count >= OUTQ_LEN - 2 && strcmp(action, "StopTransaction"))) {
		LOG_ERR("outgoing queue full, %s dropped", action);
		return -ENOMEM;
	}
	if (strlen(payload) >= OUT_PAYLOAD_MAX) {
		LOG_ERR("%s payload too large, dropped", action);
		return -EMSGSIZE;
	}
	int insert = outq_count;
	/* Pending allows only messages explicitly requested by TriggerMessage.
	 * Put those ahead of the persisted transaction backlog, without moving an in-flight head.
	 */
	if (boot == BOOT_PENDING && handling_trigger) {
		insert = (inflight.active && inflight.from_queue) ? 1 : 0;
		for (int i = outq_count; i > insert; i--)
			outq[(outq_head + i) % OUTQ_LEN] = outq[(outq_head + i - 1) % OUTQ_LEN];
	}
	m = &outq[(outq_head + insert) % OUTQ_LEN];
	strncpy(m->action, action, sizeof(m->action) - 1);
	m->action[sizeof(m->action) - 1] = '\0';
	strcpy(m->payload, payload);
	m->tx_related = tx_related;
	m->failures = 0;
	m->retry_at = 0;
	m->triggered = handling_trigger;
	m->conn = conn;
	strncpy(m->tag, tag ? tag : "", sizeof(m->tag) - 1);
	m->tag[sizeof(m->tag) - 1] = '\0';
	m->tx_local = tx_local;
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
	inflight.conn = 0;
	inflight.tag[0] = '\0';
	inflight.tx_local = 0;
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
	if (s == ST_SUSPENDED_EV)
		jw_kv_str(&w, &first, "info", "EV side disconnected");
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
	    "Accepted",
	    "Rejected",
	    "RebootRequired",
	    "NotSupported",
	};
	enum cfg_set_result r;

	if (!jp_get_str(&jp, pl, "key", key, sizeof(key)) ||
	    !jp_get_str(&jp, pl, "value", val, sizeof(val))) {
		send_error(id, "FormationViolation", "key and value (strings) are required");
		return;
	}
	r = cfg_set(key, val, true);
	if (r == CFG_ACCEPTED && (!strcasecmp(key, "MeterValueSampleInterval") ||
				  !strcasecmp(key, "ClockAlignedDataInterval")))
		conn_meter_config_changed();
	snprintf(paybuf, sizeof(paybuf), "{\"status\":\"%s\"}", res_str[r]);
	send_result(id, paybuf);
	if (r == CFG_ACCEPTED && strcasecmp(key, "HeartbeatInterval") == 0) {
		heartbeat_reschedule();
	}
}

static void reply_status(const char *id, const char *status)
{
	snprintf(paybuf, sizeof(paybuf), "{\"status\":\"%s\"}", status);
	send_result(id, paybuf);
}

static void h_remote_start(const char *id, int pl)
{
	char tag[21];
	int c = 0;

	if (!jp_get_str(&jp, pl, "idTag", tag, sizeof(tag))) {
		send_error(id, "FormationViolation", "idTag (string, max 20) is required");
		return;
	}
	if (jp_get(&jp, pl, "connectorId") >= 0 && !jp_get_int(&jp, pl, "connectorId", &c)) {
		send_error(id, "TypeConstraintViolation", "connectorId must be an integer");
		return;
	}
	bool given = jp_get(&jp, pl, "connectorId") >= 0;

	/* Pending (spec 4.2), connectorId 0 or a connector we do not have (DESIGN_core 6/8):
	 * Rejected */
	if (boot != BOOT_ACCEPTED || (given && (c < 1 || c > CONN_MAX))) {
		reply_status(id, "Rejected");
		return;
	}
	if (!given) {
		c = 1; /* no connectorId: we choose (first connector) */
	}
	/* The .conf is sent now; Authorize/StartTransaction are queued and follow it. */
	reply_status(id, conn_remote_start(c, tag) ? "Accepted" : "Rejected");
}

static void h_remote_stop(const char *id, int pl)
{
	int tx;

	if (!jp_get_int(&jp, pl, "transactionId", &tx)) {
		send_error(id, "FormationViolation", "transactionId (integer) is required");
		return;
	}
	reply_status(id, boot == BOOT_ACCEPTED && conn_remote_stop(tx) ? "Accepted" : "Rejected");
}

static void h_change_availability(const char *id, int pl)
{
	char type[16];
	int c;
	static const char *const res[] = {"Accepted", "Rejected", "Scheduled"};

	if (!jp_get_int(&jp, pl, "connectorId", &c) ||
	    !jp_get_str(&jp, pl, "type", type, sizeof(type))) {
		send_error(id, "FormationViolation", "connectorId and type are required");
		return;
	}
	if (strcmp(type, "Operative") != 0 && strcmp(type, "Inoperative") != 0) {
		send_error(id, "PropertyConstraintViolation",
			   "type must be Operative or Inoperative");
		return;
	}
	reply_status(id, res[conn_change_availability(c, strcmp(type, "Operative") == 0)]);
}

static void h_reset(const char *id, int pl)
{
	char type[8];
	bool hard;

	if (!jp_get_str(&jp, pl, "type", type, sizeof(type)) ||
	    (strcmp(type, "Hard") != 0 && strcmp(type, "Soft") != 0)) {
		send_error(id, "FormationViolation", "type must be Hard or Soft");
		return;
	}
	hard = strcmp(type, "Hard") == 0;
	reply_status(id, "Accepted");
	/* Stop transactions first (reason HardReset / SoftReset), then reboot when the
	 * StopTransaction is delivered. Hard: at most 5 s, Soft: at most 30 s. */
	conn_stop_all(hard ? "HardReset" : "SoftReset");
	reset_req.pending = true;
	reset_req.deadline = k_uptime_get() + (hard ? 5000 : 30000);
	LOG_WRN("%s reset requested", type);
}

static void h_unlock(const char *id, int pl)
{
	int c;

	if (!jp_get_int(&jp, pl, "connectorId", &c)) {
		send_error(id, "FormationViolation", "connectorId (integer) is required");
		return;
	}
	reply_status(id, conn_unlock(c));
}

static void h_local_version(const char *id, int pl)
{
	ARG_UNUSED(pl);
	snprintf(paybuf, sizeof(paybuf), "{\"listVersion\":%d}", auth_version());
	send_result(id, paybuf);
}
static void h_local_list(const char *id, int pl)
{
	reply_status(id, auth_list_update(&jp, pl));
}
static void h_clear_cache(const char *id, int pl)
{
	ARG_UNUSED(pl);
	auth_clear_cache();
	reply_status(id, "Accepted");
}
static void h_data_transfer(const char *id, int pl)
{
	char vendor[256];
	if (!jp_get_str(&jp, pl, "vendorId", vendor, sizeof(vendor))) {
		send_error(id, "FormationViolation", "vendorId required");
		return;
	}
	reply_status(id, "UnknownVendorId");
}
static void h_reserve(const char *id, int pl)
{
	int c, r;
	int64_t expiry;
	char ts[40], tag[21], parent[21] = "";
	if (!jp_get_int(&jp, pl, "connectorId", &c) || !jp_get_int(&jp, pl, "reservationId", &r) ||
	    !jp_get_str(&jp, pl, "idTag", tag, sizeof(tag)) ||
	    !jp_get_str(&jp, pl, "expiryDate", ts, sizeof(ts)) || !clock_parse_iso(ts, &expiry) ||
	    (jp_get(&jp, pl, "parentIdTag") >= 0 &&
	     !jp_get_str(&jp, pl, "parentIdTag", parent, sizeof(parent)))) {
		send_error(id, "FormationViolation", "invalid reservation fields");
		return;
	}
	reply_status(id, conn_reserve(c, r, tag, parent, expiry));
}
static void h_cancel_reservation(const char *id, int pl)
{
	int r;
	if (!jp_get_int(&jp, pl, "reservationId", &r)) {
		send_error(id, "FormationViolation", "reservationId required");
		return;
	}
	reply_status(id, conn_cancel_reservation(r) ? "Accepted" : "Rejected");
}
static bool trigger_boot;
static void h_trigger(const char *id, int pl)
{
	char action[40];
	int c = 0;
	bool given = jp_get(&jp, pl, "connectorId") >= 0;
	if (!jp_get_str(&jp, pl, "requestedMessage", action, sizeof(action)) ||
	    (given && !jp_get_int(&jp, pl, "connectorId", &c))) {
		send_error(id, "FormationViolation", "invalid trigger fields");
		return;
	}
	bool status = !strcmp(action, "StatusNotification"), meter = !strcmp(action, "MeterValues");
	if ((status || meter) && given && (!conn_valid(c) || (meter && c == 0))) {
		reply_status(id, "Rejected");
		return;
	}
	if (!status && !meter && strcmp(action, "Heartbeat") &&
	    strcmp(action, "BootNotification")) {
		reply_status(id, "NotImplemented");
		return;
	}
	reply_status(id, "Accepted");
	handling_trigger = true;
	if (status) {
		if (given)
			enqueue_status(c);
		else
			for (int i = 0; i <= CONN_MAX; i++)
				enqueue_status(i);
	} else if (meter)
		conn_trigger_meter(c);
	else if (!strcmp(action, "Heartbeat"))
		ocpp_enqueue("Heartbeat", "{}", false);
	else
		trigger_boot = true;
	handling_trigger = false;
}

struct call_handler {
	const char *action;
	void (*fn)(const char *id, int payload_tok);
};

static const struct call_handler handlers[] = {
    {"GetConfiguration", h_get_configuration},
    {"ChangeConfiguration", h_change_configuration},
    {"RemoteStartTransaction", h_remote_start},
    {"RemoteStopTransaction", h_remote_stop},
    {"ChangeAvailability", h_change_availability},
    {"Reset", h_reset},
    {"UnlockConnector", h_unlock},
    {"GetLocalListVersion", h_local_version},
    {"SendLocalList", h_local_list},
    {"ClearCache", h_clear_cache},
    {"ReserveNow", h_reserve},
    {"CancelReservation", h_cancel_reservation},
    {"TriggerMessage", h_trigger},
    {"DataTransfer", h_data_transfer},
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
	char status[16] = "", ts[40];
	int interval = 0;

	jp_get_str(&jp, pl, "status", status, sizeof(status));
	jp_get_int(&jp, pl, "interval", &interval);
	if (jp_get_str(&jp, pl, "currentTime", ts, sizeof(ts))) {
		clock_sync_iso(ts);
		conn_clock_changed();
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
		if (recovered) {
			recovered = false;
			conn_stop_all("PowerLoss");
		}
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
	} else if (strcmp(inflight.action, "Authorize") == 0 ||
		   strcmp(inflight.action, "StartTransaction") == 0) {
		char status[24] = "Invalid", parent[21] = "";
		int info = jp_get(&jp, pl, "idTagInfo");
		int tx = -1;

		jp_get_str(&jp, info, "status", status, sizeof(status));
		jp_get_str(&jp, info, "parentIdTag", parent, sizeof(parent));
		auth_update(inflight.tag, &jp, info);
		if (inflight.action[0] == 'A') {
			conn_on_authorize_conf(inflight.conn, inflight.tag, status, parent);
		} else {
			jp_get_int(&jp, pl, "transactionId", &tx);
			conn_on_start_conf(inflight.tx_local, tx, status, parent);
		}
	} else if (!strcmp(inflight.action, "StopTransaction")) {
		auth_update(inflight.tag, &jp, jp_get(&jp, pl, "idTagInfo"));
	} else if (strcmp(inflight.action, "Heartbeat") == 0) {
		char ts[40];

		if (jp_get_str(&jp, pl, "currentTime", ts, sizeof(ts))) {
			clock_sync_iso(ts);
			conn_clock_changed();
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
	if (inflight.from_queue && outq_count && outq[outq_head].tx_related) {
		struct out_msg *m = &outq[outq_head];
		m->failures++;
		if (m->failures < cfg_get_int("TransactionMessageAttempts", 3)) {
			m->retry_at = k_uptime_get() +
				      (int64_t)cfg_get_int("TransactionMessageRetryInterval", 60) *
					  1000 * m->failures;
			return;
		}
	}
	conn_on_call_failed(inflight.action, inflight.conn, inflight.tx_local);
	if (inflight.from_queue) {
		outq_pop();
	}
}

static void dispatch(char *msg, int len)
{
	char id[40], action[40];
	int type;

	log_msg("<<", msg);
	if (jp_parse(&jp, msg, len) < 1 || !jp_is_type(&jp, 0, JSMN_ARRAY) || jp.tok[0].size < 3 ||
	    !jp_int(&jp, 1, &type) || !jp_str(&jp, 2, id, sizeof(id))) {
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
		/* Commit the stop before simulated shutdown. Backup power permits up to
		 * five seconds to deliver it; otherwise replay only after the next boot. */
		LOG_WRN("mains power lost: charger stops");
		conn_stop_all("PowerLoss");
		checkpoint_save();
		if (cfg_get_bool("BackupPower")) {
			power_down_at = k_uptime_get() + 5000;
		} else {
			dead = true;
			ws_close();
		}
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
		LOG_INF("hw event %d on connector %d %s", e->type, e->conn, e->text);
		conn_on_hw(e);
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
	if (!inflight.active &&
	    (trigger_boot || boot == BOOT_NOT_SENT ||
	     ((boot == BOOT_PENDING || boot == BOOT_REJECTED) && now >= boot_retry_at))) {
		trigger_boot = false;
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
	if (!ocpp_online()) {
		return;
	}
	for (int c = 0; c <= CONN_MAX; c++) {
		enum cp_status current = conn_status(c);
		if (current != stable_status[c]) {
			stable_status[c] = current;
			stable_since[c] = k_uptime_get();
		}
		if ((current != reported[c] || (c == 0 && reported_conflict != auth_conflict())) &&
		    k_uptime_get() - stable_since[c] >=
			(int64_t)cfg_get_int("MinimumStatusDuration", 0) * 1000) {
			enqueue_status(c);
			if (c == 0)
				reported_conflict = auth_conflict();
		}
	}
}

static void send_next(void)
{
	if (!ws_is_up() || (boot != BOOT_ACCEPTED && boot != BOOT_PENDING) || inflight.active ||
	    outq_count == 0) {
		return;
	}
	struct out_msg *m = &outq[outq_head];
	if ((boot == BOOT_PENDING && !m->triggered) || k_uptime_get() < m->retry_at)
		return;
	const char *ph = strstr(m->payload, "%TX%");

	if (ph) {
		int real = conn_tx_real_id(m->tx_local);

		if (real < 0) {
			/* StartTransaction was refused with CALLERROR: no transactionId exists */
			LOG_ERR("%s for transaction #%d has no transactionId, dropped", m->action,
				m->tx_local);
			outq_pop();
			return;
		}
		snprintf(sendbuf, sizeof(sendbuf), "%.*s%d%s", (int)(ph - m->payload), m->payload,
			 real, ph + 4);
	}
	inflight.conn = m->conn;
	strcpy(inflight.tag, m->tag);
	inflight.tx_local = m->tx_local;
	send_call(m->action, ph ? sendbuf : m->payload, true);
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
		n += conn_status_append(c, buf + n, len - n);
	}
	return MIN(n, len);
}

/*
 * Software watchdog (docs/DESIGN_resilience.md 3-2). The main loop feeds it every pass.
 * If the loop is stuck for WDT_TIMEOUT_MS the charger reboots by itself.
 * 30 s: the longest legitimate block is a connection attempt (10 s TCP + 5 s handshake).
 */
#define WDT_TIMEOUT_MS 30000

static void wdt_expired(int channel_id, void *user_data)
{
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);
	/* The log is deferred (printk too): switch it to synchronous and flush first,
	 * otherwise the reboot wipes the message and nobody can tell why it rebooted. */
	LOG_PANIC();
	LOG_ERR("watchdog: main loop stuck for %d s, rebooting", WDT_TIMEOUT_MS / 1000);
	sys_reboot(SYS_REBOOT_COLD);
}

void ocpp_run(void)
{
	struct sim_evt e;
	int wdt = -1;

	msg_prefix = sys_rand32_get();
	conn_init();
	checkpoint_load();
	report_all_status();
	ctrl_set_status_hook(status_hook);
	LOG_INF("OCPP 1.6J charge point %s, server %s", cfg_get(CFG_CP_ID),
		cfg_get(CFG_SERVER_URL));

	if (task_wdt_init(NULL) == 0) {
		wdt = task_wdt_add(WDT_TIMEOUT_MS, wdt_expired, NULL);
	}
	if (wdt < 0) {
		LOG_ERR("software watchdog not available");
	}

	while (1) {
		int n;

		while (sim_hung()) {
			k_msleep(100); /* simulated CPU stall: no alive signal, no watchdog feed */
		}
		if (wdt >= 0) {
			task_wdt_feed(wdt);
		}
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
		conn_tick();
		report_status_changes();
		send_next();
		checkpoint_save();
		if (power_down_at &&
		    (k_uptime_get() >= power_down_at || (!outq_count && !inflight.active))) {
			dead = true;
			power_down_at = 0;
			ws_close();
		}
		if (reset_req.pending && ((outq_count == 0 && !inflight.active) ||
					  k_uptime_get() > reset_req.deadline)) {
			LOG_WRN("rebooting for Reset (%d messages left in queue)", outq_count);
			k_msleep(200);
			sys_reboot(SYS_REBOOT_COLD);
		}
	}
}
