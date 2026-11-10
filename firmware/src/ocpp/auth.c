/*
 * Local authorization: Local Authorization List and Authorization Cache (spec 3.5, 5.10, 5.15).
 *
 * - The local list comes only from the Central System (SendLocalList) and is checked first;
 *   it wins over the cache (spec 3.5.3). Tags that are in the list never stay in the cache.
 * - The cache learns from Authorize.conf / StartTransaction.conf / StopTransaction.conf.
 *   When full: first replace an empty or no-longer-valid entry, else the one used longest ago.
 * - SendLocalList is checked on a staged copy and only applied after it was saved to NVM,
 *   so a failed or partial update never damages the current list.
 * - If the server answers differently from our local list, connector 0 reports
 *   LocalListConflict (see connector.c).
 * Both are persisted (store.c records 3 and 4). Sizes: docs/DECISIONS.md D26.
 */
#include "ocpp/auth.h"
#include "ocpp/clock.h"
#include "ocpp/config.h"
#include "store/store.h"
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
struct auth_list {
	int version;
	struct auth_info entries[AUTH_MAX];
};
static struct auth_list local, staged;
static struct auth_info cache[AUTH_MAX];
static uint32_t uses;
static bool conflict;
static bool parse_info(const struct jp *p, int t, struct auth_info *a)
{
	char ts[40];
	if (!jp_get_str(p, t, "status", a->status, sizeof(a->status)))
		return false;
	if (strcmp(a->status, "Accepted") && strcmp(a->status, "Blocked") &&
	    strcmp(a->status, "Expired") && strcmp(a->status, "Invalid") &&
	    strcmp(a->status, "ConcurrentTx"))
		return false;
	a->parent[0] = 0;
	a->expiry = 0;
	if (jp_get(p, t, "parentIdTag") >= 0 &&
	    !jp_get_str(p, t, "parentIdTag", a->parent, sizeof(a->parent)))
		return false;
	if (jp_get(p, t, "expiryDate") >= 0 &&
	    (!jp_get_str(p, t, "expiryDate", ts, sizeof(ts)) || !clock_parse_iso(ts, &a->expiry)))
		return false;
	return true;
}
static struct auth_info *find(struct auth_info *entries, const char *tag)
{
	for (int i = 0; i < AUTH_MAX; i++)
		if (entries[i].tag[0] && !strcasecmp(entries[i].tag, tag))
			return &entries[i];
	return NULL;
}
bool auth_accepted(const struct auth_info *a)
{
	return a && !strcmp(a->status, "Accepted") &&
	       (!a->expiry || (clock_valid() && clock_now() < a->expiry));
}
void auth_init(void)
{
	if (store_load(STORE_REC_LOCAL, &local, sizeof(local)) != sizeof(local))
		memset(&local, 0, sizeof(local));
	if (store_load(STORE_REC_CACHE, cache, sizeof(cache)) != sizeof(cache))
		memset(cache, 0, sizeof(cache));
	for (int i = 0; i < AUTH_MAX; i++)
		uses = MAX(uses, cache[i].used);
}
const struct auth_info *auth_lookup(const char *tag)
{
	struct auth_info *a =
	    cfg_get_bool("LocalAuthListEnabled") ? find(local.entries, tag) : NULL;
	if (!a && cfg_get_bool("AuthorizationCacheEnabled"))
		a = find(cache, tag);
	if (a)
		a->used = ++uses;
	return a;
}
void auth_update(const char *tag, const struct jp *p, int info)
{
	struct auth_info a = {0}, *dst;
	if (!tag || !*tag || !parse_info(p, info, &a))
		return;
	dst = find(local.entries, tag);
	if (dst) {
		conflict = strcmp(dst->status, a.status) != 0 || strcmp(dst->parent, a.parent) != 0;
		return;
	}
	if (!cfg_get_bool("AuthorizationCacheEnabled"))
		return;
	dst = find(cache, tag);
	if (!dst)
		for (int i = 0; i < AUTH_MAX; i++)
			if (!cache[i].tag[0] || !auth_accepted(&cache[i])) {
				dst = &cache[i];
				break;
			}
	if (!dst) {
		dst = &cache[0];
		for (int i = 1; i < AUTH_MAX; i++)
			if (cache[i].used < dst->used)
				dst = &cache[i];
	}
	strcpy(a.tag, tag);
	a.used = ++uses;
	*dst = a;
	store_save(STORE_REC_CACHE, cache, sizeof(cache));
}
void auth_clear_cache(void)
{
	memset(cache, 0, sizeof(cache));
	store_save(STORE_REC_CACHE, cache, sizeof(cache));
}
int auth_version(void)
{
	if (!cfg_get_bool("LocalAuthListEnabled"))
		return -1;
	for (int i = 0; i < AUTH_MAX; i++)
		if (local.entries[i].tag[0])
			return local.version;
	return 0;
}
bool auth_conflict(void)
{
	return conflict;
}
const char *auth_list_update(const struct jp *p, int pl)
{
	int version, arr = jp_get(p, pl, "localAuthorizationList");
	char type[20];
	if (!jp_get_int(p, pl, "listVersion", &version) || version < 0 ||
	    !jp_get_str(p, pl, "updateType", type, sizeof(type)))
		return "Failed";
	bool full = !strcmp(type, "Full");
	if (!full && strcmp(type, "Differential"))
		return "Failed";
	if (!full && version <= local.version)
		return "VersionMismatch";
	if (arr >= 0 && (!jp_is_type(p, arr, JSMN_ARRAY) || p->tok[arr].size > 16))
		return "Failed";
	if (full)
		memset(&staged, 0, sizeof(staged));
	else
		staged = local;
	for (int n = 0, t = arr + 1; arr >= 0 && n < p->tok[arr].size; n++, t = jp_skip(p, t)) {
		char tag[21];
		struct auth_info *dst;
		int info = jp_get(p, t, "idTagInfo");
		if (!jp_get_str(p, t, "idTag", tag, sizeof(tag)) || !*tag)
			return "Failed";
		/* Duplicate identities in one request are invalid, even in Differential mode. */
		for (int j = 0, u = arr + 1; j < n; j++, u = jp_skip(p, u)) {
			char earlier[21];
			if (jp_get_str(p, u, "idTag", earlier, sizeof(earlier)) &&
			    !strcasecmp(tag, earlier))
				return "Failed";
		}
		dst = find(staged.entries, tag);
		if (info < 0) {
			if (full)
				return "Failed";
			if (dst)
				memset(dst, 0, sizeof(*dst));
			continue;
		}
		if (!dst)
			for (int i = 0; i < AUTH_MAX; i++)
				if (!staged.entries[i].tag[0]) {
					dst = &staged.entries[i];
					break;
				}
		if (!dst || !parse_info(p, info, dst))
			return "Failed";
		strcpy(dst->tag, tag);
	}
	staged.version = version;
	if (cfg_get_bool("_StoreFail") || store_save(STORE_REC_LOCAL, &staged, sizeof(staged)))
		return "Failed";
	local = staged;
	conflict = false;
	/* Entries in the list must never remain in the cache. */
	for (int i = 0; i < AUTH_MAX; i++)
		if (find(local.entries, cache[i].tag))
			memset(&cache[i], 0, sizeof(cache[i]));
	store_save(STORE_REC_CACHE, cache, sizeof(cache));
	return "Accepted";
}
