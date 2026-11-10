/*
 * Configuration keys (OCPP 1.6 chapter 9 + a few internal ones).
 * All values are kept as text, exactly like OCPP sends them, and saved to disk.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define CFG_VAL_MAX 256

enum cfg_type { CFG_BOOL, CFG_INT, CFG_STR, CFG_CSL };

enum cfg_flags {
	CFG_RO       = 1 << 0, /* readonly="true" in GetConfiguration */
	CFG_HIDDEN   = 1 << 1, /* internal: not visible over OCPP (server URL, CP id) */
	CFG_REBOOT   = 1 << 2, /* ChangeConfiguration answers RebootRequired */
};

enum cfg_set_result {
	CFG_ACCEPTED,
	CFG_REJECTED,        /* readonly or bad value */
	CFG_REBOOT_REQUIRED,
	CFG_NOT_SUPPORTED,   /* unknown key */
};

struct cfg_item {
	const char *key;
	enum cfg_type type;
	unsigned flags;
	const char *def;     /* default value */
	char val[CFG_VAL_MAX];
};

/* Internal keys (CFG_HIDDEN) */
#define CFG_SERVER_URL  "_ServerUrl"
#define CFG_CP_ID       "_ChargePointId"
#define CFG_BUILD_TAG   "_BuildDefaults"

void cfg_init(void);
const char *cfg_get(const char *key);       /* NULL if unknown */
int cfg_get_int(const char *key, int fallback);
bool cfg_get_bool(const char *key);
/* from_ocpp=true applies OCPP rules (readonly/hidden keys are refused). */
enum cfg_set_result cfg_set(const char *key, const char *value, bool from_ocpp);
void cfg_factory_reset(void);

/* Iterate all items (for GetConfiguration and the shell). */
size_t cfg_count(void);
const struct cfg_item *cfg_at(size_t i);
const struct cfg_item *cfg_find(const char *key);
