/*
 * Charging session state machine, one per connector.
 *
 *   IDLE ──card + Authorize Accepted / RemoteStart──▶ AUTHORIZED ──cable in──▶ CHARGING
 *     ▲            (cable may already be in)            │ ConnectionTimeOut      │ stop: same card,
 *     │                                                 ▼                        │ RemoteStop, EV side
 *     └──────────────── cable out ◀────────────── (back to IDLE)                 ▼ disconnect, invalid id
 *     └──────────────── cable out ◀──────────────────────────────────────── FINISHING
 *
 * OCPP status (7.7) is derived from the phase + hardware:
 *   Faulted (sensor/lock fault) > Charging/SuspendedEV/SuspendedEVSE > Finishing
 *   > Preparing (authorized, or cable in) > Available
 *
 * Transaction ids: StartTransaction gets a local number (tx_local). Messages that need the
 * real transactionId (MeterValues, StopTransaction) carry the placeholder %TX% and ocpp.c
 * fills in the real id when it sends them; by then StartTransaction.conf has arrived
 * because the queue is strictly in order. (Same mechanism will serve offline transactions.)
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "ocpp/connector.h"
#include "ocpp/ocpp.h"
#include "ocpp/config.h"
#include "ocpp/clock.h"
#include "ocpp/json.h"
#include "sim/sim.h"

LOG_MODULE_REGISTER(conn, LOG_LEVEL_INF);

#define TAG_LEN       (SIM_TAG_MAX + 1)
#define TXMAP_LEN     16

enum phase { PH_IDLE, PH_AUTHORIZED, PH_CHARGING, PH_FINISHING };

struct conn {
	enum phase phase;
	char auth_tag[TAG_LEN];     /* authorized, waiting for cable */
	int64_t auth_at;            /* uptime ms of authorization (ConnectionTimeOut) */
	bool authorizing;           /* Authorize.req in flight for this connector */
	bool remote_pending;        /* RemoteStart accepted, waiting for Authorize.conf */
	char pending_tag[TAG_LEN];
	int tx_local;               /* local transaction number, 0 = none */
	char tx_tag[TAG_LEN];
	uint32_t meter_start;
	bool suspended_evse;        /* invalid id + StopTransactionOnInvalidId=false */
	bool lock_failed;           /* ConnectorLockFailure */
	int64_t next_sample_at;
	int scheduled_avail;        /* ChangeAvailability while charging: 0 none, 1 Operative, 2 Inoperative */
};

static struct conn conns[CONN_MAX + 1];
static int next_tx_local = 1;
static struct { int local; int real; } txmap[TXMAP_LEN];

static const char *const names[ST_COUNT + 1] = {
	"Available", "Preparing", "Charging", "SuspendedEV", "SuspendedEVSE",
	"Finishing", "Reserved", "Unavailable", "Faulted", "Unknown",
};

static char jbuf[1024];

void conn_init(void)
{
	memset(conns, 0, sizeof(conns));
}

static bool valid(int c)
{
	return c >= 1 && c <= CONN_MAX;
}

bool conn_valid(int c)
{
	return c >= 0 && c <= CONN_MAX;
}

/* ---------------- availability (persisted in config key _Inoperative) ---------------- */

static bool inoperative(int c)
{
	const char *list = cfg_get(CFG_INOPERATIVE);
	char tok[4];

	snprintf(tok, sizeof(tok), "%d", c);
	for (const char *p = list; p && *p;) {
		size_t n = strcspn(p, ",");

		if (n == strlen(tok) && strncmp(p, tok, n) == 0) {
			return true;
		}
		p += n;
		if (*p == ',') {
			p++;
		}
	}
	return false;
}

static void set_inoperative(int c, bool inop)
{
	char list[CFG_VAL_MAX] = "";
	size_t n = 0;

	for (int i = 0; i <= CONN_MAX; i++) {
		bool on = (i == c) ? inop : inoperative(i);

		if (on) {
			n += snprintf(list + n, sizeof(list) - n, "%s%d", n ? "," : "", i);
		}
	}
	cfg_set(CFG_INOPERATIVE, list, false);
}

static bool unavailable(int c)
{
	return inoperative(c) || inoperative(0);
}

/* ---------------- transaction id map ---------------- */

int conn_tx_real_id(int tx_local)
{
	for (int i = 0; i < TXMAP_LEN; i++) {
		if (txmap[i].local == tx_local && tx_local != 0) {
			return txmap[i].real;
		}
	}
	return -1;
}

static void txmap_set(int local, int real)
{
	int slot = local % TXMAP_LEN;

	txmap[slot].local = local;
	txmap[slot].real = real;
}

static int find_by_real_tx(int real)
{
	for (int c = 1; c <= CONN_MAX; c++) {
		if (conns[c].phase == PH_CHARGING && conn_tx_real_id(conns[c].tx_local) == real) {
			return c;
		}
	}
	return 0;
}

/* ---------------- status ---------------- */

const char *conn_status_str(enum cp_status s)
{
	return names[s <= ST_COUNT ? s : ST_COUNT];
}

enum cp_status conn_status(int c)
{
	struct sim_conn_state hw;
	struct conn *k;

	if (c == 0) {
		return inoperative(0) ? ST_UNAVAILABLE : ST_AVAILABLE;
	}
	k = &conns[c];
	sim_get(c, &hw);
	if (hw.fault[0] || k->lock_failed) {
		return ST_FAULTED;
	}
	switch (k->phase) {
	case PH_CHARGING:
		if (k->suspended_evse) {
			return ST_SUSPENDED_EVSE;
		}
		return hw.cable_ev ? ST_CHARGING : ST_SUSPENDED_EV;
	case PH_FINISHING:
		return ST_FINISHING;
	case PH_AUTHORIZED:
		return ST_PREPARING;
	default:
		if (unavailable(c)) {
			return ST_UNAVAILABLE;
		}
		return hw.cable_cs ? ST_PREPARING : ST_AVAILABLE;
	}
}

const char *conn_error_code(int c)
{
	static char code[SIM_FAULT_MAX + 1];
	struct sim_conn_state hw;

	if (!valid(c)) {
		return "NoError";
	}
	if (conns[c].lock_failed) {
		return "ConnectorLockFailure";
	}
	sim_get(c, &hw);
	if (!hw.fault[0]) {
		return "NoError";
	}
	strcpy(code, hw.fault);
	return code;
}

size_t conn_status_append(int c, char *buf, size_t len)
{
	struct conn *k = &conns[c];
	int real = conn_tx_real_id(k->tx_local);

	return snprintf(buf, len, " c%d.status=%s c%d.tx=%d c%d.tag=%s", c,
			conn_status_str(conn_status(c)), c,
			k->phase == PH_CHARGING ? real : -1, c,
			k->phase == PH_CHARGING ? k->tx_tag :
			(k->phase == PH_AUTHORIZED ? k->auth_tag : "-"));
}

/* ---------------- messages ---------------- */

static void send_authorize(int c, const char *tag)
{
	struct jw w;
	bool first = true;

	jw_init(&w, jbuf, sizeof(jbuf));
	jw_raw(&w, "{");
	jw_kv_str(&w, &first, "idTag", tag);
	jw_raw(&w, "}");
	conns[c].authorizing = true;
	ocpp_enqueue_ex("Authorize", jbuf, false, c, tag, 0);
}

/* One MeterValue element with every configured measurand. */
static void meter_value(struct jw *w, int c, const char *context, const char *keylist)
{
	struct sim_conn_state hw;
	char ts[CLOCK_ISO_LEN], list[CFG_VAL_MAX], *save, *m;
	bool first_sv = true;

	sim_get(c, &hw);
	clock_now_iso(ts, sizeof(ts));
	strncpy(list, keylist, sizeof(list) - 1);
	list[sizeof(list) - 1] = '\0';
	jw_printf(w, "{\"timestamp\":\"%s\",\"sampledValue\":[", ts);
	for (m = strtok_r(list, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
		char val[16];
		const char *unit;

		while (*m == ' ') {
			m++;
		}
		if (strcmp(m, "Energy.Active.Import.Register") == 0) {
			snprintf(val, sizeof(val), "%u", (unsigned)(hw.energy_mwh / 1000));
			unit = "Wh";
		} else if (strcmp(m, "Power.Active.Import") == 0) {
			snprintf(val, sizeof(val), "%d", hw.power_w);
			unit = "W";
		} else if (strcmp(m, "Current.Import") == 0) {
			snprintf(val, sizeof(val), "%d", hw.current_a);
			unit = "A";
		} else if (strcmp(m, "Voltage") == 0) {
			snprintf(val, sizeof(val), "%d", hw.voltage_v);
			unit = "V";
		} else {
			continue;
		}
		jw_printf(w, "%s{\"value\":\"%s\",\"context\":\"%s\",\"format\":\"Raw\","
			  "\"measurand\":\"%s\",\"location\":\"Outlet\",\"unit\":\"%s\"}",
			  first_sv ? "" : ",", val, context, m, unit);
		first_sv = false;
	}
	jw_raw(w, "]}");
}

static void send_meter_values(int c)
{
	struct jw w;
	const char *list = cfg_get("MeterValuesSampledData");

	if (!list || !*list) {
		return;
	}
	jw_init(&w, jbuf, sizeof(jbuf));
	jw_printf(&w, "{\"connectorId\":%d,\"transactionId\":%%TX%%,\"meterValue\":[", c);
	meter_value(&w, c, "Sample.Periodic", list);
	jw_raw(&w, "]}");
	if (!w.err) {
		ocpp_enqueue_ex("MeterValues", jbuf, true, c, NULL, conns[c].tx_local);
	}
}

static void start_transaction(int c, const char *tag)
{
	struct conn *k = &conns[c];
	struct jw w;
	char ts[CLOCK_ISO_LEN];
	int interval;

	if (!sim_lock(c)) {
		/* DESIGN_core 8 / TC_024: lock motor failed -> do not start, report fault */
		LOG_WRN("connector %d: lock failed, transaction not started", c);
		k->lock_failed = true;
		k->phase = PH_IDLE;
		return;
	}
	k->phase = PH_CHARGING;
	k->tx_local = next_tx_local++;
	strcpy(k->tx_tag, tag);
	k->meter_start = sim_meter_wh(c);
	k->suspended_evse = false;
	interval = cfg_get_int("MeterValueSampleInterval", 0);
	k->next_sample_at = interval > 0 ? k_uptime_get() + (int64_t)interval * 1000 : INT64_MAX;
	sim_set_relay(c, true);

	clock_now_iso(ts, sizeof(ts));
	jw_init(&w, jbuf, sizeof(jbuf));
	jw_printf(&w, "{\"connectorId\":%d,\"idTag\":", c);
	jw_str(&w, tag);
	jw_printf(&w, ",\"meterStart\":%u,\"timestamp\":\"%s\"}", k->meter_start, ts);
	ocpp_enqueue_ex("StartTransaction", jbuf, true, c, tag, k->tx_local);
	LOG_INF("connector %d: transaction #%d started for %s", c, k->tx_local, tag);
}

static void stop_transaction(int c, const char *reason, const char *tag)
{
	struct conn *k = &conns[c];
	struct sim_conn_state hw;
	struct jw w;
	char ts[CLOCK_ISO_LEN];

	if (k->phase != PH_CHARGING) {
		return;
	}
	sim_set_relay(c, false);
	clock_now_iso(ts, sizeof(ts));
	jw_init(&w, jbuf, sizeof(jbuf));
	jw_raw(&w, "{\"transactionId\":%TX%");
	if (tag) {
		jw_raw(&w, ",\"idTag\":");
		jw_str(&w, tag);
	}
	jw_printf(&w, ",\"meterStop\":%u,\"timestamp\":\"%s\",\"reason\":\"%s\"}",
		  sim_meter_wh(c), ts, reason);
	ocpp_enqueue_ex("StopTransaction", jbuf, true, c, tag, k->tx_local);
	LOG_INF("connector %d: transaction #%d stopped (%s)", c, k->tx_local, reason);

	/* Detachable cable: unlock so the driver can take it out. */
	sim_unlock(c);
	sim_get(c, &hw);
	k->phase = hw.cable_cs ? PH_FINISHING : PH_IDLE;
	k->tx_tag[0] = '\0';
	k->auth_tag[0] = '\0';
	if (k->scheduled_avail) {
		set_inoperative(c, k->scheduled_avail == 2);
		k->scheduled_avail = 0;
	}
}

/* Authorized (by card or remote start): start now if the cable is in, else wait. */
static void authorized(int c, const char *tag)
{
	struct conn *k = &conns[c];
	struct sim_conn_state hw;

	sim_get(c, &hw);
	if (hw.cable_cs && hw.cable_ev) {
		start_transaction(c, tag);
		return;
	}
	k->phase = PH_AUTHORIZED;
	strcpy(k->auth_tag, tag);
	k->auth_at = k_uptime_get();
	LOG_INF("connector %d: %s authorized, waiting for cable (ConnectionTimeOut %d s)", c, tag,
		cfg_get_int("ConnectionTimeOut", 0));
}

/* ---------------- hardware events ---------------- */

static void on_card(int c, const char *tag)
{
	struct conn *k = &conns[c];

	if (conn_status(c) == ST_FAULTED || (k->phase != PH_CHARGING && unavailable(c))) {
		LOG_WRN("connector %d faulted/unavailable: card %s ignored", c, tag);
		return;
	}
	switch (k->phase) {
	case PH_CHARGING:
		if (strcmp(tag, k->tx_tag) == 0) {
			stop_transaction(c, "Local", tag);  /* TC_068: same card stops */
		} else {
			LOG_INF("connector %d: card %s is not the one that started the transaction", c, tag);
		}
		return;
	case PH_AUTHORIZED:
		if (strcmp(tag, k->auth_tag) == 0) {
			LOG_INF("connector %d: same card again, authorization cancelled", c);
			k->phase = PH_IDLE;
		}
		return;
	case PH_FINISHING:
		return;
	default:
		break;
	}
	if (k->authorizing) {
		return;
	}
	if (!ocpp_online()) {
		/* Offline authorization (cache / local list) comes with the resilience step. */
		LOG_WRN("offline: cannot authorize %s", tag);
		return;
	}
	send_authorize(c, tag);
}

void conn_on_hw(const struct sim_evt *e)
{
	int c = e->conn;
	struct conn *k;

	if (!valid(c)) {
		return;
	}
	k = &conns[c];
	switch (e->type) {
	case SIM_EVT_PLUG:
		k->lock_failed = false;
		if (k->phase == PH_AUTHORIZED) {
			start_transaction(c, k->auth_tag);
		}
		break;
	case SIM_EVT_UNPLUG:
		k->lock_failed = false;
		if (k->phase == PH_CHARGING) {
			stop_transaction(c, "EVDisconnected", NULL);
		}
		if (k->phase == PH_FINISHING) {
			k->phase = PH_IDLE;
		}
		break;
	case SIM_EVT_EV_DISCONNECT:
		if (k->phase == PH_CHARGING && cfg_get_bool("StopTransactionOnEVSideDisconnect")) {
			stop_transaction(c, "EVDisconnected", NULL);
		} else if (k->phase == PH_CHARGING) {
			sim_set_relay(c, false);  /* stays as SuspendedEV */
		}
		if (cfg_get_bool("UnlockConnectorOnEVSideDisconnect")) {
			sim_unlock(c);
		}
		break;
	case SIM_EVT_CARD:
		on_card(c, e->text);
		break;
	case SIM_EVT_FAULT:
		if (k->phase == PH_CHARGING) {
			stop_transaction(c, "Other", NULL);
		}
		k->phase = (k->phase == PH_AUTHORIZED) ? PH_IDLE : k->phase;
		break;
	default:
		break;
	}
}

/* ---------------- remote control ---------------- */

bool conn_remote_start(int c, const char *tag)
{
	struct conn *k;

	if (c == 0) {
		c = 1; /* connectorId omitted: first connector (we have one) */
	}
	if (!valid(c)) {
		return false;
	}
	k = &conns[c];
	/* DESIGN_core 8: refuse if busy, faulted or unavailable */
	if (k->phase != PH_IDLE || k->authorizing || conn_status(c) == ST_FAULTED || unavailable(c)) {
		return false;
	}
	if (cfg_get_bool("AuthorizeRemoteTxRequests")) {
		k->remote_pending = true;
		strcpy(k->pending_tag, tag);
		send_authorize(c, tag);
	} else {
		authorized(c, tag);
	}
	return true;
}

bool conn_remote_stop(int transaction_id)
{
	int c = find_by_real_tx(transaction_id);

	if (!c) {
		return false;
	}
	stop_transaction(c, "Remote", NULL);
	return true;
}

enum avail_result conn_change_availability(int c, bool operative)
{
	bool busy = false;

	if (!conn_valid(c)) {
		return AVAIL_REJECTED;
	}
	for (int i = 1; i <= CONN_MAX; i++) {
		if ((c == 0 || i == c) && conns[i].phase == PH_CHARGING) {
			busy = true;
			if (!operative) {
				conns[i].scheduled_avail = 2; /* apply when the transaction ends */
			}
		}
	}
	if (busy && !operative) {
		if (c == 0) {
			set_inoperative(0, true);  /* other connectors go Unavailable at once */
		}
		return AVAIL_SCHEDULED;
	}
	set_inoperative(c, !operative);
	if (c == 0 && operative) {
		for (int i = 1; i <= CONN_MAX; i++) {
			set_inoperative(i, false);
		}
	}
	return AVAIL_ACCEPTED;
}

const char *conn_unlock(int c)
{
	if (!valid(c)) {
		return "NotSupported";
	}
	if (conns[c].phase == PH_CHARGING) {
		stop_transaction(c, "UnlockCommand", NULL); /* spec 5.17: stop first, then unlock */
	}
	return sim_unlock(c) ? "Unlocked" : "UnlockFailed";
}

int conn_stop_all(const char *reason)
{
	int n = 0;

	for (int c = 1; c <= CONN_MAX; c++) {
		if (conns[c].phase == PH_CHARGING) {
			stop_transaction(c, reason, NULL);
			n++;
		}
	}
	return n;
}

/* ---------------- answers ---------------- */

void conn_on_authorize_conf(int c, const char *tag, const char *status)
{
	struct conn *k;

	if (!valid(c)) {
		return;
	}
	k = &conns[c];
	k->authorizing = false;
	k->remote_pending = false;
	if (strcmp(status, "Accepted") != 0) {
		LOG_INF("connector %d: %s not authorized (%s)", c, tag, status);
		return;
	}
	if (k->phase == PH_IDLE) {
		authorized(c, tag);
	}
}

void conn_on_start_conf(int tx_local, int transaction_id, const char *status)
{
	txmap_set(tx_local, transaction_id);
	for (int c = 1; c <= CONN_MAX; c++) {
		struct conn *k = &conns[c];

		if (k->phase != PH_CHARGING || k->tx_local != tx_local) {
			continue;
		}
		LOG_INF("connector %d: transaction #%d has id %d (%s)", c, tx_local, transaction_id,
			status);
		if (strcmp(status, "Accepted") == 0) {
			return;
		}
		/* spec 3.4.4 / TC_037: id not valid after all */
		if (cfg_get_bool("StopTransactionOnInvalidId")) {
			stop_transaction(c, "DeAuthorized", NULL);
		} else {
			k->suspended_evse = true;
			sim_set_relay(c, false);
		}
	}
}

void conn_on_call_failed(const char *action, int c, int tx_local)
{
	if (!valid(c)) {
		return;
	}
	if (strcmp(action, "Authorize") == 0) {
		conns[c].authorizing = false;
		conns[c].remote_pending = false;
	}
	ARG_UNUSED(tx_local);
}

/* ---------------- timers ---------------- */

void conn_tick(void)
{
	int64_t now = k_uptime_get();

	for (int c = 1; c <= CONN_MAX; c++) {
		struct conn *k = &conns[c];

		if (k->phase == PH_AUTHORIZED) {
			int t = cfg_get_int("ConnectionTimeOut", 0);

			if (t > 0 && now - k->auth_at >= (int64_t)t * 1000) {
				LOG_INF("connector %d: no cable within %d s, authorization cancelled", c, t);
				k->phase = PH_IDLE;
				k->auth_tag[0] = '\0';
			}
		}
		if (k->phase == PH_CHARGING && now >= k->next_sample_at) {
			int interval = cfg_get_int("MeterValueSampleInterval", 0);

			send_meter_values(c);
			k->next_sample_at = interval > 0 ? k->next_sample_at + (int64_t)interval * 1000
							 : INT64_MAX;
		}
	}
}
