/* SPDX-License-Identifier: GPL-3.0-only
 * World Clock checks. Expected offsets were cross-checked against
 * Python's zoneinfo (IANA tzdata) for 2026 before writing them here.
 */
#include "worldclock.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int64_t utc(int year, int month, int day, int hour, int min)
{
    return wc_days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
           (int64_t)hour * 3600 + (int64_t)min * 60;
}

static const struct wc_city *find(const char *code)
{
    for (int i = 0; i < WC_CITY_COUNT; ++i)
        if (!strcmp(WC_CITIES[i].code, code))
            return &WC_CITIES[i];
    return NULL;
}

static void expect(const char *code, int64_t when, int offset)
{
    const struct wc_city *city = find(code);
    assert(city);
    assert(wc_offset_minutes(city, when) == offset);
}

int main(void)
{
    assert(WC_CITY_COUNT == 16);
    assert(find("LAX") && find("SGN") && find("TYO"));

    /* Civil calendar helpers. */
    assert(wc_days_from_civil(1970, 1, 1) == 0);
    assert(wc_days_from_civil(2024, 2, 29) == 19782);
    assert(wc_weekday(2024, 2, 29) == 4); /* Thursday */
    assert(wc_nth_weekday(2026, 3, 2, 0) == 8);  /* 2nd Sunday Mar 2026 */
    assert(wc_nth_weekday(2026, 3, 5, 0) == 29); /* last Sunday Mar 2026 */
    assert(wc_nth_weekday(2026, 10, 1, 0) == 4); /* 1st Sunday Oct 2026 */
    assert(wc_nth_weekday(2026, 9, 5, 0) == 27); /* last Sunday Sep 2026 */

    /* US DST: forward 2nd Sun Mar 02:00 standard, back 1st Sun Nov 02:00. */
    expect("LAX", utc(2026, 3, 8, 9, 59), -480);
    expect("LAX", utc(2026, 3, 8, 10, 0), -420);
    expect("LAX", utc(2026, 11, 1, 8, 59), -420);
    expect("LAX", utc(2026, 11, 1, 9, 0), -480);
    expect("NYC", utc(2026, 3, 8, 6, 59), -300);
    expect("NYC", utc(2026, 3, 8, 7, 0), -240);
    expect("CHI", utc(2026, 7, 1, 0, 0), -300);
    expect("DEN", utc(2026, 1, 1, 0, 0), -420);
    expect("HNL", utc(2026, 7, 1, 0, 0), -600);
    expect("HNL", utc(2026, 1, 1, 0, 0), -600);

    /* EU DST: forward last Sun Mar 01:00 UTC, back last Sun Oct 01:00 UTC. */
    expect("LON", utc(2026, 3, 29, 0, 59), 0);
    expect("LON", utc(2026, 3, 29, 1, 0), 60);
    expect("LON", utc(2026, 10, 25, 0, 59), 60);
    expect("LON", utc(2026, 10, 25, 1, 0), 0);
    expect("PAR", utc(2026, 3, 29, 0, 59), 60);
    expect("PAR", utc(2026, 3, 29, 1, 0), 120);
    expect("BER", utc(2026, 10, 25, 1, 0), 60);

    /* No-DST zones keep one offset all year; Mumbai is a half hour. */
    expect("MOW", utc(2026, 7, 1, 0, 0), 180);
    expect("DXB", utc(2026, 1, 1, 0, 0), 240);
    expect("BOM", utc(2026, 7, 1, 0, 0), 330);
    expect("SGN", utc(2026, 1, 1, 0, 0), 420);
    expect("SGN", utc(2026, 7, 1, 0, 0), 420);
    expect("SIN", utc(2026, 7, 1, 0, 0), 480);
    expect("TYO", utc(2026, 7, 1, 0, 0), 540);

    /* Southern hemisphere: DST Oct-Apr, opposite to the north. */
    expect("SYD", utc(2026, 1, 15, 0, 0), 660);
    expect("SYD", utc(2026, 4, 4, 15, 59), 660);
    expect("SYD", utc(2026, 4, 4, 16, 0), 600);
    expect("SYD", utc(2026, 10, 3, 15, 59), 600);
    expect("SYD", utc(2026, 10, 3, 16, 0), 660);
    expect("AKL", utc(2026, 9, 26, 13, 59), 720);
    expect("AKL", utc(2026, 9, 26, 14, 0), 780);
    expect("AKL", utc(2026, 4, 4, 13, 59), 780);
    expect("AKL", utc(2026, 4, 4, 14, 0), 720);

    /* Local time: leap-day rollover and date changes across the line. */
    {
        int y, mo, d, h, mi, s, w;
        wc_local_time(find("TYO"), utc(2024, 2, 29, 23, 0), &y, &mo, &d, &h, &mi, &s, &w);
        assert(y == 2024 && mo == 3 && d == 1 && h == 8 && mi == 0 && w == 5);
        wc_local_time(find("LAX"), utc(2024, 2, 29, 23, 0), &y, &mo, &d, &h, &mi, &s, &w);
        assert(y == 2024 && mo == 2 && d == 29 && h == 15 && mi == 0 && w == 4);
        wc_local_time(find("AKL"), utc(2026, 1, 1, 11, 30), &y, &mo, &d, &h, &mi, &s, &w);
        assert(y == 2026 && mo == 1 && d == 2 && h == 0 && mi == 30);
    }

    /* Map picker: every city has a position, country and abbreviation;
     * the longitude order is strictly west to east; taps land on the
     * nearest city, including across the date line. */
    assert(WC_MAP_COUNT == WC_CITY_COUNT);
    for (int i = 0; i < WC_CITY_COUNT; ++i) {
        const struct wc_city *city = &WC_CITIES[i];
        assert(city->lat >= -60 && city->lat <= 85);
        assert(city->lon >= -180 && city->lon <= 180);
        assert(city->country && city->abbr_std);
        if (i)
            assert(WC_CITIES[WC_MAP_ORDER[i - 1]].lon < WC_CITIES[WC_MAP_ORDER[i]].lon);
        assert(wc_map_position(city) >= 0 && wc_map_position(city) < WC_MAP_COUNT);
        assert(&WC_CITIES[WC_MAP_ORDER[wc_map_position(city)]] == city);
    }
    assert(wc_map_position(find("HNL")) == 0);
    assert(wc_map_position(find("LAX")) == 1);
    assert(wc_map_position(find("SGN")) == 12);
    assert(wc_map_position(find("SIN")) == 11);
    assert(wc_map_position(find("TYO")) == 13);
    assert(wc_map_position(find("AKL")) == 15);
    assert(wc_nearest_city(-118.2, 34.0) == find("LAX"));
    assert(wc_nearest_city(139.7, 35.7) == find("TYO"));
    assert(wc_nearest_city(-0.1, 51.5) == find("LON"));
    assert(wc_nearest_city(174.8, -36.9) == find("AKL"));
    assert(wc_nearest_city(106.6, 10.8) == find("SGN"));
    assert(wc_nearest_city(103.8, 1.3) == find("SIN"));
    assert(wc_nearest_city(151.2, -33.9) == find("SYD"));
    assert(wc_nearest_city(-179.0, -37.0) == find("AKL")); /* date line wrap */
    assert(wc_nearest_longitude(-100.0) == find("DEN"));
    assert(wc_nearest_longitude(150.0) == find("SYD"));
    assert(wc_nearest_longitude(179.0) == find("AKL"));

    puts("World Clock checks passed");
    return 0;
}
