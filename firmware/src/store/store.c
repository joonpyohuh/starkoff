/*
 * Persistent record storage on memory-mapped non-volatile memory (NVM).
 *
 * On QEMU the NVM is an ivshmem device backed by a host file (see
 * boards/qemu_x86.overlay). On a real charger this would be NOR flash or FRAM.
 *
 * A "record" is a small blob (max STORE_REC_MAX bytes) kept in two slots, A and B.
 * Every save goes to the slot that is NOT the newest one, with a higher sequence
 * number and a CRC. The header (with the CRC) is written LAST, so a half-written
 * slot never has a valid CRC. On load we take the newest slot whose CRC is valid.
 * So if power is cut in the middle of a write, the older slot is still intact.
 *
 * Layout (4 KiB slots):
 *   record 0 (config):       slot A @ 0x0000, slot B @ 0x1000
 *   record 1 (transaction):  slot A @ 0x2000, slot B @ 0x3000
 *   record 2 (tx msg queue): slot A @ 0x4000, slot B @ 0x5000   (later phases)
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/virtualization/ivshmem.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include "store/store.h"

LOG_MODULE_REGISTER(store, LOG_LEVEL_INF);

#define REC_MAGIC 0x534b4346u /* "SKCF" */
#define MAX_RECS  8

struct rec_hdr {
	uint32_t magic;
	uint32_t seq;
	uint32_t len;
	uint32_t crc;
};

static const struct device *nvm_dev = DEVICE_DT_GET(DT_NODELABEL(nvmem0));
static uint8_t *nvm;    /* base of the mapped memory, NULL if not available */
static size_t nvm_size;
static K_MUTEX_DEFINE(store_lock);

/*
 * The transaction message queue checkpoint (D28) is bigger than one 4 KiB slot, so it has
 * its own region right after the normal records: two 32 KiB slots at 0x10000 and 0x18000.
 */
#define QUEUE_BASE       ((size_t)MAX_RECS * 2 * STORE_SLOT_BYTES) /* = 0x10000 */
#define QUEUE_SLOT_BYTES (32 * 1024)
#define NVM_NEEDED       (QUEUE_BASE + 2 * QUEUE_SLOT_BYTES)

static size_t rec_max(int rec_id)
{
	return rec_id == STORE_REC_QUEUE ? QUEUE_SLOT_BYTES - sizeof(struct rec_hdr) : STORE_REC_MAX;
}

static uint8_t *slot_ptr(int rec_id, int slot)
{
	if (rec_id == STORE_REC_QUEUE) {
		return nvm + QUEUE_BASE + (size_t)slot * QUEUE_SLOT_BYTES;
	}
	return nvm + (size_t)(rec_id * 2 + slot) * STORE_SLOT_BYTES;
}

int store_init(void)
{
	uintptr_t base;

	if (!device_is_ready(nvm_dev)) {
		LOG_ERR("NV memory device not ready: settings will NOT persist");
		return -ENODEV;
	}
	nvm_size = ivshmem_get_mem(nvm_dev, &base);
	if (nvm_size < NVM_NEEDED) {
		LOG_ERR("NV memory too small (%u bytes): settings will NOT persist",
			(unsigned)nvm_size);
		return -ENOSPC;
	}
	nvm = (uint8_t *)base;
	LOG_INF("NV memory ready: %u KiB", (unsigned)(nvm_size / 1024));
	return 0;
}

bool store_available(void)
{
	return nvm != NULL;
}

/* Returns seq if the slot holds a valid record, or -1. */
static int64_t check_slot(int rec_id, int slot)
{
	struct rec_hdr h;
	const uint8_t *p = slot_ptr(rec_id, slot);

	memcpy(&h, p, sizeof(h));
	if (h.magic != REC_MAGIC || h.len > rec_max(rec_id)) {
		return -1;
	}
	if (crc32_ieee(p + sizeof(h), h.len) != h.crc) {
		return -1;
	}
	return h.seq;
}

/* Finds the newest valid slot. Returns slot index or -1, and its seq. */
static int newest_slot(int rec_id, uint32_t *seq_out)
{
	int64_t a = check_slot(rec_id, 0);
	int64_t b = check_slot(rec_id, 1);

	if (a < 0 && b < 0) {
		return -1;
	}
	if (b > a) {
		*seq_out = (uint32_t)b;
		return 1;
	}
	*seq_out = (uint32_t)a;
	return 0;
}

int store_load(int rec_id, void *out, size_t max_len)
{
	uint32_t seq;
	struct rec_hdr h;
	int slot, len;

	if (!nvm || rec_id < 0 || rec_id >= MAX_RECS) {
		return -ENODEV;
	}
	k_mutex_lock(&store_lock, K_FOREVER);
	slot = newest_slot(rec_id, &seq);
	if (slot < 0) {
		k_mutex_unlock(&store_lock);
		return -ENOENT;
	}
	memcpy(&h, slot_ptr(rec_id, slot), sizeof(h));
	len = MIN(h.len, max_len);
	memcpy(out, slot_ptr(rec_id, slot) + sizeof(h), len);
	k_mutex_unlock(&store_lock);
	return len;
}

int store_save(int rec_id, const void *data, size_t len)
{
	uint32_t seq = 0;
	struct rec_hdr h;
	uint8_t *p;
	int slot;

	if (!nvm || rec_id < 0 || rec_id >= MAX_RECS) {
		return -ENODEV;
	}
	if (len > rec_max(rec_id)) {
		return -EINVAL;
	}
	k_mutex_lock(&store_lock, K_FOREVER);
	slot = newest_slot(rec_id, &seq);
	slot = (slot == 0) ? 1 : 0; /* write the other slot */
	p = slot_ptr(rec_id, slot);

	/* 1. invalidate the target slot, 2. write data, 3. write header last */
	memset(p, 0, sizeof(h));
	compiler_barrier();
	memcpy(p + sizeof(h), data, len);
	compiler_barrier();
	h.magic = REC_MAGIC;
	h.seq = seq + 1;
	h.len = len;
	h.crc = crc32_ieee(data, len);
	memcpy(p, &h, sizeof(h));
	k_mutex_unlock(&store_lock);
	return 0;
}

int store_erase(int rec_id)
{
	if (!nvm || rec_id < 0 || rec_id >= MAX_RECS) {
		return -ENODEV;
	}
	k_mutex_lock(&store_lock, K_FOREVER);
	memset(slot_ptr(rec_id, 0), 0, sizeof(struct rec_hdr));
	memset(slot_ptr(rec_id, 1), 0, sizeof(struct rec_hdr));
	k_mutex_unlock(&store_lock);
	return 0;
}
