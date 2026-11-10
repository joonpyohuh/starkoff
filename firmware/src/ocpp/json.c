/* JSON helpers. See json.h. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* jsmn implementation is compiled exactly once, here (before json.h, which only
 * pulls in the declarations; jsmn.h has an include guard). */
#define JSMN_STRICT
#include "jsmn.h"
#include "ocpp/json.h"

int jp_parse(struct jp *p, const char *js, size_t len)
{
	jsmn_parser parser;

	p->js = js;
	jsmn_init(&parser);
	p->ntok = jsmn_parse(&parser, js, len, p->tok, JP_MAX_TOKENS);
	return p->ntok;
}

int jp_skip(const struct jp *p, int i)
{
	int end = i + 1;

	if (i < 0 || i >= p->ntok) {
		return p->ntok;
	}
	if (p->tok[i].type == JSMN_OBJECT) {
		for (int k = 0; k < p->tok[i].size; k++) {
			end = jp_skip(p, end);     /* key */
			end = jp_skip(p, end);     /* value */
		}
	} else if (p->tok[i].type == JSMN_ARRAY) {
		for (int k = 0; k < p->tok[i].size; k++) {
			end = jp_skip(p, end);
		}
	}
	return end;
}

bool jp_is_type(const struct jp *p, int i, jsmntype_t t)
{
	return i >= 0 && i < p->ntok && p->tok[i].type == t;
}

bool jp_eq(const struct jp *p, int i, const char *s)
{
	size_t n = strlen(s);

	return jp_is_type(p, i, JSMN_STRING) &&
	       (size_t)(p->tok[i].end - p->tok[i].start) == n &&
	       strncmp(p->js + p->tok[i].start, s, n) == 0;
}

int jp_get(const struct jp *p, int obj, const char *key)
{
	int i;

	if (!jp_is_type(p, obj, JSMN_OBJECT)) {
		return -1;
	}
	i = obj + 1;
	for (int k = 0; k < p->tok[obj].size && i < p->ntok; k++) {
		if (jp_eq(p, i, key)) {
			return i + 1;
		}
		i = jp_skip(p, i + 1); /* skip key's value */
	}
	return -1;
}

bool jp_str(const struct jp *p, int i, char *out, size_t len)
{
	size_t o = 0;

	if (!jp_is_type(p, i, JSMN_STRING) || len == 0) {
		return false;
	}
	for (int k = p->tok[i].start; k < p->tok[i].end; k++) {
		char c = p->js[k];

		if (c == '\\' && k + 1 < p->tok[i].end) {
			c = p->js[++k];
			switch (c) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u': c = '?'; k += 4; break; /* non-ASCII not needed by OCPP 1.6 fields */
			default: break;                   /* \" \\ \/ */
			}
		}
		if (o + 1 >= len) {
			return false;
		}
		out[o++] = c;
	}
	out[o] = '\0';
	return true;
}

bool jp_int(const struct jp *p, int i, int *out)
{
	char tmp[16];
	int n;
	char *end;

	if (!jp_is_type(p, i, JSMN_PRIMITIVE)) {
		return false;
	}
	n = p->tok[i].end - p->tok[i].start;
	if (n <= 0 || n >= (int)sizeof(tmp)) {
		return false;
	}
	memcpy(tmp, p->js + p->tok[i].start, n);
	tmp[n] = '\0';
	*out = (int)strtol(tmp, &end, 10);
	return *end == '\0';
}

bool jp_get_str(const struct jp *p, int obj, const char *key, char *out, size_t len)
{
	return jp_str(p, jp_get(p, obj, key), out, len);
}

bool jp_get_int(const struct jp *p, int obj, const char *key, int *out)
{
	return jp_int(p, jp_get(p, obj, key), out);
}

/* ---------------- writer ---------------- */

void jw_init(struct jw *w, char *buf, size_t len)
{
	w->buf = buf;
	w->len = len;
	w->pos = 0;
	w->err = false;
	if (len) {
		buf[0] = '\0';
	}
}

static void put(struct jw *w, char c)
{
	if (w->pos + 1 >= w->len) {
		w->err = true;
		return;
	}
	w->buf[w->pos++] = c;
	w->buf[w->pos] = '\0';
}

void jw_raw(struct jw *w, const char *s)
{
	while (*s) {
		put(w, *s++);
	}
}

void jw_printf(struct jw *w, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (w->err) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(w->buf + w->pos, w->len - w->pos, fmt, ap);
	va_end(ap);
	if (n < 0 || w->pos + n >= w->len) {
		w->err = true;
		w->buf[w->pos] = '\0';
		return;
	}
	w->pos += n;
}

void jw_str(struct jw *w, const char *s)
{
	put(w, '"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			put(w, '\\');
			put(w, c);
		} else if (c < 0x20) {
			jw_printf(w, "\\u%04x", c);
		} else {
			put(w, c);
		}
	}
	put(w, '"');
}

static void key(struct jw *w, bool *first, const char *k)
{
	if (!*first) {
		put(w, ',');
	}
	*first = false;
	jw_str(w, k);
	put(w, ':');
}

void jw_kv_str(struct jw *w, bool *first, const char *k, const char *val)
{
	key(w, first, k);
	jw_str(w, val);
}

void jw_kv_int(struct jw *w, bool *first, const char *k, long val)
{
	key(w, first, k);
	jw_printf(w, "%ld", val);
}

void jw_kv_raw(struct jw *w, bool *first, const char *k, const char *raw)
{
	key(w, first, k);
	jw_raw(w, raw);
}
