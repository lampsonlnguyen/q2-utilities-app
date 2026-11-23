/* SPDX-License-Identifier: GPL-3.0-only
 * World Clock: a fixed preset of cities with embedded DST rules.
 *
 * Why embedded rules, not tzdata: the Q2's stock V1.36 sysroot has no
 * /usr/share/zoneinfo at all, and the firmware gives the app no timezone
 * database to read, so localtime_r/TZ cannot resolve another city's zone
 * on-device. Each city below carries its IANA base offset plus the
 * transition rule that IANA tzdata uses for it, checked against Python's
 * zoneinfo for 2026 (see tests/test_worldclock.c). If a government
 * changes a rule, this table — not the device — needs updating.
 *
 * Transition rules are expressed in local standard time: the DST start
 * and end instants are the nth weekday of a month at a given hour in
 * standard time, so the UTC instant is that local time minus the base
 * offset. That single form covers the US (02:00 standard to start,
 * 02:00 daylight = 01:00 standard to end), the EU (01:00 UTC, i.e.
 * 01:00 standard in London, 02:00 in Paris/Berlin) and the southern
 * hemisphere (02:00 standard at both ends).
 * week 5 means the last such weekday of the month.
 */
#ifndef UTILITIES_WORLDCLOCK_H
#define UTILITIES_WORLDCLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wc_rule {
    int start_month, start_week, start_dow, start_hour;
    int end_month, end_week, end_dow, end_hour;
    bool southern; /* DST spans the year boundary (Oct-Apr) */
};

struct wc_city {
    const char *code; /* Casio AE1200-style 3-letter code, e.g. TYO, NYC */
    const char *name;
    const char *zone; /* IANA name, for documentation only */
    int base_min;     /* Standard-time offset from UTC, minutes */
    const struct wc_rule *dst; /* NULL: no daylight saving */
    /* Map picker: position for the tiny world map, country and the
     * abbreviations the Ubuntu-style tooltip shows ("PDT (UTC-07)"). */
    double lat, lon;
    const char *country;
    const char *abbr_std, *abbr_dst; /* abbr_dst NULL: no daylight saving */
};

extern const struct wc_city WC_CITIES[];
extern const int WC_CITY_COUNT;
/* City indices in WC_CITIES, west to east by longitude: the order the
 * map picker's wheel steps through, so the pin travels across the map. */
extern const int WC_MAP_ORDER[];
extern const int WC_MAP_COUNT;

/* Days since 1970-01-01 (Howard Hinnant's civil algorithm) and back. */
int64_t wc_days_from_civil(int year, unsigned month, unsigned day);
void wc_civil_from_days(int64_t days, int *year, unsigned *month, unsigned *day);
/* Weekday of a date, Sunday = 0 (same convention as struct tm). */
int wc_weekday(int year, unsigned month, unsigned day);
/* Day-of-month of the nth (week 1-4) or last (week 5) given weekday. */
int wc_nth_weekday(int year, int month, int week, int dow);

/* UTC offset in minutes for a city at a UTC epoch (seconds). */
int wc_offset_minutes(const struct wc_city *city, int64_t utc_seconds);
bool wc_is_dst(const struct wc_city *city, int64_t utc_seconds);

/* Local civil time for a city at a UTC epoch. */
void wc_local_time(const struct wc_city *city, int64_t utc_seconds,
                   int *year, int *month, int *mday, int *hour, int *min,
                   int *sec, int *wday);

/* Map picker: the city nearest a tapped longitude/latitude (longitude
 * wraps at the date line), and the city whose longitude is nearest
 * (for taps on the GMT strip below the map). */
const struct wc_city *wc_nearest_city(double lon, double lat);
const struct wc_city *wc_nearest_longitude(double lon);
/* Position of a city in WC_MAP_ORDER (0 = westernmost), or 0. */
int wc_map_position(const struct wc_city *city);

#endif
