/*
 * Connector = one charging socket and its charging session (transaction).
 * Connector 0 = the charge point as a whole (status only).
 */
#pragma once
#include "sim/sim.h"
#include <stdbool.h>
#include <stddef.h>

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
/* Called every loop: ConnectionTimeOut, MeterValues sampling. */
void conn_tick(void);
/* Hardware events from the simulator (plug, unplug, card, EV disconnect, fault). */
void conn_on_hw(const struct sim_evt *e);

/* RemoteStartTransaction / RemoteStopTransaction. Return true = Accepted. */
bool conn_remote_start(int c, const char *id_tag);
bool conn_remote_stop(int transaction_id);

enum avail_result {
	AVAIL_ACCEPTED,
	AVAIL_REJECTED,
	AVAIL_SCHEDULED
};
/* ChangeAvailability. c = 0 means the whole charge point. */
enum avail_result conn_change_availability(int c, bool operative);
/* UnlockConnector: "Unlocked", "UnlockFailed" or "NotSupported". */
const char *conn_unlock(int c);
/* Stop every running transaction (Reset). Returns how many were stopped. */
int conn_stop_all(const char *reason);
bool conn_valid(int c);

/* Answers from the Central System (routed by ocpp.c). */
void conn_on_authorize_conf(int c, const char *id_tag, const char *status, const char *parent);
void conn_on_start_conf(int tx_local, int transaction_id, const char *status, const char *parent);
/* A CALL we sent for this connector failed (CALLERROR or not deliverable). */
void conn_on_call_failed(const char *action, int c, int tx_local);
/* Real transactionId for a local transaction number, -1 if not known (yet). */
int conn_tx_real_id(int tx_local);

enum cp_status conn_status(int c);  /* c = 0..CONN_MAX */
const char *conn_error_code(int c); /* OCPP ChargePointErrorCode, "NoError" if none */
const char *conn_status_str(enum cp_status s);
/* Appends " cN.tx=... cN.tag=..." for the control port status line. */
size_t conn_status_append(int c, char *buf, size_t len);

void conn_clock_changed(void);
void conn_trigger_meter(int c);
const char *conn_reserve(int c, int id, const char *tag, const char *parent, int64_t expiry);
bool conn_cancel_reservation(int id);

size_t conn_checkpoint_save(void *out, size_t capacity);
void conn_checkpoint_load(const void *data, size_t size);

void conn_meter_config_changed(void);
