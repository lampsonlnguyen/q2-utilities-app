/* SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "q2pod_scheme.h"
#include "worldclock.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Q2 Pod theme IDs/accent colors: assets/themes and patch/offsets.inc. */
static uint32_t BG=0, PANEL=0x191919, INK=0xeeeeec, MUTED=0x949294,
                ACCENT=0xeeeeec, SELECT=0xeeeeec, SELECT_INK=0x161616;
/* The resolved q2-pod scheme (src/q2pod_scheme.c): bar gradient, selection
 * painter and surfaces for the parsed theme, accent and light/dark half. */
static Q2PodScheme SCHEME;
static int config_number(const char *config, const char *key, int fallback, int limit) {
    int ipod=0;
    while (*config) {
        const char *end=config;
        while (*end && *end!='\n') ++end;
        const char *p=config;
        while (p<end && (*p==' ' || *p=='\t')) ++p;
        if (p<end && *p=='[') {
            const char section[]="[IPOD]";
            int i=0;
            while (p+i<end && section[i] && p[i]==section[i]) ++i;
            ipod=i==6;
        } else if (ipod) {
            int i=0;
            while (p+i<end && key[i] && p[i]==key[i]) ++i;
            if (!key[i]) {
                p+=i;
                while (p<end && (*p==' ' || *p=='\t')) ++p;
                if (p<end && *p++=='=') {
                    while (p<end && (*p==' ' || *p=='\t')) ++p;
                    unsigned value=0; int digits=0;
                    while (p<end && *p>='0' && *p<='9') {
                        value=value*10+(unsigned)(*p++-'0');
                        if (value>=(unsigned)limit) return fallback;
                        digits=1;
                    }
                    if (digits && (p==end || *p=='\r' || *p==' ' || *p=='\t' || *p==';' || *p=='#'))
                        return (int)value;
                }
            }
        }
        config=*end?end+1:end;
    }
    return fallback;
}
void app_theme(const char *config, int hour)
{
    static const uint32_t accents[]={0x424242,0xe8123f,0x13838d,0x8c732c,
        0x2a6ad8,0xc45400,0x903868,0x424242,0x647f34};
    static const uint32_t bright[]={0xd8d8d8,0xff1448,0x30929b,0x9a8446,
        0x447cdd,0xcb681e,0x9d4f7a,0xd8d8d8,0x778e4c};
    int theme=config_number(config,"THEME",0,3);
    const char *key=theme==0?"MINIMAL_ACCENT":theme==1?"CLASSIC_LIGHT_ACCENT":"ACCENT";
    int raw_accent=config_number(config,key,0,9);
    int accent=raw_accent;
    if(theme==1)
        accent=accent==0?4:accent==8?8:accent-1;
    else if(theme==2 && accent==7)
        accent=0;
    int mode=config_number(config,"LIGHT",0,3);
    bool light=mode==1 || (mode==2 && hour>=7 && hour<19);
    /* Classic accent 0 is Classic Blue, its own gloss band (accent 9 in
     * q2pod_scheme); the other Classic values follow the flat mapping
     * above. Minimal/Modern use the mapped accent. */
    q2pod_scheme_resolve(theme, theme==1 ? (raw_accent==0 ? 9 : accent) : accent,
                         light, &SCHEME);
    BG=SCHEME.paper;
    PANEL=light?0xe7e7e7:0x191919;
    INK=SCHEME.ink;
    MUTED=SCHEME.muted;
    SELECT=accents[accent]; SELECT_INK=0xffffff;
    ACCENT=light?accents[accent]:bright[accent];
    if(theme==0 && (accent==0 || accent==7)) {
        SELECT=light?0x161616:0xeeeeec;
        ACCENT=INK; SELECT_INK=BG;
    }
}

enum { RETURN_KEY = 170, CENTRE_KEY = 218, WHEEL_STEP = 9, TASKS_PER_PAGE = 5,
       WC_ROW_H = 52, WC_ROWS = 5,
       /* Tiny world map (WC_MAP): equirectangular, lon -180..180 across,
        * lat 84..-58 down, so 1 degree is about 1 pixel each way. */
       WC_MAP_X = 6, WC_MAP_Y = 30, WC_MAP_W = 363, WC_MAP_H = 170,
       WC_MAP_LAT_TOP = 84, WC_MAP_LAT_SPAN = 142,
       WC_MAP_STRIP_Y = 202, WC_MAP_INFO_Y = 230, WC_MAP_HINT_Y = 297 };

struct button {
    int x, y, w, h, code;
    char label[32];
};

struct layout {
    struct button items[36];
    int count;
};

static const char *const titles[] = {
    "Utilities", "Stopwatch", "Calendar", "To-do", "Calculator",
    "World Clock", "World Clock", "New task", "Task options", "World map"
};

static const char *const months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static void screen(struct app *app, enum screen next)
{
    app->screen = next;
    app->focus = app->wheel = 0;
    app->touch_button = -1;
    if (next == HOME)
        app->focus = app->home_focus;
    if (next == WORLD_CLOCK)
        app->wc_top = 0;
}

void app_init(struct app *app, const char *task_path)
{
    *app = (struct app){ .task_path = task_path, .touch_button = -1, .number = "0" };
    FILE *file = fopen(task_path, "r");
    if (!file) {
        app->load_failed = errno != ENOENT;
        return;
    }
    char line[TASK_LENGTH + 5];
    while (app->task_count < TASK_LIMIT && fgets(line, sizeof line, file)) {
        size_t length = strcspn(line, "\r\n");
        if (length < 3 || length > TASK_LENGTH + 2 || !strchr("01", line[0]) ||
            line[1] != '\t' || line[length] == '\0') {
            app->load_failed = true;
            break;
        }
        line[length] = '\0';
        struct task *task = &app->tasks[app->task_count++];
        task->done = line[0] == '1';
        memcpy(task->text, line + 2, length - 1);
    }
    if (ferror(file) || (app->task_count == TASK_LIMIT && fgetc(file) != EOF))
        app->load_failed = true;
    fclose(file);
}

static bool save_tasks(const struct app *app)
{
    if (app->load_failed)
        return false;
    char temp[512];
    int length = snprintf(temp, sizeof temp, "%s.XXXXXX", app->task_path);
    if (length < 0 || (size_t)length >= sizeof temp)
        return false;
    int fd = mkstemp(temp);
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temp);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < app->task_count && ok; ++i)
        ok = fprintf(file, "%d\t%s\n", app->tasks[i].done, app->tasks[i].text) >= 0;
    ok = ok && fflush(file) == 0 && fsync(fd) == 0;
    if (fclose(file) != 0)
        ok = false;
    if (ok && rename(temp, app->task_path) == 0)
        return true;
    unlink(temp);
    return false;
}

static int64_t elapsed(const struct app *app, int64_t now)
{
    return app->elapsed + (app->running ? now - app->started : 0);
}

static void format_elapsed(char *text, size_t size, int64_t ms)
{
    snprintf(text, size, "%02lld:%02lld:%02lld.%01lld",
             (long long)(ms / 3600000), (long long)(ms / 60000 % 60),
             (long long)(ms / 1000 % 60), (long long)(ms / 100 % 10));
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month] + (month == 1 && year % 4 == 0 &&
                         (year % 100 != 0 || year % 400 == 0));
}

static int first_weekday(int year, int month)
{
    /* Gregorian calendar, Sunday = 0. */
    static const int offsets[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    year -= month < 2;
    return (year + year / 4 - year / 100 + year / 400 + offsets[month] + 1) % 7;
}

static void set_today(struct app *app, const struct tm *today)
{
    app->year = today->tm_year + 1900;
    app->month = today->tm_mon;
}

static void move_month(struct app *app, int delta)
{
    int month = app->year * 12 + app->month + delta;
    if (month >= 12 && month < 10000 * 12) {
        app->year = month / 12;
        app->month = month % 12;
    }
}

static void calculate(struct app *app, int key)
{
    size_t length = strlen(app->number);
    if (key == 'C') {
        strcpy(app->number, "0");
        app->op = 0;
        app->total = 0;
        app->entering = app->error = false;
        return;
    }
    if (app->error)
        return;
    if (key == '<') {
        if (app->entering && length > 1)
            app->number[length - 1] = '\0';
        else if (app->entering)
            strcpy(app->number, "0");
        return;
    }
    if ((key >= '0' && key <= '9') || key == '.') {
        if (!app->entering) {
            strcpy(app->number, "0");
            app->entering = true;
            length = 1;
        }
        if (key == '.' && strchr(app->number, '.'))
            return;
        if (key != '.' && strcmp(app->number, "0") == 0)
            length = 0;
        if (length < 14) {
            app->number[length++] = (char)key;
            app->number[length] = '\0';
        }
        return;
    }
    if (app->entering || !app->op) {
        double value = app->entering ? strtod(app->number, NULL) : app->total;
        switch (app->op) {
        case '+': app->total += value; break;
        case '-': app->total -= value; break;
        case '*': app->total *= value; break;
        case '/':
            if (value == 0) {
                app->error = true;
                strcpy(app->number, "Divide by zero");
                return;
            }
            app->total /= value;
            break;
        default: app->total = value; break;
        }
        if (!isfinite(app->total)) {
            app->error = true;
            strcpy(app->number, "Out of range");
            return;
        }
        snprintf(app->number, sizeof app->number, "%.12g", app->total);
    } else if (key == '=') {
        return;
    }
    app->op = key == '=' ? 0 : (char)key;
    app->entering = false;
}

static void button(struct layout *layout, int x, int y, int w, int h, int code, const char *label)
{
    struct button *item = &layout->items[layout->count++];
    *item = (struct button){ .x = x, .y = y, .w = w, .h = h, .code = code };
    snprintf(item->label, sizeof item->label, "%s", label);
}

static void make_layout(const struct app *app, struct layout *layout)
{
    layout->count = 0;
    switch (app->screen) {
    case HOME: {
        /* q2-pod lists: 32 px full-width rows from below the 26 px bar.
         * World Clock sits after Stopwatch as the Clock grouping; the
         * other tools keep their relative order. */
        static const enum screen order[] = {STOPWATCH, WORLD_CLOCK, CALENDAR, TODO, CALCULATOR};
        for (unsigned i = 0; i < sizeof order / sizeof order[0]; ++i)
            button(layout, 0, 26 + (int)i * 32, WIDTH, 32, order[i], titles[order[i]]);
        break;
    }
    case WORLD_CLOCK: {
        /* 52 px detail rows (GNOME Clocks-style two-line rows): five
         * visible at a time, the list scrolls under the focus. The
         * 34 px strip below the rows opens the world map picker. */
        for (int i = 0; i < WC_ROWS && app->wc_top + i < WC_CITY_COUNT; ++i)
            button(layout, 0, 26 + i * WC_ROW_H, WIDTH, WC_ROW_H,
                   app->wc_top + i, "");
        button(layout, 0, 26 + WC_ROWS * WC_ROW_H, WIDTH,
               HEIGHT - 26 - WC_ROWS * WC_ROW_H, WC_CITY_COUNT, "World map");
        break;
    }
    case WC_DETAIL:
        /* Detail view has no buttons; Return goes back to the list. */
        break;
    case WC_MAP:
        /* The map picker has no list buttons; its input is separate. */
        break;
    case STOPWATCH:
        button(layout, 18, 127, 108, 39, 0, app->running ? "Pause" : "Start");
        button(layout, 134, 127, 107, 39, 1, "Lap");
        button(layout, 249, 127, 108, 39, 2, "Reset");
        break;
    case CALENDAR:
        button(layout, 18, 247, 108, 42, -1, "< Month");
        button(layout, 134, 247, 107, 42, 0, "Today");
        button(layout, 249, 247, 108, 42, 1, "Month >");
        break;
    case TODO: {
        int start = app->task_page * TASKS_PER_PAGE;
        for (int i = start; i < app->task_count && i < start + TASKS_PER_PAGE; ++i) {
            char label[32];
            snprintf(label, sizeof label, "[%c] %s", app->tasks[i].done ? 'X' : ' ', app->tasks[i].text);
            button(layout, 0, 26 + (i - start) * 32, WIDTH, 32, i, label);
        }
        if (app->task_count < TASK_LIMIT)
            button(layout, 18, 243, 130, 44, -1, "+ New task");
        if (start)
            button(layout, 156, 243, 95, 44, -2, "< Page");
        if (start + TASKS_PER_PAGE < app->task_count)
            button(layout, 259, 243, 98, 44, -3, "Page >");
        break;
    }
    case TASK_ACTIONS:
        /* List rows below the task text (drawn at y=36). */
        button(layout, 0, 66, WIDTH, 32, 0,
               app->tasks[app->task_index].done ? "Mark open" : "Mark done");
        button(layout, 0, 98, WIDTH, 32, 1, "Delete task");
        break;
    case EDIT_TASK: {
        const char *keys = app->symbols ? "0123456789.,!?-+/:@#&()'" : "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        for (int i = 0; keys[i]; ++i) {
            char label[] = {keys[i], 0};
            button(layout, 18 + i % 8 * 43, 99 + i / 8 * 35, 38, 30, keys[i], label);
        }
        button(layout, 18, 245, 77, 39, 1, app->symbols ? "ABC" : "123");
        button(layout, 101, 245, 82, 39, ' ', "Space");
        button(layout, 189, 245, 75, 39, 2, "Erase");
        button(layout, 270, 245, 87, 39, 3, "Save");
        break;
    }
    case CALCULATOR: {
        const char *keys = "789/456*123-0.=+C<";
        for (int i = 0; keys[i]; ++i) {
            char label[] = {keys[i], 0};
            button(layout, 18 + i % 4 * 86, 96 + i / 4 * 39, 81, 34, keys[i], label);
        }
        break;
    }
    }
}

static void task_action(struct app *app, int code)
{
    int index = app->task_index;
    struct task old = app->tasks[index];
    if (code == 0) {
        app->tasks[index].done = !old.done;
    } else {
        --app->task_count;
        memmove(app->tasks + index, app->tasks + index + 1,
                (size_t)(app->task_count - index) * sizeof old);
    }
    app->save_failed = !save_tasks(app);
    if (app->save_failed) {
        if (code != 0) {
            memmove(app->tasks + index + 1, app->tasks + index,
                    (size_t)(app->task_count - index) * sizeof old);
            ++app->task_count;
        }
        app->tasks[index] = old;
    }
    if (app->task_page && app->task_page * TASKS_PER_PAGE >= app->task_count)
        --app->task_page;
    screen(app, TODO);
}

static void edit_task(struct app *app, int code)
{
    size_t length = strlen(app->draft);
    if (code == 1) {
        app->symbols = !app->symbols;
        app->focus = 0;
    } else if (code == 2 && length) {
        app->draft[length - 1] = '\0';
    } else if (code == 3) {
        const char *start = app->draft + strspn(app->draft, " ");
        while (length && app->draft[length - 1] == ' ')
            --length;
        if (app->draft + length <= start)
            return;
        struct task *task = &app->tasks[app->task_count++];
        *task = (struct task){0};
        memcpy(task->text, start, (size_t)(app->draft + length - start));
        app->save_failed = !save_tasks(app);
        if (app->save_failed)
            --app->task_count;
        else {
            app->task_page = (app->task_count - 1) / TASKS_PER_PAGE;
            screen(app, TODO);
        }
    } else if (code >= ' ' && length < TASK_LENGTH) {
        app->draft[length] = (char)code;
        app->draft[length + 1] = '\0';
    }
}

static void activate(struct app *app, int code, int64_t now, const struct tm *today)
{
    switch (app->screen) {
    case HOME:
        app->home_focus = app->focus;
        screen(app, (enum screen)code);
        if (code == CALENDAR)
            set_today(app, today);
        break;
    case STOPWATCH:
        if (code == 0) {
            app->elapsed = elapsed(app, now);
            app->running = !app->running;
            app->started = now;
        } else if (code == 1 && app->running) {
            memmove(app->laps + 1, app->laps, (LAP_LIMIT - 1) * sizeof app->laps[0]);
            app->laps[0] = elapsed(app, now);
            ++app->lap_count;
        } else if (code == 2) {
            app->running = false;
            app->elapsed = app->lap_count = 0;
        }
        break;
    case CALENDAR:
        if (code)
            move_month(app, code);
        else
            set_today(app, today);
        break;
    case WORLD_CLOCK:
        if (code == WC_CITY_COUNT) {
            /* The footer button: open the map on the city the list was
             * on (wc_selected tracks the focused city; LAX at first). */
            int pos = wc_map_position(&WC_CITIES[app->wc_selected]);
            screen(app, WC_MAP);
            app->wc_map = pos;
            break;
        }
        /* code is the city index: open its detail view. */
        app->wc_selected = code;
        screen(app, WC_DETAIL);
        break;
    case WC_DETAIL:
        break;
    case WC_MAP:
        break;
    case TODO:
        if (code == -1) {
            app->draft[0] = '\0';
            app->symbols = false;
            app->save_failed = false;
            screen(app, EDIT_TASK);
        } else if (code < 0) {
            app->task_page += code == -2 ? -1 : 1;
            screen(app, TODO);
        } else {
            app->task_index = code;
            screen(app, TASK_ACTIONS);
        }
        break;
    case TASK_ACTIONS: task_action(app, code); break;
    case EDIT_TASK: edit_task(app, code); break;
    case CALCULATOR: calculate(app, code); break;
    }
}

static int hit(const struct layout *layout, int x, int y)
{
    for (int i = 0; i < layout->count; ++i) {
        const struct button *item = &layout->items[i];
        if (x >= item->x && x < item->x + item->w && y >= item->y && y < item->y + item->h)
            return i;
    }
    return -1;
}

/* World Clock input: focus is the city index (0..15), or WC_CITY_COUNT
 * for the World map footer button, and the 5-row window scrolls under
 * it. wc_selected tracks the focused city so the map picker opens on it. */
static void world_clock_input(struct app *app, const char *message, int64_t now)
{
    struct layout layout;
    make_layout(app, &layout);
    char phase;
    int a, b;
    if (sscanf(message, "w %d", &a) == 1) {
        if (a < -200 || a > 200)
            return;
        app->wheel += a;
        int step = app->wheel / WHEEL_STEP;
        app->wheel %= WHEEL_STEP;
        app->focus -= step;
        if (app->focus < 0)
            app->focus = 0;
        if (app->focus > WC_CITY_COUNT)
            app->focus = WC_CITY_COUNT;
        if (app->focus < WC_CITY_COUNT)
            app->wc_selected = app->focus;
        if (app->focus < app->wc_top)
            app->wc_top = app->focus;
        if (app->focus >= app->wc_top + WC_ROWS)
            app->wc_top = app->focus - WC_ROWS + 1;
    } else if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
        int index = hit(&layout, a, b);
        if (phase == 'd')
            app->touch_button = index;
        if (phase == 'u') {
            bool pressed = index >= 0 && index == app->touch_button;
            app->touch_button = -1;
            if (pressed) {
                app->focus = layout.items[index].code;
                if (app->focus < WC_CITY_COUNT)
                    app->wc_selected = app->focus;
                activate(app, app->focus, now, NULL);
            }
        }
    } else if (sscanf(message, "k %c %d", &phase, &a) == 2 && phase == 'u') {
        if (a == CENTRE_KEY)
            activate(app, app->focus, now, NULL);
        else if (a == RETURN_KEY)
            screen(app, HOME);
    }
}

/* World map picker input: the wheel steps the pin west to east through
 * the cities in longitude order (wc_map is a WC_MAP_ORDER position).
 * A tap on the map moves the pin to the nearest city, a tap on the GMT
 * strip below it picks by longitude alone, and a tap on the info panel
 * (or Centre) opens that city's detail view. Return goes back to the
 * list, focused on the pin's city. */
static int map_lon(int x)
{
    return (x - WC_MAP_X) * 360 / WC_MAP_W - 180;
}

static double map_lat(int y)
{
    return WC_MAP_LAT_TOP - (double)(y - WC_MAP_Y) * WC_MAP_LAT_SPAN / WC_MAP_H;
}

static void world_clock_map_input(struct app *app, const char *message, int64_t now)
{
    char phase;
    int a, b;
    if (sscanf(message, "w %d", &a) == 1) {
        if (a < -200 || a > 200)
            return;
        app->wheel += a;
        int step = app->wheel / WHEEL_STEP;
        app->wheel %= WHEEL_STEP;
        app->wc_map -= step;
        if (app->wc_map < 0)
            app->wc_map = 0;
        if (app->wc_map >= WC_MAP_COUNT)
            app->wc_map = WC_MAP_COUNT - 1;
    } else if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
        /* Touch regions: 0 map, 1 GMT strip, 2 info panel, -1 elsewhere. */
        int region = -1;
        if (a >= WC_MAP_X && a < WC_MAP_X + WC_MAP_W &&
            b >= WC_MAP_Y && b < WC_MAP_Y + WC_MAP_H)
            region = 0;
        else if (a >= WC_MAP_X && a < WC_MAP_X + WC_MAP_W &&
                 b >= WC_MAP_STRIP_Y && b < WC_MAP_INFO_Y)
            region = 1;
        else if (b >= WC_MAP_INFO_Y && b < WC_MAP_HINT_Y)
            region = 2;
        if (phase == 'd') {
            app->touch_button = region;
            return;
        }
        if (phase != 'u')
            return;
        bool pressed = region >= 0 && region == app->touch_button;
        app->touch_button = -1;
        if (!pressed)
            return;
        if (region == 0)
            app->wc_map = wc_map_position(wc_nearest_city(map_lon(a), map_lat(b)));
        else if (region == 1)
            app->wc_map = wc_map_position(wc_nearest_longitude(map_lon(a)));
        else {
            app->wc_selected = WC_MAP_ORDER[app->wc_map];
            screen(app, WC_DETAIL);
        }
    } else if (sscanf(message, "k %c %d", &phase, &a) == 2 && phase == 'u') {
        if (a == CENTRE_KEY) {
            app->wc_selected = WC_MAP_ORDER[app->wc_map];
            screen(app, WC_DETAIL);
        } else if (a == RETURN_KEY) {
            int city = WC_MAP_ORDER[app->wc_map];
            screen(app, WORLD_CLOCK);
            app->focus = city;
            app->wc_selected = city;
            app->wc_top = city >= WC_ROWS ? city - WC_ROWS + 1 : 0;
        }
    }
    (void)now;
}

void app_input(struct app *app, const char *message, int64_t now, const struct tm *today)
{
    if (app->screen == WORLD_CLOCK) {
        world_clock_input(app, message, now);
        return;
    }
    if (app->screen == WC_MAP) {
        world_clock_map_input(app, message, now);
        return;
    }
    struct layout layout;
    make_layout(app, &layout);
    char phase;
    int a, b;
    if (sscanf(message, "w %d", &a) == 1 && layout.count) {
        if (a < -200 || a > 200)
            return;
        app->wheel += a;
        int step = app->wheel / WHEEL_STEP;
        app->wheel %= WHEEL_STEP;
        app->focus = (app->focus - step % layout.count + layout.count) % layout.count;
    } else if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
        int index = hit(&layout, a, b);
        if (phase == 'd')
            app->touch_button = index;
        if (phase == 'u') {
            bool pressed = index >= 0 && index == app->touch_button;
            app->touch_button = -1;
            if (pressed) {
                app->focus = index;
                activate(app, layout.items[index].code, now, today);
            }
        }
    } else if (sscanf(message, "k %c %d", &phase, &a) == 2 && phase == 'u') {
        if (a == CENTRE_KEY && app->focus < layout.count)
            activate(app, layout.items[app->focus].code, now, today);
        if (a == RETURN_KEY) {
            if (app->screen == HOME)
                app->quit = true;
            else if (app->screen == WC_DETAIL) {
                /* Back to the list, focused on the city just viewed. */
                screen(app, WORLD_CLOCK);
                app->focus = app->wc_selected;
                app->wc_top = app->wc_selected >= WC_ROWS
                                  ? app->wc_selected - WC_ROWS + 1
                                  : 0;
            } else if (app->screen == EDIT_TASK || app->screen == TASK_ACTIONS)
                screen(app, TODO);
            else
                screen(app, HOME);
        }
    }
}

#include "font.h"

static void fill(uint32_t *pixels, unsigned stride, int x, int y, int w, int h, uint32_t color)
{
    /* Logical (x, y) is (HEIGHT - 1 - y, x) in the Q2 portrait buffer. */
    int left = HEIGHT - y - h;
    int right = HEIGHT - y;
    for (int column = x; column < x + w; ++column) {
        uint32_t *row = pixels + column * stride;
        for (int px = left; px < right; ++px)
            row[px] = 0xff000000u | color;
    }
}

static int glyph(unsigned char c)
{
    return c >= 32 && c <= 126 ? c - 32 : '?' - 32;
}

static int text_width(const char *text)
{
    int width = 0;
    for (; *text; ++text)
        width += f20_adv[glyph((unsigned char)*text)];
    return width;
}

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned alpha)
{
    unsigned rb = ((bg & 0xff00ff) * (255 - alpha) + (fg & 0xff00ff) * alpha) >> 8;
    unsigned g = ((bg & 0x00ff00) * (255 - alpha) + (fg & 0x00ff00) * alpha) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

static void draw_text_fitted(uint32_t *pixels, unsigned stride, int x, int y, int max_width,
                             const char *text, uint32_t color)
{
    bool narrow = text_width(text) > max_width;
    for (; *text; ++text) {
        int index = glyph((unsigned char)*text);
        int w = f20_adv[index];
        int advance = narrow ? w * 2 / 3 : w;
        if (x + advance > WIDTH || y + F20_H > HEIGHT)
            break;
        const unsigned char *bitmap = f20_px + f20_off[index];
        for (int r = 0; r < F20_H; ++r) {
            for (int c = 0; c < advance; ++c) {
                unsigned alpha = bitmap[r * w + c * w / advance];
                if (alpha) {
                    uint32_t *pixel = &pixels[(x + c) * stride + (HEIGHT - 1 - y - r)];
                    *pixel = 0xff000000u | (alpha == 255 ? color : blend(*pixel, color, alpha));
                }
            }
        }
        x += advance;
    }
}

static void draw_text(uint32_t *pixels, unsigned stride, int x, int y, const char *text, uint32_t color)
{
    draw_text_fitted(pixels, stride, x, y, WIDTH - x, text, color);
}

static void centered(uint32_t *pixels, unsigned stride, int x, int y, int w, const char *text, uint32_t color)
{
    int offset = (w - text_width(text)) / 2;
    draw_text(pixels, stride, x + (offset > 0 ? offset : 0), y, text, color);
}

/* --- q2-pod list painters (bar gradient, selection, chevron) --- */

static uint32_t lerp_channel(int a, int b, int num, int den)
{
    /* Signed: most stops darken, so (b - a) is negative and must not wrap. */
    return (uint32_t)(a + (b - a) * num / den);
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int num, int den)
{
    return (lerp_channel(a >> 16 & 255, b >> 16 & 255, num, den) << 16) |
           (lerp_channel(a >> 8 & 255, b >> 8 & 255, num, den) << 8) |
           lerp_channel(a & 255, b & 255, num, den);
}

/* Vertical gradient through the scheme's stops (positions 0-100). */
static void vgrad(uint32_t *pixels, unsigned stride, int x, int y, int w, int h,
                  const Q2PodStop *stops, int count)
{
    for (int yy = 0; yy < h; ++yy) {
        int t = h <= 1 ? 0 : yy * 100 / (h - 1);
        uint32_t color = stops[count - 1].rgb;
        for (int i = 0; i + 1 < count; ++i) {
            if (t >= stops[i].pos && t <= stops[i + 1].pos) {
                int den = stops[i + 1].pos - stops[i].pos;
                color = den ? lerp_color(stops[i].rgb, stops[i + 1].rgb,
                                         t - stops[i].pos, den) : stops[i].rgb;
                break;
            }
        }
        fill(pixels, stride, x, y + yy, w, 1, color);
    }
}

static void outline(uint32_t *pixels, unsigned stride, int x, int y, int w, int h, uint32_t color)
{
    fill(pixels, stride, x, y, w, 1, color);
    fill(pixels, stride, x, y + h - 1, w, 1, color);
    fill(pixels, stride, x, y, 1, h, color);
    fill(pixels, stride, x + w - 1, y, 1, h, color);
}

/* Text colour on a selection: the scheme's sel_text, or the row's own ink
 * when the painter keeps it (Minimal's card). */
static uint32_t sel_text_color(void)
{
    return SCHEME.sel_text ? SCHEME.sel_text : INK;
}

/* Paint the theme's selection over a rectangle: Classic's gloss band,
 * Modern's flat accent bar, Minimal's card fill + outline. */
static void paint_selection(uint32_t *pixels, unsigned stride, int x, int y, int w, int h)
{
    if (SCHEME.painter == Q2POD_SEL_STOPS) {
        vgrad(pixels, stride, x, y, w, h, SCHEME.sel, Q2POD_GLOSS_STOPS);
        fill(pixels, stride, x, y, w, 1, SCHEME.sel_top);
        fill(pixels, stride, x, y + h - 1, w, 1, SCHEME.sel_edge);
    } else if (SCHEME.painter == Q2POD_SEL_BAR) {
        fill(pixels, stride, x, y, w, h, SCHEME.accent_top);
    } else {
        fill(pixels, stride, x, y, w, h, SCHEME.fill);
        uint32_t edge = SCHEME.paper == 0 ? 0xeeeeecu : 0x424242u;
        outline(pixels, stride, x, y, w, h, edge);
        outline(pixels, stride, x + 1, y + 1, w - 2, h - 2, edge);
    }
}

/* Paint one list row: the selection above when focused, otherwise the
 * theme's unselected row (Classic hairline, Minimal card, Modern flat). */
static void paint_list_row(uint32_t *pixels, unsigned stride, const struct button *item, bool selected)
{
    if (selected) {
        if (SCHEME.painter == Q2POD_SEL_CARD) {
            /* Minimal's card: inset rounded look, accent dot, 2 px outline. */
            int cx = 14, cw = WIDTH - 28;
            fill(pixels, stride, cx, item->y + 3, cw, item->h - 6, SCHEME.fill);
            uint32_t edge = SCHEME.paper == 0 ? 0xeeeeecu : 0x424242u;
            outline(pixels, stride, cx, item->y + 3, cw, item->h - 6, edge);
            outline(pixels, stride, cx + 1, item->y + 4, cw - 2, item->h - 8, edge);
            fill(pixels, stride, 24, item->y + item->h / 2 - 4, 8, 8, edge);
        } else {
            paint_selection(pixels, stride, item->x, item->y, item->w, item->h);
        }
        return;
    }
    if (SCHEME.painter == Q2POD_SEL_STOPS) {
        uint32_t hair = SCHEME.paper == 0 ? 0x333333u : 0xe2e3e2u;
        fill(pixels, stride, item->x, item->y + item->h - 1, item->w, 1, hair);
    } else if (SCHEME.painter == Q2POD_SEL_CARD) {
        int cx = 14, cw = WIDTH - 28;
        fill(pixels, stride, cx, item->y + 3, cw, item->h - 6, SCHEME.fill);
        outline(pixels, stride, cx, item->y + 3, cw, item->h - 6,
                SCHEME.paper == 0 ? 0x444444u : 0xe8e8e8u);
    }
}

/* Upper-case a title in place (Minimal's bar and rows are small capitals). */
static void upper(char *text)
{
    for (; *text; ++text)
        if (*text >= 'a' && *text <= 'z')
            *text -= 'a' - 'A';
}

static void draw_calendar(const struct app *app, uint32_t *pixels, unsigned stride, const struct tm *today)
{
    static const char *const weekdays[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    char label[32];
    for (int i = 0; i < 7; ++i)
        centered(pixels, stride, 19 + i * 48, 44, 48, weekdays[i], MUTED);
    int first = first_weekday(app->year, app->month);
    int days = days_in_month(app->year, app->month);
    bool current_month = app->year == today->tm_year + 1900 && app->month == today->tm_mon;
    for (int day = 1; day <= days; ++day) {
        int cell = first + day - 1;
        int x = 19 + cell % 7 * 48, y = 68 + cell / 7 * 24;
        bool current = current_month && day == today->tm_mday;
        if (current)
            paint_selection(pixels, stride, x + 9, y, 30, 24);
        snprintf(label, sizeof label, "%d", day);
        centered(pixels, stride, x, y, 48, label, current ? sel_text_color() : INK);
    }
}

/* --- World Clock drawing (GNOME Clocks two-line rows, Casio codes) --- */

static void format_offset(char *text, size_t size, int minutes)
{
    int sign = minutes < 0 ? '-' : '+';
    int abs_min = minutes < 0 ? -minutes : minutes;
    if (abs_min % 60)
        snprintf(text, size, "UTC%c%d:%02d", sign, abs_min / 60, abs_min % 60);
    else
        snprintf(text, size, "UTC%c%d", sign, abs_min / 60);
}

static void draw_world_clock_row(struct app *app, uint32_t *pixels, unsigned stride,
                                 const struct button *item, int64_t utc,
                                 const struct tm *today)
{
    const struct wc_city *city = &WC_CITIES[item->code];
    bool selected = item->code == app->focus;
    paint_list_row(pixels, stride, item, selected);
    int year, month, mday, hour, min, sec, wday;
    wc_local_time(city, utc, &year, &month, &mday, &hour, &min, &sec, &wday);
    static const char *const wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char left[48], time[8], sub[64], offset[16];
    snprintf(left, sizeof left, "%s  %s", city->code, city->name);
    snprintf(time, sizeof time, "%02d:%02d", hour, min);
    format_offset(offset, sizeof offset, wc_offset_minutes(city, utc));
    int64_t city_day = wc_days_from_civil(year, (unsigned)month, (unsigned)mday);
    int64_t home_day = wc_days_from_civil(today->tm_year + 1900, (unsigned)today->tm_mon + 1,
                                          (unsigned)today->tm_mday);
    const char *roll = city_day > home_day ? " +1d" : city_day < home_day ? " -1d" : "";
    snprintf(sub, sizeof sub, "%s %d %s | %s%s | %s%s", wd[wday], mday, mo[month - 1],
             offset, wc_is_dst(city, utc) ? " DST" : "", hour >= 6 && hour < 18 ? "Day" : "Night",
             roll);
    int y1 = item->y + 5, y2 = item->y + 27;
    char label[48];
    snprintf(label, sizeof label, "%s", left);
    if (SCHEME.caps_rows)
        upper(label);
    draw_text_fitted(pixels, stride, SCHEME.text_x, y1, 210, label,
                     selected ? sel_text_color() : INK);
    /* Time ends at x=312 so the chevron at x=322 never overlaps it. */
    draw_text(pixels, stride, 312 - text_width(time), y1, time,
              selected ? sel_text_color() : ACCENT);
    draw_text_fitted(pixels, stride, SCHEME.text_x, y2, WIDTH - SCHEME.text_x - 20, sub,
                     selected ? sel_text_color() : MUTED);
    if (selected)
        draw_text(pixels, stride, 322, item->y + (WC_ROW_H - F20_H) / 2, ">",
                  SCHEME.painter == Q2POD_SEL_CARD ? SCHEME.chevron : sel_text_color());
}

/* --- Tiny world map picker (WC_MAP) ---
 * The map is drawn from coarse lon/lat continent polygons, filled by a
 * scanline fill in the map's equirectangular projection (1 degree is
 * about 1 pixel). No image files are loaded: the Q2 ships no map art
 * and the card app has no assets. The Q2 is a colour screen, so each
 * landmass gets its own soft atlas colour — one fixed palette, picked
 * mid-tone so it reads on both the light and the dark paper — instead
 * of the theme's ink. Vertical lines mark the 24 one-hour GMT bands,
 * the pin city's band is tinted with the theme accent, and the info
 * panel below reads like Ubuntu's Date & Time tooltip: city and
 * country, abbreviation and UTC offset with the local time, then the
 * date and the difference from Los Angeles. */
static const short POLY_NORTH_AMERICA[][2] = {
    {-168, 66}, {-160, 58}, {-153, 58}, {-145, 60}, {-137, 58}, {-130, 54},
    {-125, 49}, {-124, 40}, {-121, 34}, {-117, 33}, {-114, 29}, {-110, 23},
    {-105, 20}, {-101, 18}, {-97, 16}, {-94, 16}, {-91, 14}, {-87, 13},
    {-83, 9}, {-77, 8}, {-80, 10}, {-83, 11}, {-86, 12}, {-88, 15},
    {-88, 17}, {-90, 21}, {-94, 18}, {-97, 19}, {-97, 22}, {-97, 26},
    {-93, 30}, {-89, 30}, {-84, 30}, {-81, 26}, {-80, 32}, {-76, 35},
    {-74, 40}, {-70, 42}, {-66, 45}, {-60, 47}, {-55, 52}, {-59, 55},
    {-64, 58}, {-68, 58}, {-73, 62}, {-78, 62}, {-85, 66}, {-92, 66},
    {-95, 69}, {-105, 68}, {-115, 69}, {-125, 70}, {-137, 70}, {-150, 71},
    {-162, 71},
};
static const short POLY_HAWAII[][2] = {
    {-155, 19}, {-156, 20}, {-159, 21}, {-160, 22}, {-156, 19},
};
static const short POLY_GREENLAND[][2] = {
    {-58, 64}, {-52, 60}, {-44, 60}, {-40, 64}, {-32, 68}, {-25, 70},
    {-20, 73}, {-22, 76}, {-30, 78}, {-45, 80}, {-58, 78}, {-62, 74},
    {-60, 70},
};
static const short POLY_SOUTH_AMERICA[][2] = {
    {-79, 9}, {-75, 11}, {-71, 12}, {-64, 10}, {-60, 8}, {-53, 5},
    {-50, 0}, {-44, -3}, {-37, -6}, {-35, -9}, {-39, -13}, {-40, -20},
    {-41, -23}, {-48, -27}, {-52, -32}, {-57, -38}, {-62, -41}, {-65, -45},
    {-68, -50}, {-70, -54}, {-73, -50}, {-73, -44}, {-73, -37}, {-72, -30},
    {-71, -22}, {-70, -18}, {-75, -15}, {-79, -8}, {-81, -4}, {-80, 0},
    {-80, 4},
};
static const short POLY_EUROPE[][2] = {
    {-10, 36}, {-9, 43}, {-2, 48}, {0, 50}, {3, 53}, {8, 57}, {10, 59},
    {18, 56}, {21, 55}, {24, 59}, {28, 60}, {30, 62}, {30, 66}, {27, 70},
    {22, 70}, {18, 69}, {13, 66}, {11, 62}, {6, 58}, {5, 55}, {8, 54},
    {10, 54}, {12, 51}, {10, 48}, {8, 45}, {7, 44}, {3, 42}, {0, 40},
    {-2, 37}, {-6, 36},
};
static const short POLY_BRITAIN[][2] = {
    {-5, 50}, {-3, 53}, {-5, 56}, {-3, 58}, {-1, 57}, {0, 53}, {1, 51},
    {-3, 50},
};
static const short POLY_AFRICA[][2] = {
    {-6, 35}, {-10, 31}, {-15, 27}, {-17, 21}, {-17, 15}, {-15, 11},
    {-12, 8}, {-8, 5}, {-4, 5}, {0, 6}, {6, 4}, {9, 4}, {9, -1},
    {12, -5}, {13, -10}, {12, -17}, {14, -22}, {16, -28}, {18, -34},
    {20, -35}, {25, -34}, {28, -33}, {32, -29}, {35, -24}, {36, -18},
    {40, -15}, {40, -10}, {41, -4}, {42, 0}, {46, 2}, {51, 11}, {48, 11},
    {44, 13}, {41, 16}, {39, 21}, {37, 25}, {35, 28}, {33, 31}, {30, 31},
    {25, 32}, {20, 32}, {15, 33}, {11, 34}, {10, 37}, {5, 37}, {0, 36},
};
static const short POLY_ASIA[][2] = {
    {28, 36}, {30, 36}, {33, 36}, {36, 36}, {39, 38}, {42, 40}, {44, 41},
    {48, 42}, {50, 44}, {52, 46}, {55, 47}, {58, 49}, {60, 51}, {63, 53},
    {66, 55}, {68, 58}, {70, 60}, {72, 63}, {74, 66}, {76, 69}, {78, 71},
    {82, 73}, {86, 74}, {90, 75}, {95, 76}, {100, 77}, {105, 77},
    {110, 76}, {115, 74}, {120, 73}, {126, 72}, {132, 72}, {138, 72},
    {144, 71}, {150, 70}, {156, 69}, {162, 68}, {168, 67}, {174, 66},
    {178, 65}, {178, 64}, {170, 60}, {164, 60}, {160, 58}, {158, 53},
    {156, 51}, {150, 59}, {146, 59}, {142, 55}, {141, 50}, {140, 45},
    {138, 42}, {135, 40}, {132, 38}, {129, 35}, {127, 34}, {124, 32},
    {122, 30}, {120, 27}, {117, 23}, {113, 22}, {110, 20}, {108, 18},
    {106, 10}, {104, 8}, {101, 7}, {99, 10}, {98, 14}, {96, 16}, {94, 18},
    {92, 21}, {90, 22}, {88, 22}, {86, 20}, {84, 19}, {82, 16}, {80, 13},
    {78, 8}, {76, 9}, {74, 14}, {72, 20}, {70, 23}, {67, 25}, {64, 25},
    {61, 25}, {58, 26}, {59, 23}, {57, 19}, {53, 16}, {48, 14}, {44, 13},
    {43, 16}, {41, 20}, {39, 24}, {38, 28}, {39, 31}, {36, 36}, {33, 36},
    {30, 36},
};
static const short POLY_JAPAN[][2] = {
    {130, 31}, {131, 33}, {133, 34}, {136, 35}, {138, 35}, {140, 36},
    {141, 38}, {141, 41}, {142, 44}, {144, 44}, {143, 42}, {141, 39},
    {139, 37}, {137, 35}, {134, 34}, {131, 32},
};
static const short POLY_SUMATRA[][2] = {
    {95, 5}, {100, 0}, {104, -3}, {106, -6}, {103, -5}, {99, 0},
};
static const short POLY_BORNEO[][2] = {
    {109, 1}, {113, 4}, {117, 6}, {119, 1}, {117, -3}, {113, -4}, {110, -2},
};
static const short POLY_NEW_GUINEA[][2] = {
    {131, -1}, {136, -2}, {141, -3}, {146, -6}, {150, -8}, {147, -9},
    {143, -8}, {138, -7}, {134, -4},
};
static const short POLY_AUSTRALIA[][2] = {
    {114, -22}, {114, -30}, {115, -34}, {118, -35}, {122, -34}, {126, -32},
    {129, -32}, {132, -32}, {135, -35}, {138, -35}, {140, -38}, {144, -38},
    {147, -39}, {150, -37}, {153, -32}, {153, -27}, {151, -24}, {149, -20},
    {146, -19}, {143, -15}, {142, -11}, {140, -12}, {137, -12}, {135, -15},
    {132, -12}, {130, -13}, {127, -14}, {124, -16}, {121, -18}, {118, -20},
    {115, -21},
};
static const short POLY_NEW_ZEALAND[][2] = {
    {167, -45}, {169, -43}, {171, -42}, {173, -41}, {175, -38}, {177, -38},
    {178, -37}, {176, -39}, {174, -41}, {172, -43}, {169, -46},
};

static int map_px(double lon)
{
    return WC_MAP_X + (int)((lon + 180.0) * WC_MAP_W / 360.0);
}

static int map_py(double lat)
{
    return WC_MAP_Y + (int)((WC_MAP_LAT_TOP - lat) * WC_MAP_H / WC_MAP_LAT_SPAN);
}

static void fill_poly(uint32_t *pixels, unsigned stride, const short (*pts)[2], int n, uint32_t color)
{
    for (int py = 0; py < WC_MAP_H; ++py) {
        double lat = WC_MAP_LAT_TOP - (py + 0.5) * WC_MAP_LAT_SPAN / WC_MAP_H;
        double xs[32];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            double lat1 = pts[i][1], lat2 = pts[j][1];
            if ((lat1 <= lat && lat < lat2) || (lat2 <= lat && lat < lat1)) {
                double lon = pts[i][0] + (lat - lat1) * (pts[j][0] - pts[i][0]) / (lat2 - lat1);
                if (m < 32)
                    xs[m++] = lon;
            }
        }
        for (int i = 1; i < m; ++i) {
            double v = xs[i];
            int k = i - 1;
            while (k >= 0 && xs[k] > v) {
                xs[k + 1] = xs[k];
                --k;
            }
            xs[k + 1] = v;
        }
        for (int k = 0; k + 1 < m; k += 2) {
            int xa = map_px(xs[k]), xb = map_px(xs[k + 1]);
            if (xa < WC_MAP_X)
                xa = WC_MAP_X;
            if (xb > WC_MAP_X + WC_MAP_W)
                xb = WC_MAP_X + WC_MAP_W;
            if (xb > xa)
                fill(pixels, stride, xa, WC_MAP_Y + py, xb - xa, 1, color);
        }
    }
}

static void disc(uint32_t *pixels, unsigned stride, int cx, int cy, int r, uint32_t color)
{
    /* Integer disc: widen each row until the next pixel would leave it. */
    for (int dy = -r; dy <= r; ++dy) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
            ++dx;
        fill(pixels, stride, cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

static void draw_world_clock_map(struct app *app, uint32_t *pixels, unsigned stride,
                                 int64_t utc, const struct tm *today)
{
    (void)today;
    const struct wc_city *city = &WC_CITIES[WC_MAP_ORDER[app->wc_map]];
    uint32_t grid = blend(BG, MUTED, 64);
    /* The pin city's GMT band: one hour is 15 degrees of longitude,
     * tinted with the theme accent so it follows the accent colour. */
    int band = (int)((city->lon + 180.0) / 15.0);
    if (band < 0)
        band = 0;
    if (band > 23)
        band = 23;
    fill(pixels, stride, WC_MAP_X + band * WC_MAP_W / 24, WC_MAP_Y,
         WC_MAP_W / 24 + 1, WC_MAP_INFO_Y - WC_MAP_Y, blend(BG, ACCENT, 48));
    /* GMT grid: a line every hour of longitude, one every 30 degrees
     * of latitude. */
    for (int k = 0; k <= 24; ++k)
        fill(pixels, stride, WC_MAP_X + k * WC_MAP_W / 24, WC_MAP_Y, 1, WC_MAP_H, grid);
    static const int lats[] = {60, 30, 0, -30};
    for (unsigned i = 0; i < sizeof lats / sizeof lats[0]; ++i)
        fill(pixels, stride, WC_MAP_X, map_py(lats[i]), WC_MAP_W, 1, grid);
    /* Land in soft atlas colours, one per landmass: teal North America,
     * ice-blue Greenland, leaf-green South America, lavender Europe,
     * sandy Africa, rose Asia, tropical green Indonesia / New Guinea,
     * and ochre Australia / New Zealand. Mid-tone, so the same palette
     * reads on the light paper and the dark paper. */
    enum {
        MAP_NORTH_AMERICA = 0x63b3a5, MAP_GREENLAND = 0x93c4de,
        MAP_SOUTH_AMERICA = 0x8fc06a, MAP_EUROPE = 0xa58bd0,
        MAP_AFRICA = 0xd89a4e, MAP_ASIA = 0xd97f7f, MAP_TROPICAL = 0x66bb8a,
        MAP_OCEANIA = 0xcfa53b
    };
    static const struct { const short (*pts)[2]; int n; uint32_t color; } polys[] = {
        {POLY_NORTH_AMERICA, (int)(sizeof POLY_NORTH_AMERICA / sizeof POLY_NORTH_AMERICA[0]), MAP_NORTH_AMERICA},
        {POLY_HAWAII, (int)(sizeof POLY_HAWAII / sizeof POLY_HAWAII[0]), MAP_NORTH_AMERICA},
        {POLY_GREENLAND, (int)(sizeof POLY_GREENLAND / sizeof POLY_GREENLAND[0]), MAP_GREENLAND},
        {POLY_SOUTH_AMERICA, (int)(sizeof POLY_SOUTH_AMERICA / sizeof POLY_SOUTH_AMERICA[0]), MAP_SOUTH_AMERICA},
        {POLY_EUROPE, (int)(sizeof POLY_EUROPE / sizeof POLY_EUROPE[0]), MAP_EUROPE},
        {POLY_BRITAIN, (int)(sizeof POLY_BRITAIN / sizeof POLY_BRITAIN[0]), MAP_EUROPE},
        {POLY_AFRICA, (int)(sizeof POLY_AFRICA / sizeof POLY_AFRICA[0]), MAP_AFRICA},
        {POLY_ASIA, (int)(sizeof POLY_ASIA / sizeof POLY_ASIA[0]), MAP_ASIA},
        {POLY_JAPAN, (int)(sizeof POLY_JAPAN / sizeof POLY_JAPAN[0]), MAP_ASIA},
        {POLY_SUMATRA, (int)(sizeof POLY_SUMATRA / sizeof POLY_SUMATRA[0]), MAP_TROPICAL},
        {POLY_BORNEO, (int)(sizeof POLY_BORNEO / sizeof POLY_BORNEO[0]), MAP_TROPICAL},
        {POLY_NEW_GUINEA, (int)(sizeof POLY_NEW_GUINEA / sizeof POLY_NEW_GUINEA[0]), MAP_TROPICAL},
        {POLY_AUSTRALIA, (int)(sizeof POLY_AUSTRALIA / sizeof POLY_AUSTRALIA[0]), MAP_OCEANIA},
        {POLY_NEW_ZEALAND, (int)(sizeof POLY_NEW_ZEALAND / sizeof POLY_NEW_ZEALAND[0]), MAP_OCEANIA},
    };
    for (unsigned i = 0; i < sizeof polys / sizeof polys[0]; ++i)
        fill_poly(pixels, stride, polys[i].pts, polys[i].n, polys[i].color);
    outline(pixels, stride, WC_MAP_X - 1, WC_MAP_Y - 1, WC_MAP_W + 2, WC_MAP_H + 2, grid);
    for (int i = 0; i < WC_CITY_COUNT; ++i)
        fill(pixels, stride, map_px(WC_CITIES[i].lon) - 1, map_py(WC_CITIES[i].lat) - 1,
             3, 3, INK);
    disc(pixels, stride, map_px(city->lon), map_py(city->lat), 6, BG);
    disc(pixels, stride, map_px(city->lon), map_py(city->lat), 5, ACCENT);
    disc(pixels, stride, map_px(city->lon), map_py(city->lat), 2, BG);
    /* GMT strip: a tick every hour, labelled every four hours. */
    for (int k = 0; k <= 24; ++k)
        fill(pixels, stride, WC_MAP_X + k * WC_MAP_W / 24, WC_MAP_STRIP_Y, 1, 6, MUTED);
    static const int strip_lon[] = {-180, -120, -60, 0, 60, 120, 180};
    static const char *const strip_label[] = {"-12", "-8", "-4", "0", "+4", "+8", "+12"};
    for (unsigned i = 0; i < sizeof strip_lon / sizeof strip_lon[0]; ++i) {
        int cx = map_px(strip_lon[i]);
        int w = text_width(strip_label[i]);
        int x = i == 0 ? WC_MAP_X : i + 1 == sizeof strip_lon / sizeof strip_lon[0]
                             ? WC_MAP_X + WC_MAP_W - w
                             : cx - w / 2;
        draw_text(pixels, stride, x, WC_MAP_STRIP_Y + 6, strip_label[i], MUTED);
    }
    /* Info panel, Ubuntu tooltip style: city and country, abbreviation
     * and offset with the local time, date and difference from home. */
    int year, month, mday, hour, min, sec, wday;
    wc_local_time(city, utc, &year, &month, &mday, &hour, &min, &sec, &wday);
    static const char *const wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char offset[16], line[80];
    format_offset(offset, sizeof offset, wc_offset_minutes(city, utc));
    snprintf(line, sizeof line, "%s, %s", city->name, city->country);
    draw_text_fitted(pixels, stride, 18, WC_MAP_INFO_Y, WIDTH - 36, line, INK);
    const char *abbr = wc_is_dst(city, utc) && city->abbr_dst ? city->abbr_dst : city->abbr_std;
    snprintf(line, sizeof line, "%s (%s)   %02d:%02d", abbr, offset, hour, min);
    draw_text_fitted(pixels, stride, 18, WC_MAP_INFO_Y + 22, WIDTH - 36, line, ACCENT);
    int diff = wc_offset_minutes(city, utc) - wc_offset_minutes(&WC_CITIES[0], utc);
    char difftext[48];
    if (!diff)
        snprintf(difftext, sizeof difftext, "Same time as Los Angeles");
    else {
        int ad = diff < 0 ? -diff : diff;
        if (ad % 60)
            snprintf(difftext, sizeof difftext, "%dh %02dm %s Los Angeles", ad / 60, ad % 60,
                     diff > 0 ? "ahead of" : "behind");
        else
            snprintf(difftext, sizeof difftext, "%dh %s Los Angeles", ad / 60,
                     diff > 0 ? "ahead of" : "behind");
    }
    snprintf(line, sizeof line, "%s %d %s, %s", wd[wday], mday, mo[month - 1], difftext);
    draw_text_fitted(pixels, stride, 18, WC_MAP_INFO_Y + 44, WIDTH - 36, line, MUTED);
    centered(pixels, stride, 0, WC_MAP_HINT_Y, WIDTH, "Wheel moves pin, Centre opens", MUTED);
}

static void draw_world_clock_detail(struct app *app, uint32_t *pixels, unsigned stride,
                                    int64_t utc)
{
    const struct wc_city *city = &WC_CITIES[app->wc_selected];
    int year, month, mday, hour, min, sec, wday;
    wc_local_time(city, utc, &year, &month, &mday, &hour, &min, &sec, &wday);
    static const char *const wd[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};
    char value[64], offset[16];
    snprintf(value, sizeof value, "%02d:%02d:%02d", hour, min, sec);
    centered(pixels, stride, 18, 70, 339, value, ACCENT);
    snprintf(value, sizeof value, "%s, %d %s %d", wd[wday], mday, months[month - 1], year);
    centered(pixels, stride, 18, 112, 339, value, INK);
    format_offset(offset, sizeof offset, wc_offset_minutes(city, utc));
    snprintf(value, sizeof value, "%s | %s | %s", offset,
             wc_is_dst(city, utc) ? "Daylight saving" : "Standard time",
             hour >= 6 && hour < 18 ? "Day" : "Night");
    centered(pixels, stride, 18, 142, 339, value, MUTED);
    int diff = wc_offset_minutes(city, utc) - wc_offset_minutes(&WC_CITIES[0], utc);
    if (!diff)
        snprintf(value, sizeof value, "Same time as %s", WC_CITIES[0].name);
    else {
        int abs_diff = diff < 0 ? -diff : diff;
        if (abs_diff % 60)
            snprintf(value, sizeof value, "%dh %02dm %s %s", abs_diff / 60, abs_diff % 60,
                     diff > 0 ? "ahead of" : "behind", WC_CITIES[0].name);
        else
            snprintf(value, sizeof value, "%dh %s %s", abs_diff / 60,
                     diff > 0 ? "ahead of" : "behind", WC_CITIES[0].name);
    }
    centered(pixels, stride, 18, 172, 339, value, MUTED);
}

void app_draw(struct app *app, uint32_t *pixels, unsigned stride, int64_t now, time_t utc, const struct tm *today)
{
    struct layout layout;
    make_layout(app, &layout);
    fill(pixels, stride, 0, 0, WIDTH, HEIGHT, BG);
    /* q2-pod's 26 px status bar: the theme's gloss/flat bar, its bottom
     * line, and the centred screen title (the calendar's month + year).
     * There is no "< Back" label: the wheel's Return key already goes
     * back, so the bar keeps only the title. */
    vgrad(pixels, stride, 0, 0, WIDTH, Q2POD_BAR_H, SCHEME.bar, Q2POD_BAR_STOPS);
    if (SCHEME.bar_line2)
        fill(pixels, stride, 0, Q2POD_BAR_H - 2, WIDTH, 1, SCHEME.bar_line2);
    fill(pixels, stride, 0, Q2POD_BAR_H - 1, WIDTH, 1, SCHEME.bar_line);
    char title[32];
    if (app->screen == CALENDAR)
        snprintf(title, sizeof title, "%s %d", months[app->month], app->year);
    else if (app->screen == WC_DETAIL)
        snprintf(title, sizeof title, "%s", WC_CITIES[app->wc_selected].name);
    else
        snprintf(title, sizeof title, "%s", titles[app->screen]);
    if (SCHEME.caps_bar)
        upper(title);
    centered(pixels, stride, 0, 3, WIDTH, title, INK);
    char value[64];

    switch (app->screen) {
    case STOPWATCH:
        format_elapsed(value, sizeof value, elapsed(app, now));
        centered(pixels, stride, 18, 78, 339, value, ACCENT);
        for (unsigned i = 0; i < app->lap_count && i < LAP_LIMIT; ++i) {
            char time[32];
            format_elapsed(time, sizeof time, app->laps[i]);
            snprintf(value, sizeof value, "Lap %u    %s", app->lap_count - i, time);
            centered(pixels, stride, 18, 175 + 25 * (int)i, 339, value, MUTED);
        }
        break;
    case CALENDAR:
        draw_calendar(app, pixels, stride, today);
        break;
    case TODO:
        if (!app->task_count)
            draw_text(pixels, stride, 18, 44, "No tasks yet", MUTED);
        break;
    case TASK_ACTIONS:
        draw_text(pixels, stride, 20, 36, app->tasks[app->task_index].text, INK);
        break;
    case EDIT_TASK:
        fill(pixels, stride, 18, 36, 339, 30, PANEL);
        draw_text_fitted(pixels, stride, 28, 43, 319, app->draft[0] ? app->draft : "Task name", app->draft[0] ? INK : MUTED);
        break;
    case CALCULATOR:
        fill(pixels, stride, 18, 34, 339, 36, PANEL);
        if (app->op && !app->error) {
            snprintf(value, sizeof value, "%c", app->op);
            draw_text(pixels, stride, 30, 43, value, MUTED);
        }
        draw_text(pixels, stride, 345 - text_width(app->number), 43, app->number, INK);
        break;
    case WORLD_CLOCK:
        for (int i = 0; i < layout.count; ++i) {
            if (layout.items[i].code == WC_CITY_COUNT) {
                /* The World map footer button below the rows. */
                bool selected = app->focus == WC_CITY_COUNT;
                paint_list_row(pixels, stride, &layout.items[i], selected);
                char label[] = "World map";
                if (SCHEME.caps_rows)
                    upper(label);
                int y = layout.items[i].y + (layout.items[i].h - F20_H) / 2;
                draw_text_fitted(pixels, stride, SCHEME.text_x, y,
                                 WIDTH - SCHEME.text_x - 40, label,
                                 selected ? sel_text_color() : INK);
                if (selected)
                    draw_text(pixels, stride, 322, y, ">",
                              SCHEME.painter == Q2POD_SEL_CARD ? SCHEME.chevron
                                                               : sel_text_color());
            } else
                draw_world_clock_row(app, pixels, stride, &layout.items[i], utc, today);
        }
        break;
    case WC_MAP:
        draw_world_clock_map(app, pixels, stride, utc, today);
        break;
    case WC_DETAIL:
        draw_world_clock_detail(app, pixels, stride, utc);
        break;
    case HOME:
        break;
    }

    /* World Clock rows are drawn above; the generic list loop below
     * would draw them a second time. The map has no list rows. */
    if (app->screen == WORLD_CLOCK || app->screen == WC_MAP)
        return;

    for (int i = 0; i < layout.count; ++i) {
        const struct button *item = &layout.items[i];
        bool selected = i == app->focus;
        int y = item->y + (item->h - F20_H) / 2;
        bool list = app->screen == HOME || app->screen == TASK_ACTIONS ||
                    (app->screen == TODO && item->code >= 0);
        if (list) {
            /* System list rows: theme selection painter, theme text
             * edge, chevron on the selected row only. */
            paint_list_row(pixels, stride, item, selected);
            char label[32];
            snprintf(label, sizeof label, "%s", item->label);
            if (SCHEME.caps_rows)
                upper(label);
            uint32_t color = selected ? sel_text_color() : INK;
            draw_text_fitted(pixels, stride, SCHEME.text_x, y,
                             WIDTH - SCHEME.text_x - 40, label, color);
            if (selected)
                draw_text(pixels, stride, 322, y, ">",
                          SCHEME.painter == Q2POD_SEL_CARD ? SCHEME.chevron
                                                           : sel_text_color());
        } else {
            /* Action keys keep their buttons, painted with the theme's
             * selection when focused instead of a flat fill. */
            if (selected)
                paint_selection(pixels, stride, item->x, item->y, item->w, item->h);
            else
                fill(pixels, stride, item->x, item->y, item->w, item->h, PANEL);
            uint32_t color = selected ? sel_text_color() : INK;
            centered(pixels, stride, item->x, y, item->w, item->label, color);
        }
    }
}
