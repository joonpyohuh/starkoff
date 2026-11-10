/*
 * Wall clock (UTC only, docs/DESIGN_core.md section 1).
 *
 * The charger has no battery-backed RTC. The Central System tells us the time in
 * BootNotification.conf / Heartbeat.conf (currentTime). We remember
 *     base_epoch_ms = that time (milliseconds since 1970, UTC)
 *     base_uptime = our uptime (ms) when we received it
 * and compute   now = (base_epoch_ms + (uptime_now - base_uptime)) / 1000.
 * Before the first sync the clock is "not valid"; callers then still get a
 * timestamp (1970 + uptime) but can check clock_valid().
 */
#include "ocpp/clock.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>

/* Milliseconds, not seconds: rounding both the server time and the elapsed time down
 * to whole seconds made our clock up to ~2 s late (reservations expired late, TC_047). */
static int64_t base_epoch_ms; /* server time at sync, ms since 1970 */
static int64_t base_uptime;   /* our uptime at sync, ms */
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

static bool parse_iso_ms(const char *s, int64_t *ms_out)
{
	int frac_ms = 0;

	int y, mo, d, h, mi, sec;
	int n = 0;
	int64_t t;

	/* YYYY-MM-DDTHH:MM:SS[.fff][Z|+hh:mm|-hh:mm] */
	if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6) {
		return false;
	}
	static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (y < 1970 || mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
	    sec < 0 || sec > 59) {
		return false;
	}
	int days = month_days[mo - 1] + (mo == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
	if (d > days) {
		return false;
	}
	t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
	s += n;
	if (*s == '.') {
		int digits = 0;

		s++;
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
		while (isdigit((unsigned char)*s)) {
			if (digits < 3) { /* keep milliseconds, ignore finer digits */
				frac_ms = frac_ms * 10 + (*s - '0');
				digits++;
			}
			s++;
		}
		while (digits++ < 3) {
			frac_ms *= 10;
		}
	}
	if (*s == 'Z' && !s[1]) {
	} else if ((*s == '+' || *s == '-') && strlen(s) == 6 && s[3] == ':' &&
		   isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) &&
		   isdigit((unsigned char)s[4]) && isdigit((unsigned char)s[5])) {
		int oh = (s[1] - '0') * 10 + s[2] - '0', om = (s[4] - '0') * 10 + s[5] - '0';
		if (oh > 23 || om > 59) {
			return false;
		}
		t += (*s == '+' ? -1 : 1) * (oh * 3600 + om * 60);
	} else {
		return false;
	}
	*ms_out = t * 1000 + frac_ms;
	return true;
}

bool clock_parse_iso(const char *s, int64_t *epoch_out)
{
	int64_t ms;

	if (!parse_iso_ms(s, &ms)) {
		return false;
	}
	*epoch_out = ms / 1000; /* dates are after 1970 (checked), so this rounds down */
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
	snprintf(out, len, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d, (int)(rem / 3600),
		 (int)(rem % 3600 / 60), (int)(rem % 60));
}

bool clock_sync_iso(const char *iso)
{
	int64_t ms;

	if (!iso || !parse_iso_ms(iso, &ms)) {
		return false;
	}
	base_epoch_ms = ms;
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
	return (base_epoch_ms + (k_uptime_get() - base_uptime)) / 1000;
}

void clock_now_iso(char *out, size_t len)
{
	clock_format_iso(clock_now(), out, len);
}
