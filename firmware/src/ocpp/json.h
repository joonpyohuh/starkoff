/*
 * Small JSON helpers.
 * Reading: jsmn (lib/jsmn, MIT) splits the text into tokens without allocating memory;
 *          these helpers find keys and copy values with bounds checks.
 * Writing: append to a fixed buffer; any overflow sets w->err (message is then dropped).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#define JSMN_HEADER
#define JSMN_STRICT
#include "jsmn.h"

#define JP_MAX_TOKENS 160

struct jp {
	const char *js;
	int ntok;
	jsmntok_t tok[JP_MAX_TOKENS];
};

/* Returns number of tokens, or <0 on parse error. */
int jp_parse(struct jp *p, const char *js, size_t len);
/* Index of the token after token i and all its children. */
int jp_skip(const struct jp *p, int i);
/* Value token of "key" inside object token obj, or -1. */
int jp_get(const struct jp *p, int obj, const char *key);
/* Token i is a string equal to s? */
bool jp_eq(const struct jp *p, int i, const char *s);
/* Copy string token (unescaped) into out. False if missing / not a string / too long. */
bool jp_str(const struct jp *p, int i, char *out, size_t len);
/* Integer value of a primitive token. */
bool jp_int(const struct jp *p, int i, int *out);
bool jp_is_type(const struct jp *p, int i, jsmntype_t t);
/* Shortcuts on an object: copy string / int of a key. */
bool jp_get_str(const struct jp *p, int obj, const char *key, char *out, size_t len);
bool jp_get_int(const struct jp *p, int obj, const char *key, int *out);

struct jw {
	char *buf;
	size_t len;
	size_t pos;
	bool err;
};

void jw_init(struct jw *w, char *buf, size_t len);
void jw_raw(struct jw *w, const char *s);
void jw_printf(struct jw *w, const char *fmt, ...);
/* Appends "s" as a quoted, escaped JSON string. */
void jw_str(struct jw *w, const char *s);
/* Appends "key":"value" (with a leading comma if first == false). */
void jw_kv_str(struct jw *w, bool *first, const char *key, const char *val);
void jw_kv_int(struct jw *w, bool *first, const char *key, long val);
void jw_kv_raw(struct jw *w, bool *first, const char *key, const char *raw);
