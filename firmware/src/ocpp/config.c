/*
 * Configuration key table.
 *
 * - One table holds every key: OCPP keys (GetConfiguration/ChangeConfiguration)
 *   and internal keys (server URL, CP id) that only the shell/control port may change.
 * - Values are text. Validation happens in cfg_set() based on the key type.
 * - Every change is written to disk (store.c) as "key=value\n" lines, only for
 *   keys that differ from their default.
 * - If the firmware was rebuilt with a different default URL/CP id
 *   (scripts/build.sh --url/--id), the stored URL/id are dropped: the new build wins.
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "ocpp/config.h"
#include "store/store.h"

LOG_MODULE_REGISTER(cfg, LOG_LEVEL_INF);

#define STR(x)  #x
#define XSTR(x) STR(x)

/* Measurands this charger can measure (sim/sim.c provides all of them). */
static const char *const measurands[] = {
	"Energy.Active.Import.Register",
	"Power.Active.Import",
	"Current.Import",
	"Voltage",
};

/* clang-format off */
static struct cfg_item items[] = {
	/* key                                     type      flags        default */
	{ "AllowOfflineTxForUnknownId",           CFG_BOOL, 0,           "false" },
	{ "AuthorizationCacheEnabled",            CFG_BOOL, 0,           "true" },
	{ "AuthorizeRemoteTxRequests",            CFG_BOOL, 0,           "true" },
	{ "BackupPower",                          CFG_BOOL, 0,           "false" }, /* vendor key, see docs/DESIGN_resilience.md */
	{ "ClockAlignedDataInterval",             CFG_INT,  0,           "0" },
	{ "ConnectionTimeOut",                    CFG_INT,  0,           "30" },
	{ "ConnectorPhaseRotation",               CFG_CSL,  0,           "0.NotApplicable" },
	{ "GetConfigurationMaxKeys",              CFG_INT,  CFG_RO,      "50" },
	{ "HeartbeatInterval",                    CFG_INT,  0,           "300" },
	{ "LocalAuthorizeOffline",                CFG_BOOL, 0,           "true" },
	{ "LocalPreAuthorize",                    CFG_BOOL, 0,           "false" },
	{ "MeterValuesAlignedData",               CFG_CSL,  0,           "Energy.Active.Import.Register" },
	{ "MeterValuesSampledData",               CFG_CSL,  0,           "Energy.Active.Import.Register" },
	{ "MeterValueSampleInterval",             CFG_INT,  0,           "60" },
	{ "MinimumStatusDuration",                CFG_INT,  0,           "0" },
	{ "NumberOfConnectors",                   CFG_INT,  CFG_RO,      XSTR(CONFIG_OCPP_NUM_CONNECTORS) },
	{ "ResetRetries",                         CFG_INT,  0,           "1" },
	{ "StopTransactionOnEVSideDisconnect",    CFG_BOOL, 0,           "true" },
	{ "StopTransactionOnInvalidId",           CFG_BOOL, 0,           "true" },
	{ "StopTxnAlignedData",                   CFG_CSL,  0,           "" },
	{ "StopTxnSampledData",                   CFG_CSL,  0,           "" },
	{ "SupportedFeatureProfiles",             CFG_CSL,  CFG_RO,      "Core,LocalAuthListManagement,Reservation,RemoteTrigger" },
	{ "TransactionMessageAttempts",           CFG_INT,  0,           "3" },
	{ "TransactionMessageRetryInterval",      CFG_INT,  0,           "60" },
	{ "UnlockConnectorOnEVSideDisconnect",    CFG_BOOL, 0,           "true" },
	{ "LocalAuthListEnabled",                 CFG_BOOL, 0,           "true" },
	{ "LocalAuthListMaxLength",               CFG_INT,  CFG_RO,      "32" },
	{ "SendLocalListMaxLength",               CFG_INT,  CFG_RO,      "16" },
	{ "ReserveConnectorZeroSupported",        CFG_BOOL, CFG_RO,      "true" },
	{ "MeterValuesAlignedDataMaxLength",      CFG_INT,  CFG_RO,      "4" },
	{ "MeterValuesSampledDataMaxLength",      CFG_INT,  CFG_RO,      "4" },
	{ "StopTxnAlignedDataMaxLength",          CFG_INT,  CFG_RO,      "4" },
	{ "StopTxnSampledDataMaxLength",          CFG_INT,  CFG_RO,      "4" },
	{ "SupportedFeatureProfilesMaxLength",    CFG_INT,  CFG_RO,      "4" },
	/* internal */
	{ "_StoreFail",                           CFG_BOOL, CFG_HIDDEN,  "false" }, /* test hook: make SendLocalList fail (TC_043_3) */
	{ CFG_SERVER_URL,                         CFG_STR,  CFG_HIDDEN,  CONFIG_OCPP_SERVER_URL },
	{ CFG_CP_ID,                              CFG_STR,  CFG_HIDDEN,  CONFIG_OCPP_CHARGE_POINT_ID },
	{ CFG_BUILD_TAG,                          CFG_STR,  CFG_HIDDEN,  "" },
	/* connectors set Inoperative by ChangeAvailability, e.g. "0,1"; persists across reboot (TC_013) */
	{ CFG_INOPERATIVE,                        CFG_STR,  CFG_HIDDEN,  "" },
};
/* clang-format on */

static K_MUTEX_DEFINE(cfg_lock);
static char file_buf[STORE_REC_MAX];

size_t cfg_count(void)
{
	return ARRAY_SIZE(items);
}

const struct cfg_item *cfg_at(size_t i)
{
	return i < ARRAY_SIZE(items) ? &items[i] : NULL;
}

static struct cfg_item *find(const char *key)
{
	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		/* OCPP 1.6: key names are case-insensitive */
		if (strcasecmp(items[i].key, key) == 0) {
			return &items[i];
		}
	}
	return NULL;
}

const struct cfg_item *cfg_find(const char *key)
{
	return find(key);
}

const char *cfg_get(const char *key)
{
	struct cfg_item *it = find(key);

	return it ? it->val : NULL;
}

int cfg_get_int(const char *key, int fallback)
{
	const char *v = cfg_get(key);

	return (v && *v) ? atoi(v) : fallback;
}

bool cfg_get_bool(const char *key)
{
	const char *v = cfg_get(key);

	return v && strcmp(v, "true") == 0;
}

static bool valid_int(const char *v)
{
	if (*v == '\0' || strlen(v) > 9) {
		return false;
	}
	for (; *v; v++) {
		if (!isdigit((unsigned char)*v)) {
			return false;
		}
	}
	return true;
}

static bool is_measurand_key(const char *key)
{
	return strstr(key, "AlignedData") || strstr(key, "SampledData");
}

/* Every item of a comma-separated list must be a measurand we can measure. */
static bool valid_measurands(const char *v)
{
	char tmp[CFG_VAL_MAX];
	char *save, *tok;

	strncpy(tmp, v, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	for (tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ') {
			tok++;
		}
		bool known = false;

		for (size_t i = 0; i < ARRAY_SIZE(measurands); i++) {
			if (strcmp(tok, measurands[i]) == 0) {
				known = true;
			}
		}
		if (!known) {
			return false;
		}
	}
	return true;
}

/* Write all non-default values to disk. Caller holds cfg_lock. */
static void save_locked(void)
{
	size_t used = 0;

	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		if (strcmp(items[i].val, items[i].def) == 0) {
			continue;
		}
		int n = snprintf(file_buf + used, sizeof(file_buf) - used, "%s=%s\n", items[i].key,
				 items[i].val);
		if (n < 0 || used + n >= sizeof(file_buf)) {
			LOG_ERR("config does not fit in one record");
			return;
		}
		used += n;
	}
	if (store_available()) {
		store_save(STORE_REC_CONFIG, file_buf, used);
	}
}

enum cfg_set_result cfg_set(const char *key, const char *value, bool from_ocpp)
{
	struct cfg_item *it = find(key);
	char norm[CFG_VAL_MAX];
	enum cfg_set_result res = CFG_ACCEPTED;

	if (!it || (from_ocpp && (it->flags & CFG_HIDDEN))) {
		return CFG_NOT_SUPPORTED;
	}
	if (it->flags & CFG_RO) { /* readonly for everyone, value always comes from the build */
		return CFG_REJECTED;
	}
	if (strlen(value) >= CFG_VAL_MAX) {
		return CFG_REJECTED;
	}
	strcpy(norm, value);

	switch (it->type) {
	case CFG_BOOL:
		if (strcasecmp(norm, "true") == 0) {
			strcpy(norm, "true");
		} else if (strcasecmp(norm, "false") == 0) {
			strcpy(norm, "false");
		} else {
			return CFG_REJECTED;
		}
		break;
	case CFG_INT:
		if (!valid_int(norm) || strtoul(norm, NULL, 10) > 86400) {
			return CFG_REJECTED;
		}
		break;
	case CFG_CSL:
		if (is_measurand_key(it->key) && *norm && !valid_measurands(norm)) {
			return CFG_REJECTED;
		}
		break;
	case CFG_STR:
		break;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	strcpy(it->val, norm);
	save_locked();
	k_mutex_unlock(&cfg_lock);

	if (it->flags & CFG_REBOOT) {
		res = CFG_REBOOT_REQUIRED;
	}
	LOG_INF("config %s = %s", it->key, it->val);
	return res;
}

static void load_defaults(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		strcpy(items[i].val, items[i].def);
	}
}

/* "url|id" of this build. If it changed since the values were saved, drop stored url/id. */
static void build_tag(char *out, size_t len)
{
	snprintf(out, len, "%s|%s", CONFIG_OCPP_SERVER_URL, CONFIG_OCPP_CHARGE_POINT_ID);
}

void cfg_init(void)
{
	char tag[CFG_VAL_MAX];
	int len;

	load_defaults();
	build_tag(tag, sizeof(tag));

	len = store_available() ? store_load(STORE_REC_CONFIG, file_buf, sizeof(file_buf) - 1) : -1;
	if (len >= 0) {
		file_buf[len] = '\0';
		char *save, *line;

		for (line = strtok_r(file_buf, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char *eq = strchr(line, '=');
			struct cfg_item *it;

			if (!eq) {
				continue;
			}
			*eq = '\0';
			it = find(line);
			if (it && strlen(eq + 1) < CFG_VAL_MAX) {
				strcpy(it->val, eq + 1);
			}
		}
		LOG_INF("config loaded from disk (%d bytes)", len);
	} else {
		LOG_INF("no stored config, using defaults");
	}

	if (strcmp(cfg_get(CFG_BUILD_TAG), tag) != 0) {
		/* New build defaults: they win over values stored by an older build. */
		strcpy(find(CFG_SERVER_URL)->val, CONFIG_OCPP_SERVER_URL);
		strcpy(find(CFG_CP_ID)->val, CONFIG_OCPP_CHARGE_POINT_ID);
		k_mutex_lock(&cfg_lock, K_FOREVER);
		strcpy(find(CFG_BUILD_TAG)->val, tag);
		save_locked();
		k_mutex_unlock(&cfg_lock);
	}
	/* NumberOfConnectors etc. are readonly and always come from the build. */
	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		if (items[i].flags & CFG_RO) {
			strcpy(items[i].val, items[i].def);
		}
	}
}

void cfg_factory_reset(void)
{
	char tag[CFG_VAL_MAX];

	k_mutex_lock(&cfg_lock, K_FOREVER);
	load_defaults();
	build_tag(tag, sizeof(tag));
	strcpy(find(CFG_BUILD_TAG)->val, tag);
	save_locked();
	k_mutex_unlock(&cfg_lock);
}
