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
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include "sim/sim.h"

LOG_MODULE_REGISTER(sim, LOG_LEVEL_INF);

#define CHARGE_POWER_W 7400 /* 32 A single phase */
#define NOMINAL_V      230
#define NOMINAL_TEMP_C 35

/* Safe operating range. Outside it = fault (range check, docs/DESIGN_resilience.md 3-3). */
#define MAX_CURRENT_A 40
#define MAX_VOLTAGE_V 260
#define MAX_TEMP_C    85

K_MSGQ_DEFINE(sim_evtq, sizeof(struct sim_evt), 16, 4);
static K_MUTEX_DEFINE(sim_lock_mtx);

static struct sim_conn_state conn[SIM_MAX_CONN + 1]; /* index 1..N */
static bool power_on = true;
static bool link_up = true;
static int64_t last_alive_ms;

static void meter_tick(struct k_work *w);
static void debounce_tick(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_tick);

/*
 * Cable detect with debouncing (bonus from docs/DESIGN_resilience.md).
 * plug/unplug only change the raw signal. Every 10 ms the signal is sampled into an
 * up/down counter (0..DEB_MAX). The debounced state turns ON at >= DEB_ON and OFF at
 * <= DEB_OFF (hysteresis). With "noise <c> on" each sample reads the wrong value with
 * probability 1/4; the gap 16/12/4 makes a false change practically impossible
 * (12 wrong samples in a row: (1/4)^12 per sample, about once in 5 days at 100 Hz).
 * Cost: a plug is recognised ~120 ms after the raw change.
 */
#define DEB_PERIOD_MS 10
#define DEB_MAX       16
#define DEB_ON        12
#define DEB_OFF       4

static bool raw_cable[SIM_MAX_CONN + 1];
static bool noise_on[SIM_MAX_CONN + 1];
static int deb_count[SIM_MAX_CONN + 1];
static uint32_t glitches[SIM_MAX_CONN + 1];
static K_WORK_DELAYABLE_DEFINE(meter_work, meter_tick);

static bool valid_conn(int c)
{
	return c >= 1 && c <= SIM_MAX_CONN;
}

static void post(uint8_t type, int c, const char *text)
{
	struct sim_evt e = {.type = type, .conn = (uint8_t)c};

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
	k_work_schedule(&debounce_work, K_MSEC(DEB_PERIOD_MS));
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
	/* Cable already detected in the socket, only the car side was out: re-plug at once. */
	bool replug_ev = conn[c].cable_cs && !conn[c].cable_ev;

	raw_cable[c] = true;
	if (replug_ev) {
		conn[c].cable_ev = true;
	}
	k_mutex_unlock(&sim_lock_mtx);
	if (replug_ev) {
		post(SIM_EVT_PLUG, c, NULL);
	}
	return 0; /* otherwise the debouncer reports the plug ~120 ms later */
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
	raw_cable[c] = false; /* the debouncer reports the unplug ~120 ms later */
	k_mutex_unlock(&sim_lock_mtx);
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
	n += snprintf(buf + n, len - n, "power=%s led=%s link=%s", power_on ? "on" : "off",
		      sim_led_green() ? "green" : "off", link_up ? "up" : "down");
	for (int c = 1; c <= SIM_MAX_CONN && n < len; c++) {
		struct sim_conn_state *s = &conn[c];

		n += snprintf(buf + n, len - n,
			      " c%d.cable=%d c%d.ev=%d c%d.locked=%d c%d.relay=%d c%d.wh=%u"
			      " c%d.power_w=%d c%d.noise=%s c%d.glitches=%u c%d.fault=%s",
			      c, s->cable_cs, c, s->cable_ev, c, s->locked, c, s->relay, c,
			      (unsigned)(s->energy_mwh / 1000), c, s->power_w, c,
			      noise_on[c] ? "on" : "off", c, (unsigned)glitches[c], c,
			      *s->fault ? s->fault : "none");
	}
	k_mutex_unlock(&sim_lock_mtx);
	return MIN(n, len);
}

void sim_restore(int c, const struct sim_conn_state *state)
{
	if (!valid_conn(c)) {
		return;
	}
	k_mutex_lock(&sim_lock_mtx, K_FOREVER);
	conn[c] = *state;
	conn[c].relay = false;
	conn[c].power_w = 0;
	raw_cable[c] = state->cable_cs; /* debouncer starts settled */
	deb_count[c] = state->cable_cs ? DEB_MAX : 0;
	k_mutex_unlock(&sim_lock_mtx);
}

static volatile bool hung;

void sim_hang(void)
{
	hung = true;
}

bool sim_hung(void)
{
	return hung;
}

int sim_noise(int c, bool on)
{
	if (!valid_conn(c)) {
		return -EINVAL;
	}
	noise_on[c] = on;
	return 0;
}

static void debounce_tick(struct k_work *w)
{
	ARG_UNUSED(w);

	for (int c = 1; c <= SIM_MAX_CONN; c++) {
		uint8_t evt = 0xff;

		k_mutex_lock(&sim_lock_mtx, K_FOREVER);
		bool sample = raw_cable[c];

		if (noise_on[c] && (sys_rand32_get() & 3) == 0) {
			sample = !sample; /* electrical noise: one wrong reading */
			glitches[c]++;
		}
		deb_count[c] += sample ? 1 : -1;
		deb_count[c] = CLAMP(deb_count[c], 0, DEB_MAX);
		if (!conn[c].cable_cs && deb_count[c] >= DEB_ON) {
			conn[c].cable_cs = true;
			conn[c].cable_ev = true;
			evt = SIM_EVT_PLUG;
		} else if (conn[c].cable_cs && deb_count[c] <= DEB_OFF) {
			conn[c].cable_cs = false;
			conn[c].cable_ev = false;
			evt = SIM_EVT_UNPLUG;
		}
		k_mutex_unlock(&sim_lock_mtx);
		if (evt != 0xff) {
			post(evt, c, NULL);
		}
	}
	k_work_schedule(&debounce_work, K_MSEC(DEB_PERIOD_MS));
}
