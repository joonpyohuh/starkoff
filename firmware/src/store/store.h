/* Persistent record storage (memory-mapped NV memory, A/B slots with CRC). See store.c. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define STORE_SLOT_BYTES 4096
#define STORE_REC_MAX    (STORE_SLOT_BYTES - 16)

enum store_rec {
	STORE_REC_CONFIG = 0,
	STORE_REC_TX = 1,    /* ongoing transaction record */
	STORE_REC_LOCAL = 3,
	STORE_REC_CACHE = 4,
	STORE_REC_RESERVATION = 5,
	STORE_REC_QUEUE = 2, /* transaction message queue */
};

int store_init(void);
bool store_available(void);
/* Returns number of bytes read, or negative errno (-ENOENT if nothing stored yet). */
int store_load(int rec_id, void *out, size_t max_len);
int store_save(int rec_id, const void *data, size_t len);
int store_erase(int rec_id);
