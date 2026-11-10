/*
 * Connector state as reported to the Central System (OCPP 1.6 section 7.7 ChargePointStatus).
 * Connector 0 = the charge point as a whole.
 */
#pragma once
#include <stdbool.h>

#define CONN_MAX CONFIG_OCPP_NUM_CONNECTORS

enum cp_status {
	ST_AVAILABLE,
	ST_PREPARING,
	ST_CHARGING,
	ST_SUSPENDED_EV,
	ST_SUSPENDED_EVSE,
	ST_FINISHING,
	ST_RESERVED,
	ST_UNAVAILABLE,
	ST_FAULTED,
	ST_COUNT,
	ST_UNKNOWN = ST_COUNT, /* "not reported yet" */
};

void conn_init(void);
enum cp_status conn_status(int c);           /* c = 0..CONN_MAX */
const char *conn_error_code(int c);          /* OCPP ChargePointErrorCode, "NoError" if none */
const char *conn_status_str(enum cp_status s);
