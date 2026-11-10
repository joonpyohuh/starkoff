/*
 * STARKOFF OCPP 1.6J Charge Point - entry point.
 * Networking is brought up by Zephyr's net_config (static 10.0.2.15) before main() runs.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include "store/store.h"
#include "ocpp/config.h"
#include "sim/sim.h"
#include "ctrl/ctrl.h"
#include "net/link.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	struct sim_evt e;

	LOG_INF("STARKOFF charge point firmware starting (Zephyr %s)", KERNEL_VERSION_STRING);
	store_init();
	cfg_init();
	LOG_INF("server %s, CP id %s", cfg_get(CFG_SERVER_URL), cfg_get(CFG_CP_ID));
	sim_init();
	ctrl_start();
	link_announce_start();

	/* Step 2 placeholder: the OCPP charger logic will consume these events. */
	while (1) {
		sim_alive_kick();
		if (sim_evt_get(&e, K_MSEC(200)) == 0) {
			LOG_INF("hw event type=%d conn=%d text=%s", e.type, e.conn, e.text);
		}
	}
	return 0;
}
