/*
 * Connector status, derived from the simulated hardware.
 *
 * Step 3 rules (transactions and availability come in the next steps):
 *   fault on the connector  -> Faulted
 *   cable plugged in        -> Preparing
 *   otherwise               -> Available
 * Connector 0 (whole charge point) is Available.
 */
#include <string.h>
#include "ocpp/connector.h"
#include "sim/sim.h"

static const char *const names[ST_COUNT + 1] = {
	"Available", "Preparing", "Charging", "SuspendedEV", "SuspendedEVSE",
	"Finishing", "Reserved", "Unavailable", "Faulted", "Unknown",
};

void conn_init(void)
{
}

const char *conn_status_str(enum cp_status s)
{
	return names[s <= ST_COUNT ? s : ST_COUNT];
}

enum cp_status conn_status(int c)
{
	struct sim_conn_state hw;

	if (c == 0) {
		return ST_AVAILABLE;
	}
	sim_get(c, &hw);
	if (hw.fault[0]) {
		return ST_FAULTED;
	}
	if (hw.cable_cs) {
		return ST_PREPARING;
	}
	return ST_AVAILABLE;
}

const char *conn_error_code(int c)
{
	static char code[SIM_FAULT_MAX + 1];
	struct sim_conn_state hw;

	if (c == 0) {
		return "NoError";
	}
	sim_get(c, &hw);
	if (!hw.fault[0]) {
		return "NoError";
	}
	strcpy(code, hw.fault);
	return code;
}
