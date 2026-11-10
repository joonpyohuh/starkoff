/* OCPP 1.6J charge point engine. See ocpp.c. */
#pragma once
#include <stdbool.h>

enum boot_state {
	BOOT_NOT_SENT,
	BOOT_SENT,      /* waiting for BootNotification.conf */
	BOOT_ACCEPTED,
	BOOT_PENDING,
	BOOT_REJECTED,
};

/* Never returns. */
void ocpp_run(void);
enum boot_state ocpp_boot_state(void);
/* Queue a CALL. tx_related = transaction message (kept across reconnects). */
int ocpp_enqueue(const char *action, const char *payload, bool tx_related);
