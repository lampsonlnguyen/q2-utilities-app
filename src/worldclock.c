/* SPDX-License-Identifier: GPL-3.0-only */
#include "worldclock.h"

/* DST rules, one per region. start/end hour is local standard time. */
/* US: forward 2nd Sun Mar 02:00 standard; back 1st Sun Nov 02:00 daylight,
 * which is 01:00 standard — the end hour is in standard time. */
static const struct wc_rule WC_US = {3, 2, 0, 2, 11, 1, 0, 1, false};
static const struct wc_rule WC_EU_GMT = {3, 5, 0, 1, 10, 5, 0, 1, false};
static const struct wc_rule WC_EU_CET = {3, 5, 0, 2, 10, 5, 0, 2, false};
static const struct wc_rule WC_SYD = {10, 1, 0, 2, 4, 1, 0, 2, true};
static const struct wc_rule WC_AKL = {9, 5, 0, 2, 4, 1, 0, 2, true};

const struct wc_city WC_CITIES[] = {
    {"LAX", "Los Angeles", "America/Los_Angeles", -480, &WC_US, 34.05, -118.24, "United States", "PST", "PDT"},
    {"NYC", "New York", "America/New_York", -300, &WC_US, 40.71, -74.01, "United States", "EST", "EDT"},
    {"CHI", "Chicago", "America/Chicago", -360, &WC_US, 41.88, -87.63, "United States", "CST", "CDT"},
    {"DEN", "Denver", "America/Denver", -420, &WC_US, 39.74, -104.99, "United States", "MST", "MDT"},
    {"HNL", "Honolulu", "Pacific/Honolulu", -600, NULL, 21.31, -157.86, "United States", "HST", NULL},
    {"LON", "London", "Europe/London", 0, &WC_EU_GMT, 51.51, -0.13, "United Kingdom", "GMT", "BST"},
    {"PAR", "Paris", "Europe/Paris", 60, &WC_EU_CET, 48.85, 2.35, "France", "CET", "CEST"},
    {"BER", "Berlin", "Europe/Berlin", 60, &WC_EU_CET, 52.52, 13.40, "Germany", "CET", "CEST"},
    {"MOW", "Moscow", "Europe/Moscow", 180, NULL, 55.76, 37.62, "Russia", "MSK", NULL},
    {"DXB", "Dubai", "Asia/Dubai", 240, NULL, 25.20, 55.27, "United Arab Emirates", "GST", NULL},
    {"BOM", "Mumbai", "Asia/Kolkata", 330, NULL, 19.08, 72.88, "India", "IST", NULL},
    {"SGN", "Ho Chi Minh City", "Asia/Ho_Chi_Minh", 420, NULL, 10.82, 106.63, "Vietnam", "ICT", NULL},
    {"SIN", "Singapore", "Asia/Singapore", 480, NULL, 1.35, 103.82, "Singapore", "SGT", NULL},
    {"TYO", "Tokyo", "Asia/Tokyo", 540, NULL, 35.68, 139.69, "Japan", "JST", NULL},
    {"SYD", "Sydney", "Australia/Sydney", 600, &WC_SYD, -33.87, 151.21, "Australia", "AEST", "AEDT"},
    {"AKL", "Auckland", "Pacific/Auckland", 720, &WC_AKL, -36.85, 174.76, "New Zealand", "NZST", "NZDT"},
};
const int WC_CITY_COUNT = (int)(sizeof WC_CITIES / sizeof WC_CITIES[0]);

/* West to east: HNL, LAX, DEN, CHI, NYC, LON, PAR, BER, MOW, DXB, BOM,
 * SIN, SGN, TYO, SYD, AKL. Singapore (103.82E) sits just west of
 * Ho Chi Minh City (106.63E). */
const int WC_MAP_ORDER[] = {4, 0, 3, 2, 1, 5, 6, 7, 8, 9, 10, 12, 11, 13, 14, 15};
const int WC_MAP_COUNT = (int)(sizeof WC_MAP_ORDER / sizeof WC_MAP_ORDER[0]);

int wc_map_position(const struct wc_city *city)
{
    for (int i = 0; i < WC_MAP_COUNT; ++i)
        if (&WC_CITIES[WC_MAP_ORDER[i]] == city)
            return i;
    return 0;
}

static double wrap_lon(double dlon)
{
    while (dlon > 180.0)
        dlon -= 360.0;
    while (dlon < -180.0)
        dlon += 360.0;
    return dlon;
}

const struct wc_city *wc_nearest_city(double lon, double lat)
{
    /* Plain equirectangular distance, longitude wrapped at the date
     * line. The cities are far apart, so no cos(lat) scaling is needed
     * to tell them apart; this also keeps libm out of the Q2 build. */
    const struct wc_city *best = &WC_CITIES[0];
    double best_d2 = 1e30;
    for (int i = 0; i < WC_CITY_COUNT; ++i) {
        double dx = wrap_lon(WC_CITIES[i].lon - lon);
        double dy = WC_CITIES[i].lat - lat;
        double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = &WC_CITIES[i];
        }
    }
    return best;
}

const struct wc_city *wc_nearest_longitude(double lon)
{
    const struct wc_city *best = &WC_CITIES[0];
    double best_d = 1e30;
    for (int i = 0; i < WC_CITY_COUNT; ++i) {
        double d = wrap_lon(WC_CITIES[i].lon - lon);
        if (d < 0)
            d = -d;
        if (d < best_d) {
            best_d = d;
            best = &WC_CITIES[i];
        }
    }
    return best;
}

int64_t wc_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void wc_civil_from_days(int64_t days, int *year, unsigned *month, unsigned *day)
{
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp + (mp < 10 ? 3 : -9);
    *year = (int)(y + (*month <= 2));
}

int wc_weekday(int year, unsigned month, unsigned day)
{
    /* 1970-01-01 was a Thursday; days_from_civil(1970,1,1) == 0. */
    int64_t d = wc_days_from_civil(year, month, day) + 4;
    int w = (int)(d % 7);
    return w < 0 ? w + 7 : w;
}

int wc_nth_weekday(int year, int month, int week, int dow)
{
    int first = wc_weekday(year, (unsigned)month, 1);
    int day = 1 + (dow - first + 7) % 7 + (week - 1) * 7;
    if (week == 5) {
        /* Last such weekday: the 5th occurrence, or the 4th if the
         * month is too short for a 5th. */
        static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int dim = days[month - 1];
        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
            dim = 29;
        if (day > dim)
            day -= 7;
    }
    return day;
}

static int64_t transition_utc(const struct wc_city *city, int year, bool start)
{
    const struct wc_rule *r = city->dst;
    int month = start ? r->start_month : r->end_month;
    int week = start ? r->start_week : r->end_week;
    int dow = start ? r->start_dow : r->end_dow;
    int hour = start ? r->start_hour : r->end_hour;
    int day = wc_nth_weekday(year, month, week, dow);
    int64_t local_std = wc_days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
                        (int64_t)hour * 3600;
    return local_std - (int64_t)city->base_min * 60;
}

bool wc_is_dst(const struct wc_city *city, int64_t utc_seconds)
{
    if (!city->dst)
        return false;
    int64_t days = utc_seconds / 86400;
    if (utc_seconds < 0 && utc_seconds % 86400)
        --days;
    int year;
    unsigned month, day;
    wc_civil_from_days(days, &year, &month, &day);
    if (!city->dst->southern) {
        return utc_seconds >= transition_utc(city, year, true) &&
               utc_seconds < transition_utc(city, year, false);
    }
    /* Southern hemisphere: DST runs start(year) .. end(year+1), and
     * end(year) .. start(year) is standard time. */
    if (utc_seconds >= transition_utc(city, year, true) ||
        utc_seconds < transition_utc(city, year, false))
        return true;
    return false;
}

int wc_offset_minutes(const struct wc_city *city, int64_t utc_seconds)
{
    return city->base_min + (wc_is_dst(city, utc_seconds) ? 60 : 0);
}

void wc_local_time(const struct wc_city *city, int64_t utc_seconds,
                   int *year, int *month, int *mday, int *hour, int *min,
                   int *sec, int *wday)
{
    int64_t local = utc_seconds + (int64_t)wc_offset_minutes(city, utc_seconds) * 60;
    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    unsigned m, d;
    wc_civil_from_days(days, year, &m, &d);
    *month = (int)m;
    *mday = (int)d;
    *hour = (int)(rem / 3600);
    *min = (int)(rem / 60 % 60);
    *sec = (int)(rem % 60);
    *wday = wc_weekday(*year, (unsigned)*month, (unsigned)*mday);
}
