/*
 * Simulated hardware ("the outside world" of the charger).
 *
 * Think of this as the drivers of a real charger: cable detect pin, RFID reader,
 * contactor (relay), connector lock motor, energy meter, mains power, Ethernet link,
 * and sensors (voltage/current/temperature).
 * The control interface (UART shell / TCP port) changes this state; the charger
 * logic (ocpp/) reads it and receives events from sim_evt_get().
 *
 * The sim keeps running even while "power is off", because it plays the role of
 * the real world: someone has to be able to switch the power back on.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

#define SIM_MAX_CONN   CONFIG_OCPP_NUM_CONNECTORS
#define SIM_TAG_MAX    20  /* OCPP IdToken is CiString20 */
#define SIM_FAULT_MAX  32

enum sim_evt_type {
	SIM_EVT_PLUG,          /* cable connected on both charger and EV side */
	SIM_EVT_UNPLUG,        /* cable removed from charger socket */
	SIM_EVT_EV_DISCONNECT, /* cable removed on EV side only */
	SIM_EVT_CARD,          /* RFID card presented */
	SIM_EVT_POWER_OFF,
	SIM_EVT_POWER_ON,
	SIM_EVT_FAULT,         /* sensor out of range: fault text = OCPP ChargePointErrorCode */
	SIM_EVT_FAULT_CLEARED,
	SIM_EVT_LINK_DOWN,     /* network cable pulled (net_down) */
	SIM_EVT_LINK_UP,
};

struct sim_evt {
	uint8_t type;
	uint8_t conn;                    /* 1..N, 0 = whole charge point */
	char text[SIM_FAULT_MAX + 1];    /* idTag for CARD, errorCode for FAULT */
};

/* Snapshot of one connector, for status output and charger logic. */
struct sim_conn_state {
	bool cable_cs;      /* plugged into the charger socket */
	bool cable_ev;      /* plugged into the car */
	bool locked;
	bool lock_fail;     /* injected: lock motor fails */
	bool relay;         /* contactor closed = energy flows */
	uint64_t energy_mwh;/* meter register (milli-Wh) */
	int power_w;        /* present power (0 if relay open) */
	int voltage_v;
	int current_a;
	int temp_c;
	char fault[SIM_FAULT_MAX + 1]; /* "" = no fault */
};

void sim_init(void);
int sim_evt_get(struct sim_evt *evt, k_timeout_t timeout);

/* --- commands from the control interface (world -> charger) --- */
int sim_plug(int c);
int sim_unplug(int c);
int sim_ev_disconnect(int c);
int sim_card(int c, const char *tag);
int sim_power(bool on);
int sim_lock_fail(int c, bool fail);
int sim_fault(int c, const char *kind); /* "overcurrent", "overtemp", "overvoltage", "off" */
int sim_link(bool up);

/* --- actuators used by the charger logic (charger -> world) --- */
void sim_set_relay(int c, bool on);
bool sim_lock(int c);    /* returns false if the lock motor fails */
bool sim_unlock(int c);  /* returns false if the lock motor fails */

/* --- sensors read by the charger logic --- */
void sim_get(int c, struct sim_conn_state *out);
uint32_t sim_meter_wh(int c);
bool sim_power_on(void);
bool sim_link_up(void);

/* Alive signal: charger main loop calls this. Status LED is green only if the
 * last call was less than 2 s ago ("green only while healthy"). */
void sim_alive_kick(void);
bool sim_led_green(void);

/* Append "key=value" status of the hardware to buf. */
size_t sim_status(char *buf, size_t len);

/* Restore the outside world after a simulated controller reboot. */
void sim_restore(int c, const struct sim_conn_state *state);
