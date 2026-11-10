/*
 * Simulated hardware. See sim.h for the idea.
 *
 * Thread safety: the control thread, the shell thread and the charger thread all
 * touch this state, so every access goes through sim_lock_mtx.
 * Events go through a message queue so the charger logic handles them one by one
 * in its own thread (like an interrupt that only sets a flag).
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "sim/sim.h"

LOG_MODULE_REGISTER(sim, LOG_LEVEL_INF);

#define CHARGE_POWER_W  7400   /* 32 A single phase */
#define NOMINAL_V       230
#define NOMINAL_TEMP_C  35

/* Safe operating range. Outside it = fault (range check, docs/DESIGN_resilience.md 3-3). */
#define MAX_CURRENT_A   40
#define MAX_VOLTAGE_V   260
#define MAX_TEMP_C      85

K_MSGQ_DEFINE(sim_evtq, sizeof(struct sim_evt), 16, 4);
static K_MUTEX_DEFINE(sim_lock_mtx);

static struct sim_conn_state conn[SIM_MAX_CONN + 1]; /* index 1..N */
static bool power_on = true;
static bool link_up = true;
static int64_t last_alive_ms;

static void meter_tick(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(meter_work, meter_tick);

static bool valid_conn(int c)
{
	return c >= 1 && c <= SIM_MAX_CONN;
}

static void post(uint8_t type, int c, const char *text)
{
	struct sim_evt e = { .type = type, .conn = (uint8_t)c };

	if (text) {
		strncpy(e.text, text, sizeof(e.text) - 1);
	}
	if (k_msgq_put(&sim_evtq, &e, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, event %d dropped", type);
	}
}

void sim_init(void)
{
	for (int c = 1; c <= SIM_MAX_CONN; c++) {
		memset(&conn[c], 0, sizeof(conn[c]));
		conn[c].voltage_v = NOMINAL_V;
		conn[c].temp_c = NOMINAL_TEMP_C;
	}
	k_work_schedule(&meter_work, K_SECONDS(1));
}

int sim_evt_get(struct sim_evt *evt, k_timeout_t timeout)
{
	return k_msgq_get(&sim_evtq, evt, timeout);
}

/* ---------------- world -> charger ---------------- */

int sim_plug(int c)
{
	if (!valid_conn(c)) {
		return -EINVAL;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	bool was = conn[c].cable_cs && conn[c].cable_ev;

	conn[c].cable_cs = true;
	conn[c].cable_ev = true;
	k_mutex_unlock(&sim_lock_mtx);
	if (!was) {
		post(SIM_EVT_PLUG, c, NULL);
	}
	return 0;
}

int sim_unplug(int c)
{
	if (!valid_conn(c)) {
		return -EINVAL;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	if (conn[c].locked) {
		/* A locked cable cannot be pulled out of the socket, like a real charger. */
		k_mutex_unlock(&sim_lock_mtx);
		return -EBUSY;
	}
	bool was = conn[c].cable_cs;

	conn[c].cable_cs = false;
	conn[c].cable_ev = false;
	k_mutex_unlock(&sim_lock_mtx);
	if (was) {
		post(SIM_EVT_UNPLUG, c, NULL);
	}
	return 0;
}

int sim_ev_disconnect(int c)
{
	if (!valid_conn(c)) {
		return -EINVAL;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	bool was = conn[c].cable_ev;

	conn[c].cable_ev = false;
	k_mutex_unlock(&sim_lock_mtx);
	if (was) {
		post(SIM_EVT_EV_DISCONNECT, c, NULL);
	}
	return 0;
}

int sim_card(int c, const char *tag)
{
	if (!valid_conn(c) || !tag || !*tag || strlen(tag) > SIM_TAG_MAX) {
		return -EINVAL;
	}
	post(SIM_EVT_CARD, c, tag);
	return 0;
}

int sim_power(bool on)
{
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	bool was = power_on;

	power_on = on;
	if (!on) {
		for (int c = 1; c <= SIM_MAX_CONN; c++) {
			conn[c].relay = false; /* no mains = no energy */
		}
	}
	k_mutex_unlock(&sim_lock_mtx);
	if (was != on) {
		post(on ? SIM_EVT_POWER_ON : SIM_EVT_POWER_OFF, 0, NULL);
	}
	return 0;
}

int sim_lock_fail(int c, bool fail)
{
	if (!valid_conn(c)) {
		return -EINVAL;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	conn[c].lock_fail = fail;
	k_mutex_unlock(&sim_lock_mtx);
	return 0;
}

int sim_fault(int c, const char *kind)
{
	if (!valid_conn(c)) {
		return -EINVAL;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	if (strcmp(kind, "overcurrent") == 0) {
		conn[c].current_a = MAX_CURRENT_A + 10;
	} else if (strcmp(kind, "overtemp") == 0) {
		conn[c].temp_c = MAX_TEMP_C + 10;
	} else if (strcmp(kind, "overvoltage") == 0) {
		conn[c].voltage_v = MAX_VOLTAGE_V + 20;
	} else if (strcmp(kind, "off") == 0) {
		conn[c].voltage_v = NOMINAL_V;
		conn[c].temp_c = NOMINAL_TEMP_C;
		conn[c].current_a = conn[c].relay ? CHARGE_POWER_W / NOMINAL_V : 0;
	} else {
		k_mutex_unlock(&sim_lock_mtx);
		return -EINVAL;
	}
	k_mutex_unlock(&sim_lock_mtx);
	/* The range check in meter_tick() notices it within 1 s. */
	return 0;
}

int sim_link(bool up)
{
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	bool was = link_up;

	link_up = up;
	k_mutex_unlock(&sim_lock_mtx);
	if (was != up) {
		post(up ? SIM_EVT_LINK_UP : SIM_EVT_LINK_DOWN, 0, NULL);
	}
	return 0;
}

/* ---------------- charger -> world ---------------- */

void sim_set_relay(int c, bool on)
{
	if (!valid_conn(c)) {
		return;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	conn[c].relay = on && power_on;
	k_mutex_unlock(&sim_lock_mtx);
}

bool sim_lock(int c)
{
	bool ok;

	if (!valid_conn(c)) {
		return false;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	ok = !conn[c].lock_fail;
	if (ok) {
		conn[c].locked = true;
	}
	k_mutex_unlock(&sim_lock_mtx);
	return ok;
}

bool sim_unlock(int c)
{
	bool ok;

	if (!valid_conn(c)) {
		return false;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	ok = !conn[c].lock_fail;
	if (ok) {
		conn[c].locked = false;
	}
	k_mutex_unlock(&sim_lock_mtx);
	return ok;
}

/* ---------------- sensors ---------------- */

void sim_get(int c, struct sim_conn_state *out)
{
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	*out = conn[valid_conn(c) ? c : 1];
	k_mutex_unlock(&sim_lock_mtx);
}

uint32_t sim_meter_wh(int c)
{
	uint32_t wh;

	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	wh = valid_conn(c) ? (uint32_t)(conn[c].energy_mwh / 1000) : 0;
	k_mutex_unlock(&sim_lock_mtx);
	return wh;
}

bool sim_power_on(void)
{
	return power_on;
}

bool sim_link_up(void)
{
	return link_up;
}

void sim_alive_kick(void)
{
	last_alive_ms = k_uptime_get();
}

bool sim_led_green(void)
{
	return power_on && last_alive_ms != 0 && (k_uptime_get() - last_alive_ms) < 2000;
}

/* Runs every second: energy meter + range check of the sensors. */
static void meter_tick(struct k_work *w)
{
	ARG_UNUSED(w);

	for (int c = 1; c <= SIM_MAX_CONN; c++) {
		char fault[SIM_FAULT_MAX + 1] = "";
		bool changed;

		k_mutex_lock(&sim_lock_mtx, K_FOREVER);
		struct sim_conn_state *s = &conn[c];
		bool flowing = s->relay && s->cable_ev && power_on;

		s->power_w = flowing ? CHARGE_POWER_W : 0;
		if (s->current_a <= MAX_CURRENT_A) { /* keep an injected over-current value */
			s->current_a = s->power_w / NOMINAL_V;
		}
		/* power [W] * 1 s = W*s; 1 Wh = 3600 W*s; we count mWh: W*1000/3600 */
		s->energy_mwh += (uint64_t)s->power_w * 1000 / 3600;

		if (s->current_a > MAX_CURRENT_A) {
			strcpy(fault, "OverCurrentFailure");
		} else if (s->temp_c > MAX_TEMP_C) {
			strcpy(fault, "HighTemperature");
		} else if (s->voltage_v > MAX_VOLTAGE_V) {
			strcpy(fault, "OverVoltage");
		}
		changed = strcmp(fault, s->fault) != 0;
		strcpy(s->fault, fault);
		if (*fault) {
			s->relay = false; /* hardware protection opens the contactor */
		}
		k_mutex_unlock(&sim_lock_mtx);

		if (changed) {
			post(*fault ? SIM_EVT_FAULT : SIM_EVT_FAULT_CLEARED, c, fault);
		}
	}
	k_work_schedule(&meter_work, K_SECONDS(1));
}

size_t sim_status(char *buf, size_t len)
{
	size_t n = 0;

	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	n += snprintf(buf + n, len - n, "power=%s led=%s link=%s",
		      power_on ? "on" : "off", sim_led_green() ? "green" : "off",
		      link_up ? "up" : "down");
	for (int c = 1; c <= SIM_MAX_CONN && n < len; c++) {
		struct sim_conn_state *s = &conn[c];

		n += snprintf(buf + n, len - n,
			      " c%d.cable=%d c%d.ev=%d c%d.locked=%d c%d.relay=%d c%d.wh=%u"
			      " c%d.power_w=%d c%d.fault=%s",
			      c, s->cable_cs, c, s->cable_ev, c, s->locked, c, s->relay,
			      c, (unsigned)(s->energy_mwh / 1000), c, s->power_w,
			      c, *s->fault ? s->fault : "none");
	}
	k_mutex_unlock(&sim_lock_mtx);
	return MIN(n, len);
}
