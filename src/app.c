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
static int config_number(const char *config, const char *section, const char *key,
                         int fallback, int limit, bool canonical) {
    bool in_section=false;
    while (*config) {
        const char *end=config;
        while (*end && *end!='\n') ++end;
        const char *p=config;
        while (p<end && (*p==' ' || *p=='\t')) ++p;
        if (p<end && *p=='[') {
            int i=0;
            while (p+i<end && section[i] && p[i]==section[i]) ++i;
            in_section=!section[i];
        } else if (in_section) {
            int i=0;
            while (p+i<end && key[i] && p[i]==key[i]) ++i;
            if (!key[i]) {
                p+=i;
                while (p<end && (*p==' ' || *p=='\t')) ++p;
                if (p<end && *p++=='=') {
                    while (p<end && (*p==' ' || *p=='\t')) ++p;
                    if (canonical && p<end && *p=='0') return fallback;
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
    int theme=config_number(config,"[IPOD]","THEME",0,3,false);
    const char *key=theme==0?"MINIMAL_ACCENT":theme==1?"CLASSIC_LIGHT_ACCENT":"ACCENT";
    int raw_accent=config_number(config,"[IPOD]",key,0,9,false);
    int accent=raw_accent;
    if(theme==1)
        accent=accent==0?4:accent==8?8:accent-1;
    else if(theme==2 && accent==7)
        accent=0;
    int mode=config_number(config,"[IPOD]","LIGHT",0,3,false);
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

enum { RETURN_KEY = 170, CENTRE_KEY = 218, WHEEL_FINE = 16,
       WHEEL_DEFAULT = 28 * WHEEL_FINE, WHEEL_IDLE_MS = 1000, TASKS_PER_PAGE = 5,
       WC_ROW_H = 52, WC_ROWS = 5 };

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
    "World Clock", "World Clock", "New task", "Task options"
};

static const char *const months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

void app_wheel_config(struct app *app, const char *config)
{
    int percent = config_number(config, "[Q2POD]", "WHEELSENSITIVITY", 50, 201, true);
    if (percent < 10 || percent % 10)
        percent = 50;
    int units = (WHEEL_DEFAULT * 50 + percent / 2) / percent;
    app->wheel = app->wheel * units / app->wheel_units;
    app->wheel_units = units;
}

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
    *app = (struct app){ .task_path = task_path, .touch_button = -1, .number = "0",
                         .wheel_units = WHEEL_DEFAULT };
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
         * visible at a time, the list scrolls under the focus. */
        for (int i = 0; i < WC_ROWS && app->wc_top + i < WC_CITY_COUNT; ++i)
            button(layout, 0, 26 + i * WC_ROW_H, WIDTH, WC_ROW_H,
                   app->wc_top + i, "");
        break;
    }
    case WC_DETAIL:
        /* Detail view has no buttons; Return goes back to the list. */
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
        /* code is the city index: open its detail view. */
        app->wc_selected = code;
        screen(app, WC_DETAIL);
        break;
    case WC_DETAIL:
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

static void move_focus(struct app *app, int travel, int count, int64_t now)
{
    if (!travel || !count)
        return;
    /* The card-app protocol sends signed travel, without wheel-lift events. */
    if (now - app->wheel_at >= WHEEL_IDLE_MS ||
        (app->wheel > 0 && travel < 0) || (app->wheel < 0 && travel > 0))
        app->wheel = 0;
    app->wheel_at = now;
    int64_t travel_fine = (int64_t)travel * WHEEL_FINE + app->wheel;
    int step = (int)(travel_fine / app->wheel_units);
    app->wheel = (int)(travel_fine % app->wheel_units);

    int next = app->focus - step;
    app->focus = next < 0 ? 0 : next >= count ? count - 1 : next;
    if (app->screen == WORLD_CLOCK) {
        if (app->focus < app->wc_top)
            app->wc_top = app->focus;
        if (app->focus >= app->wc_top + WC_ROWS)
            app->wc_top = app->focus - WC_ROWS + 1;
    }
}

void app_input(struct app *app, const char *message, int64_t now, const struct tm *today)
{
    struct layout layout;
    make_layout(app, &layout);
    bool world_clock = app->screen == WORLD_CLOCK;
    char phase;
    int a, b;
    if (sscanf(message, "w %d", &a) == 1) {
        move_focus(app, a, world_clock ? WC_CITY_COUNT : layout.count, now);
    } else if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
        int index = hit(&layout, a, b);
        if (phase == 'd') {
            app->wheel = 0;
            app->touch_button = index;
        }
        if (phase == 'u') {
            bool pressed = index >= 0 && index == app->touch_button;
            app->touch_button = -1;
            if (pressed) {
                int code = layout.items[index].code;
                app->focus = world_clock ? code : index;
                activate(app, code, now, today);
            }
        }
    } else if (sscanf(message, "k %c %d", &phase, &a) == 2 && phase == 'u') {
        if (a == CENTRE_KEY) {
            app->wheel = 0;
            if (world_clock)
                activate(app, app->focus, now, today);
            else if (app->focus < layout.count)
                activate(app, layout.items[app->focus].code, now, today);
        }
        if (a == RETURN_KEY) {
            if (app->screen == HOME)
                app->quit = true;
            else if (app->screen == WC_DETAIL)
                screen(app, WORLD_CLOCK);
            else if (app->screen == EDIT_TASK || app->screen == TASK_ACTIONS)
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
     * line, the screen title (the calendar's month + year) and < Back. */
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
    if (app->screen != HOME)
        draw_text(pixels, stride, 10, 3, "< Back", INK);
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
        for (int i = 0; i < layout.count; ++i)
            draw_world_clock_row(app, pixels, stride, &layout.items[i], utc, today);
        break;
    case WC_DETAIL:
        draw_world_clock_detail(app, pixels, stride, utc);
        break;
    case HOME:
        break;
    }

    /* World Clock rows are drawn above; the generic list loop below
     * would draw them a second time. */
    if (app->screen == WORLD_CLOCK)
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
