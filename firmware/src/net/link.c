/*
 * Link announce: make QEMU's user-mode network (SLIRP) learn our MAC address.
 *
 * Why: Zephyr v4.2.0's e1000 driver enables receive without "accept broadcast",
 * so broadcast frames (ARP requests from SLIRP, DHCP offers) never reach us.
 * SLIRP must know our MAC to forward the control port (host:7000 -> 10.0.2.15:7000).
 * When WE send any packet to the gateway, our own ARP request teaches SLIRP our
 * MAC, and its ARP reply is unicast, so it arrives.
 * We send one tiny UDP datagram to the gateway's discard port at boot and every
 * 30 s (cheap, and keeps working if SLIRP ever forgets).
 */
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include "net/link.h"

LOG_MODULE_REGISTER(link, LOG_LEVEL_INF);

#define ANNOUNCE_PERIOD K_SECONDS(30)

static void announce(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(announce_work, announce);

static void announce(struct k_work *w)
{
	struct sockaddr_in gw = {
		.sin_family = AF_INET,
		.sin_port = htons(9), /* discard */
	};
	int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	ARG_UNUSED(w);
	zsock_inet_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_GW, &gw.sin_addr);
	if (fd >= 0) {
		zsock_sendto(fd, "cp", 2, 0, (struct sockaddr *)&gw, sizeof(gw));
		zsock_close(fd);
	}
	k_work_schedule(&announce_work, ANNOUNCE_PERIOD);
}

void link_announce_start(void)
{
	k_work_schedule(&announce_work, K_NO_WAIT);
}
