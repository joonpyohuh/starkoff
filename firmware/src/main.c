/*
 * STARKOFF OCPP 1.6J Charge Point - entry point.
 * Networking is brought up by Zephyr's net_config (static 10.0.2.15) before main() runs.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("STARKOFF charge point firmware starting (Zephyr %s)", KERNEL_VERSION_STRING);
	LOG_INF("Default server %s, CP id %s", CONFIG_OCPP_SERVER_URL, CONFIG_OCPP_CHARGE_POINT_ID);
	return 0;
}
