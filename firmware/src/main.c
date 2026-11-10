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
#include "ocpp/ocpp.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("STARKOFF charge point firmware starting (Zephyr %s)", KERNEL_VERSION_STRING);
	store_init();
	cfg_init();
	LOG_INF("server %s, CP id %s", cfg_get(CFG_SERVER_URL), cfg_get(CFG_CP_ID));
	sim_init();
	ctrl_start();
	link_announce_start();

	ocpp_run(); /* never returns */
	return 0;
}
