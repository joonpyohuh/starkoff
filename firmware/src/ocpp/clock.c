/*
 * Wall clock (UTC only, DESIGN_core_v1 section 1).
 *
 * The charger has no battery-backed RTC. The Central System tells us the time in
 * BootNotification.conf / Heartbeat.conf (currentTime). We remember
 *     base_epoch  = that time (seconds since 1970, UTC)
 *     base_uptime = our uptime (ms) when we received it
 * and compute   now = base_epoch + (uptime_now - base_uptime).
 * Before the first sync the clock is "not valid"; callers then still get a
 * timestamp (1970 + uptime) but can check clock_valid().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "ocpp/clock.h"

static int64_t base_epoch;  /* seconds */
static int64_t base_uptime; /* ms */
static bool valid;

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = (unsigned)(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;

	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp + (mp < 10 ? 3 : -9);
	*y = (int)(yoe + era * 400) + (*m <= 2);
}

bool clock_parse_iso(const char *s, int64_t *epoch_out)
{
	int y, mo, d, h, mi, sec;
	int n = 0;
	int64_t t;

	/* YYYY-MM-DDTHH:MM:SS[.fff][Z|+hh:mm|-hh:mm] */
	if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6) {
		return false;
	}
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
		return false;
	}
	t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
	s += n;
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			s++;
		}
	}
	if (*s == '+' || *s == '-') {
		int oh = 0, om = 0;

		if (sscanf(s + 1, "%2d:%2d", &oh, &om) < 1) {
			return false;
		}
		t += (*s == '+' ? -1 : 1) * (oh * 3600 + om * 60);
	}
	*epoch_out = t;
	return true;
}

void clock_format_iso(int64_t epoch, char *out, size_t len)
{
	int64_t days = epoch / 86400;
	int64_t rem = epoch % 86400;
	int y;
	unsigned m, d;

	if (rem < 0) {
		rem += 86400;
		days--;
	}
	civil_from_days(days, &y, &m, &d);
	snprintf(out, len, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d,
		 (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60));
}

bool clock_sync_iso(const char *iso)
{
	int64_t t;

	if (!iso || !clock_parse_iso(iso, &t)) {
		return false;
	}
	base_epoch = t;
	base_uptime = k_uptime_get();
	valid = true;
	return true;
}

bool clock_valid(void)
{
	return valid;
}

int64_t clock_now(void)
{
	return base_epoch + (k_uptime_get() - base_uptime) / 1000;
}

void clock_now_iso(char *out, size_t len)
{
	clock_format_iso(clock_now(), out, len);
}
