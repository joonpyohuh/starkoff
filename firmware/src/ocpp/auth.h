#pragma once
#include "ocpp/json.h"
#include <stdint.h>
#define AUTH_MAX 32
struct auth_info {
	char tag[21], status[16], parent[21];
	int64_t expiry;
	uint32_t used;
};
void auth_init(void);
/* NULL means unknown. Returned entry belongs to the auth module. */
const struct auth_info *auth_lookup(const char *tag);
void auth_update(const char *tag, const struct jp *p, int info);
void auth_clear_cache(void);
int auth_version(void);
const char *auth_list_update(const struct jp *p, int payload);
bool auth_accepted(const struct auth_info *a);
bool auth_conflict(void);
