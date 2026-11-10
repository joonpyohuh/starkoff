/* UTC wall clock synced from the Central System. See clock.c. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_ISO_LEN 25 /* "2026-11-10T12:34:56Z" + NUL, with margin */

bool clock_parse_iso(const char *s, int64_t *epoch_out);
void clock_format_iso(int64_t epoch, char *out, size_t len);
bool clock_sync_iso(const char *iso);
bool clock_valid(void);
int64_t clock_now(void);            /* seconds since 1970 (UTC) */
void clock_now_iso(char *out, size_t len);
