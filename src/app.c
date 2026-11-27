/* SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "q2pod_scheme.h"
#include "worldclock.h"
#include "utility_icons.h"

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
       WC_ROW_H = 52, WC_ROWS = 5,
       /* Tiny world map (WC_MAP): equirectangular, lon -180..180 across,
        * lat 84..-58 down, so 1 degree is about 1 pixel each way. */
       WC_MAP_X = 6, WC_MAP_Y = 30, WC_MAP_W = 363, WC_MAP_H = 170,
       WC_MAP_LAT_TOP = 84, WC_MAP_LAT_SPAN = 142,
       WC_MAP_STRIP_Y = 202, WC_MAP_INFO_Y = 230, WC_MAP_HINT_Y = 297 };

struct button {
    int x, y, w, h, code;
    char label[TASK_LENGTH + 8]; /* "[X] " + a full task name */
};

/* Task-options screen: the task name wraps onto up to TASK_LINES lines
 * from y=TASK_TEXT_Y, and the action rows follow it. */
enum { TASK_TEXT_Y = 34, TASK_LINE_H = 22, TASK_LINES = 6 };
static int wrap_lines(const char *text, int width, char lines[][TASK_LENGTH + 4], int max_lines);

/* The task editor's keyboard: the Q2's own T9 keypad (Shanling's stock
 * kb_default_t9, a 4x4 AWTK keypad, as Q2 Pod shows it, EN being a case
 * key), as Books (q2-books' T9Keypad) draws it too:
 *
 *   ,      abc    def    DEL
 *   ghi    jkl    mno    ABC   (case: abc on the capitals)
 *   pqrs   tuv    wxyz   123   (digits: each key types its number)
 *   .?!    space  Done   CLR
 *
 * Letters are multi-tap: the same key again within T9_REPEAT_MS replaces
 * the letter just typed with the key's next one; another key or a pause
 * keeps it. The letter is in the text from the first press, so no timer
 * is needed. Button codes are the key indexes. */
enum { T9_COMMA, T9_ABC, T9_DEF, T9_DEL, T9_GHI, T9_JKL, T9_MNO, T9_CASE,
       T9_PQRS, T9_TUV, T9_WXYZ, T9_DIGITS, T9_STOPS, T9_SPACE, T9_DONE, T9_CLEAR,
       T9_KEYS, T9_REPEAT_MS = 1200,
       KB_X = 12, KB_Y = 92, KB_W = 86, KB_H = 52, KB_GAP_X = 2, KB_GAP_Y = 3 };

static bool t9_function(int key)
{
    return key == T9_DEL || key == T9_CASE || key == T9_DIGITS || key == T9_DONE ||
           key == T9_CLEAR;
}

/* The characters a key cycles through (empty for function keys). */
static void t9_letters(const struct app *app, int key, char *out, size_t size)
{
    static const char *const lower[T9_KEYS] = {",'-:;", "abc", "def", "", "ghi", "jkl", "mno", "",
                                               "pqrs", "tuv", "wxyz", "", ".?!", " ", "", ""};
    static const char digit[T9_KEYS] = {'1','2','3',0,'4','5','6',0,'7','8','9',0,'0',' ',0,0};
    out[0] = '\0';
    if (key < 0 || key >= T9_KEYS)
        return;
    if (app->t9_digits) {
        if (digit[key])
            snprintf(out, size, "%c", digit[key]);
        return;
    }
    snprintf(out, size, "%s", lower[key]);
    for (char *c = out; app->t9_upper && *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c -= 'a' - 'A';
}

static void t9_label(const struct app *app, int key, char *out, size_t size)
{
    switch (key) {
    case T9_DEL: snprintf(out, size, "DEL"); break;
    case T9_CASE: snprintf(out, size, "%s", app->t9_upper ? "abc" : "ABC"); break;
    case T9_DIGITS: snprintf(out, size, "%s", app->t9_digits ? "abc" : "123"); break;
    case T9_SPACE: snprintf(out, size, "space"); break;
    case T9_DONE: snprintf(out, size, "Done"); break;
    case T9_CLEAR: snprintf(out, size, "CLR"); break;
    case T9_COMMA: snprintf(out, size, "%s", app->t9_digits ? "1" : ","); break;
    default: t9_letters(app, key, out, size); break;
    }
}

/* The key whose letters show above the keypad while a multi-tap is open
 * (-1 when none is); app->t9_tap is which of them is in the text. */
static int t9_pending(const struct app *app, int64_t now)
{
    char set[8];
    if (app->t9_last < 0 || now - app->t9_at > T9_REPEAT_MS)
        return -1;
    t9_letters(app, app->t9_last, set, sizeof set);
    return strlen(set) > 1 ? app->t9_last : -1;
}

static void t9_reset(struct app *app)
{
    app->t9_upper = app->t9_digits = false;
    app->t9_last = -1;
    app->t9_tap = 0;
}

struct layout {
    struct button items[36];
    int count;
};

static const char *const titles[] = {
    "Utilities", "Stopwatch", "Calendar", "To-do", "Calculator",
    "World Clock", "World Clock", "New task", "Task options", "World map",
    "Clocks & Timers", "Tools & Calculators", "Pomodoro", "Metronome",
    "Timer", "Unit Converter", "Random Generator", "Date Calculator",
    "Engineering", "Event Counter"
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
    extra_init(&app->extra, task_path);
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

/* MM:SS.t under an hour, H:MM:SS.t after, so the readout stays short
 * enough to draw large. Liberation Sans digits share one advance, so
 * the running time never shifts sideways. */
static void format_elapsed(char *text, size_t size, int64_t ms)
{
    if (ms < 3600000)
        snprintf(text, size, "%02lld:%02lld.%02lld",
                 (long long)(ms / 60000), (long long)(ms / 1000 % 60),
                 (long long)(ms / 10 % 100));
    else
        snprintf(text, size, "%lld:%02lld:%02lld.%02lld",
                 (long long)(ms / 3600000), (long long)(ms / 60000 % 60),
                 (long long)(ms / 1000 % 60), (long long)(ms / 10 % 100));
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
        /* Keep unfinished utilities and their code available for future work,
         * but do not expose their category submenus to users yet. */
        static const enum screen order[] = {STOPWATCH, COUNTDOWN, WORLD_CLOCK, CALENDAR, TODO, CALCULATOR};
        for (unsigned i = 0; i < sizeof order / sizeof order[0]; ++i)
            button(layout, 0, 26 + (int)i * 32, WIDTH, 32, order[i], titles[order[i]]);
        break;
    }
    case TIME_GROUP: {
        static const enum screen order[]={POMODORO,METRONOME,COUNTDOWN};
        for(int i=0;i<3;i++)
            button(layout,0,26+i*36,WIDTH,36,order[i],titles[order[i]]);
        break;
    }
    case TOOL_GROUP: {
        static const enum screen order[]={CONVERTER,RANDOMIZER,DATE_CALC,ENGINEERING,EVENT_COUNTER};
        for(int i=0;i<5;i++)
            button(layout,0,26+i*36,WIDTH,36,order[i],titles[order[i]]);
        break;
    }
    case RINGTONE: break;
    case COUNTDOWN:
        for (int i=0;i<3;i++) button(layout,12+i*117,45,117,146,i,"");
        button(layout,20,218,150,54,3,"Cancel");
        button(layout,205,218,150,54,4,app->extra.countdown.running?"Pause":"Start");
        break;
    case POMODORO: case METRONOME: case CONVERTER:
    case RANDOMIZER: case DATE_CALC: case ENGINEERING: case EVENT_COUNTER: {
        ToolKind kind=app->screen==POMODORO?TOOL_POMODORO:
                      app->screen==METRONOME?TOOL_METRONOME:
                      app->screen==COUNTDOWN?TOOL_COUNTDOWN:
                      app->screen==CONVERTER?TOOL_CONVERTER:
                      app->screen==RANDOMIZER?TOOL_RANDOM:
                      app->screen==DATE_CALC?TOOL_DATE:
                      app->screen==ENGINEERING?TOOL_ENGINEERING:TOOL_COUNTER;
        /* The compact screen design uses one dominant action and one
         * short settings strip; the old 3x3 grid made every control
         * equally prominent on a 375x320 screen.
         * Order of button() calls is wheel focus order. */
        const int primary_y=190, secondary_y=250;
        const int count=extra_button_count(kind);
        if(kind==TOOL_POMODORO) {
            for(int i=0;i<3;i++)
                button(layout,14+i*118,primary_y,110,46,i,
                       extra_button_label(&app->extra,kind,i));
            for(int i=3;i<7;i++)
                button(layout,10+(i-3)*91,secondary_y,83,42,i,
                       extra_button_label(&app->extra,kind,i));
        } else if(kind==TOOL_METRONOME) {
            /* Keep PR #9's classic swinging-pendulum metronome and its
             * dedicated lower controls instead of covering the pyramid. */
            for(int i=0;i<count;i++)
                button(layout,15+(i%3)*116,226+(i/3)*32,110,28,i,
                       extra_button_label(&app->extra,kind,i));
        } else if(kind==TOOL_COUNTDOWN) {
            for(int i=0;i<2;i++)
                button(layout,14+i*178,primary_y,168,46,i,
                       extra_button_label(&app->extra,kind,i));
            for(int i=2;i<count;i++)
                button(layout,10+(i-2)*91,secondary_y,83,42,i,
                       extra_button_label(&app->extra,kind,i));
        } else if(kind==TOOL_CONVERTER || kind==TOOL_ENGINEERING) {
            for(int i=0;i<2;i++)
                button(layout,14+i*178,primary_y,168,46,i,
                       extra_button_label(&app->extra,kind,i));
            for(int i=2;i<count;i++)
                button(layout,10+(i-2)*91,secondary_y,83,42,i,
                       extra_button_label(&app->extra,kind,i));
        } else if(kind==TOOL_RANDOM) {
            for(int i=0;i<2;i++)
                button(layout,14+i*178,primary_y,168,46,i,
                       extra_button_label(&app->extra,kind,i));
            for(int i=2;i<count;i++)
                button(layout,10+(i-2)*91,secondary_y,83,42,i,
                       extra_button_label(&app->extra,kind,i));
        } else if(kind==TOOL_DATE) {
            button(layout,14,primary_y,347,46,4,
                   extra_button_label(&app->extra,kind,4));
            for(int i=0;i<4;i++)
                button(layout,10+i*91,secondary_y,83,42,i,
                       extra_button_label(&app->extra,kind,i));
        } else if(kind==TOOL_COUNTER) {
            for(int i=0;i<3;i++)
                button(layout,14+i*118,primary_y,110,46,i,
                       extra_button_label(&app->extra,kind,i));
            for(int i=3;i<count;i++)
                button(layout,14+(i-3)*178,secondary_y,168,42,i,
                       extra_button_label(&app->extra,kind,i));
        }
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
        button(layout, 267, 254, 88, 60, 0, app->running ? "Pause" : "Start");
        button(layout, 20, 254, 88, 60, app->running?1:2, app->running?"Lap":"Reset");
        break;
    case CALENDAR:
        button(layout, 18, 247, 108, 42, -1, "< Month");
        button(layout, 134, 247, 107, 42, 0, "Today");
        button(layout, 249, 247, 108, 42, 1, "Month >");
        break;
    case TODO: {
        int start = app->task_page * TASKS_PER_PAGE;
        for (int i = start; i < app->task_count && i < start + TASKS_PER_PAGE; ++i) {
            char label[TASK_LENGTH + 8];
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
    case TASK_ACTIONS: {
        /* List rows below the wrapped task text. */
        char lines[TASK_LINES][TASK_LENGTH + 4];
        int top = TASK_TEXT_Y + TASK_LINE_H *
                  wrap_lines(app->tasks[app->task_index].text, WIDTH - 40, lines, TASK_LINES) + 8;
        button(layout, 0, top, WIDTH, 32, 0,
               app->tasks[app->task_index].done ? "Mark open" : "Mark done");
        button(layout, 0, top + 32, WIDTH, 32, 1, "Delete task");
        break;
    }
    case EDIT_TASK:
        /* T9 keypad under the field and the open key's letters. */
        for (int key = 0; key < T9_KEYS; ++key) {
            char label[8];
            t9_label(app, key, label, sizeof label);
            button(layout, KB_X + key % 4 * (KB_W + KB_GAP_X), KB_Y + key / 4 * (KB_H + KB_GAP_Y),
                   KB_W, KB_H, key, label);
        }
        break;
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

static void save_draft(struct app *app)
{
    size_t length = strlen(app->draft);
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
}

static void edit_task(struct app *app, int key, int64_t now)
{
    size_t length = strlen(app->draft);
    bool repeat = key == app->t9_last && now - app->t9_at <= T9_REPEAT_MS;
    switch (key) {
    case T9_DEL:
        if (length)
            app->draft[length - 1] = '\0';
        app->t9_last = -1;
        return;
    case T9_CLEAR:
        app->draft[0] = '\0';
        app->t9_last = -1;
        return;
    case T9_CASE:
        app->t9_upper = !app->t9_upper;
        app->t9_digits = false;
        app->t9_last = -1;
        return;
    case T9_DIGITS:
        app->t9_digits = !app->t9_digits;
        app->t9_last = -1;
        return;
    case T9_DONE:
        app->t9_last = -1;
        save_draft(app);
        return;
    }
    char set[8];
    t9_letters(app, key, set, sizeof set);
    size_t count = strlen(set);
    if (!count)
        return;
    if (repeat && count > 1 && length) {
        app->t9_tap = (app->t9_tap + 1) % (int)count;
        app->draft[length - 1] = set[app->t9_tap];
    } else {
        if (length >= TASK_LENGTH)
            return;
        app->t9_tap = 0;
        app->draft[length] = set[0];
        app->draft[length + 1] = '\0';
    }
    app->t9_last = key;
    app->t9_at = now;
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
    case TIME_GROUP:
    case TOOL_GROUP:
        screen(app,(enum screen)code);
        extra_enter(&app->extra,
            code==POMODORO?TOOL_POMODORO:
            code==METRONOME?TOOL_METRONOME:
            code==COUNTDOWN?TOOL_COUNTDOWN:
            code==CONVERTER?TOOL_CONVERTER:
            code==RANDOMIZER?TOOL_RANDOM:
            code==DATE_CALC?TOOL_DATE:
            code==ENGINEERING?TOOL_ENGINEERING:TOOL_COUNTER,now);
        break;
    case POMODORO: case METRONOME: case COUNTDOWN: case CONVERTER:
    case RANDOMIZER: case DATE_CALC: case ENGINEERING: case EVENT_COUNTER: {
        ToolKind kind=app->screen==POMODORO?TOOL_POMODORO:
                      app->screen==METRONOME?TOOL_METRONOME:
                      app->screen==COUNTDOWN?TOOL_COUNTDOWN:
                      app->screen==CONVERTER?TOOL_CONVERTER:
                      app->screen==RANDOMIZER?TOOL_RANDOM:
                      app->screen==DATE_CALC?TOOL_DATE:
                      app->screen==ENGINEERING?TOOL_ENGINEERING:TOOL_COUNTER;
        extra_action(&app->extra,kind,code,now,today);
        break;
    }
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
            t9_reset(app);
            app->save_failed = false;
            screen(app, EDIT_TASK);
            app->focus = T9_ABC; /* the wheel starts on abc, as Q2's T9 */
        } else if (code < 0) {
            app->task_page += code == -2 ? -1 : 1;
            screen(app, TODO);
        } else {
            app->task_index = code;
            screen(app, TASK_ACTIONS);
        }
        break;
    case TASK_ACTIONS: task_action(app, code); break;
    case EDIT_TASK: edit_task(app, code, now); break;
    case CALCULATOR: calculate(app, code); break;
    case RINGTONE: break; /* Dedicated input handler. */
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

static void wheel_move(struct app *app, int travel, int64_t now)
{
    if (!travel)
        return;
    /* The card-app protocol sends signed travel, without wheel-lift events. */
    if (now - app->wheel_at >= WHEEL_IDLE_MS ||
        (app->wheel > 0 && travel < 0) || (app->wheel < 0 && travel > 0))
        app->wheel = 0;
    app->wheel_at = now;
    int64_t travel_fine = (int64_t)travel * WHEEL_FINE + app->wheel;
    int step = (int)(travel_fine / app->wheel_units);
    app->wheel = (int)(travel_fine % app->wheel_units);

    if (app->screen == STOPWATCH) {
        int mode=(app->stopwatch_mode-step)%3;
        app->stopwatch_mode=mode<0?mode+3:mode;
        return;
    }
    if (app->screen == WC_MAP) {
        int next = app->wc_map - step;
        app->wc_map = next < 0 ? 0 : next >= WC_MAP_COUNT ? WC_MAP_COUNT - 1 : next;
        return;
    }

    bool world_clock = app->screen == WORLD_CLOCK;
    struct layout layout;
    make_layout(app, &layout);
    int count = world_clock ? WC_CITY_COUNT + 1 : layout.count;
    if (!count)
        return;
    int next = app->focus - step;
    app->focus = next < 0 ? 0 : next >= count ? count - 1 : next;
    if (world_clock) {
        if (app->focus < WC_CITY_COUNT)
            app->wc_selected = app->focus;
        if (app->focus < app->wc_top)
            app->wc_top = app->focus;
        if (app->focus >= app->wc_top + WC_ROWS)
            app->wc_top = app->focus - WC_ROWS + 1;
    }
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
    if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
        int index = hit(&layout, a, b);
        if (phase == 'd') {
            app->wheel = 0;
            app->touch_button = index;
        }
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
        if (a == CENTRE_KEY || a == RETURN_KEY)
            app->wheel = 0;
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

static void world_clock_map_input(struct app *app, const char *message)
{
    char phase;
    int a, b;
    if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
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
            app->wheel = 0;
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
        if (a == CENTRE_KEY || a == RETURN_KEY)
            app->wheel = 0;
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
}

static void timer_adjust(struct app *app, int field, int delta)
{
    CountdownState *t=&app->extra.countdown;
    if (t->running || field<0 || field>2) return;
    int seconds=(int)(t->remaining_ms/1000);
    int parts[3]={seconds/3600,seconds/60%60,seconds%60};
    int limit=field==0?24:60;
    parts[field]=((parts[field]+delta)%limit+limit)%limit;
    t->remaining_ms=t->preset_ms=(int64_t)(parts[0]*3600+parts[1]*60+parts[2])*1000;
    app->timer_started=false;
    app->timer_alarm=false;
}

static void timer_action(struct app *app, int code, int64_t now)
{
    CountdownState *t=&app->extra.countdown;
    if(code<3) {
        if(!t->running && !app->timer_started && !app->timer_alarm) app->timer_edit=!app->timer_edit;
        return;
    }
    app->timer_edit=false;
    if(code==3) {
        t->running=false;
        t->remaining_ms=t->preset_ms;
        app->timer_started=app->timer_alarm=false;
        app->focus=0;
    } else if(app->timer_alarm) {
        app->timer_alarm=app->timer_started=false;
        t->remaining_ms=t->preset_ms;
        app->focus=0;
    } else if(t->running) {
        t->remaining_ms=t->due_ms>now?t->due_ms-now:0;
        t->running=false;
    } else if(t->remaining_ms>0) {
        t->due_ms=now+t->remaining_ms;
        t->running=true;
        app->timer_started=true;
        app->focus=4;
    }
}

static void timer_input(struct app *app,const char *message,int64_t now)
{
    int travel, a,b; char phase;
    if(sscanf(message,"w %d",&travel)==1) {
        if(now-app->wheel_at>=WHEEL_IDLE_MS ||
           (app->wheel>0 && travel<0)||(app->wheel<0 && travel>0)) app->wheel=0;
        app->wheel_at=now;
        int64_t fine=(int64_t)travel*WHEEL_FINE+app->wheel;
        int step=(int)(fine/app->wheel_units);
        app->wheel=(int)(fine%app->wheel_units);
        if(app->timer_edit) timer_adjust(app,app->focus,-step);
        else {
            int min=app->timer_started||app->timer_alarm?3:0;
            int next=app->focus-step;
            app->focus=next<min?min:next>4?4:next;
        }
    } else if(sscanf(message,"k %c %d",&phase,&a)==2 && phase=='u') {
        if(a==CENTRE_KEY) timer_action(app,app->focus,now);
        else if(a==RETURN_KEY) {
            if(app->timer_edit) app->timer_edit=false;
            else screen(app,HOME);
        }
    } else if(sscanf(message,"t %c %d %d",&phase,&a,&b)==3) {
        struct layout layout; make_layout(app,&layout);
        int index=hit(&layout,a,b);
        if(phase=='d') {
            app->touch_button=index;
            app->timer_touch_y=b;
        } else if(phase=='u') {
            int field=app->touch_button;
            app->touch_button=-1;
            if(field<0) return;
            if(field<3 && !app->timer_started && !app->timer_alarm) {
                app->focus=field;
                int dy=app->timer_touch_y-b;
                if(dy>12||dy<-12) timer_adjust(app,field,dy/12);
                else if(index==field && b<94) timer_adjust(app,field,-1);
                else if(index==field && b>142) timer_adjust(app,field,1);
                else if(index==field) app->timer_edit=!app->timer_edit;
            } else if(field>=3 && index==field) {
                app->focus=field;
                timer_action(app,field,now);
            }
        }
    }
}


static int clip_clamp(int n,int lo,int hi)
{
    return n<lo?lo:n>hi?hi:n;
}
static void clip_adjust(struct app *app,int field,int target)
{
    int end=app->clip_start+app->clip_length;
    if(field==0) {
        app->clip_start=clip_clamp(target,end>30?end-30:0,end-5);
        app->clip_length=end-app->clip_start;
    } else {
        int limit=app->clip_start+30;
        if(limit>app->clip_duration) limit=app->clip_duration;
        app->clip_length=clip_clamp(target,app->clip_start+5,limit)-app->clip_start;
    }
    app->clip_status=0;
}
static void clip_input(struct app *app,const char *message,int64_t now)
{
    int travel,x,y; char phase;
    if(app->clip_duration<5) {
        if(sscanf(message,"k %c %d",&phase,&x)==2 && phase=='u' && x==RETURN_KEY) app->quit=true;
        return;
    }
    if(sscanf(message,"w %d",&travel)==1) {
        if(now-app->wheel_at>=WHEEL_IDLE_MS) app->wheel=0;
        app->wheel_at=now;
        int64_t fine=(int64_t)travel*WHEEL_FINE+app->wheel;
        int step=(int)(fine/app->wheel_units);
        app->wheel=(int)(fine%app->wheel_units);
        if(app->clip_edit && app->clip_focus<3) {
            if(app->clip_focus==0) {
                app->clip_start=clip_clamp(app->clip_start-step,0,app->clip_duration-app->clip_length);
                app->clip_status=0;
            } else {
                int field=app->clip_focus-1;
                clip_adjust(app,field,app->clip_start+(field?app->clip_length:0)-step);
            }
        } else app->clip_focus=clip_clamp(app->clip_focus-step,0,4);
    } else if(sscanf(message,"k %c %d",&phase,&x)==2 && phase=='u') {
        if(x==RETURN_KEY) {
            if(app->clip_edit) app->clip_edit=false;
            else app->quit=true;
        } else if(x==CENTRE_KEY) {
            if(app->clip_focus<3) app->clip_edit=!app->clip_edit;
            else app->clip_request=app->clip_focus-2;
        }
    } else if(sscanf(message,"t %c %d %d",&phase,&x,&y)==3) {
        if(phase=='d') {
            app->clip_drag=0;
            if(y>=85&&y<=202) {
                int left=15+(int)((int64_t)345*app->clip_start/app->clip_duration);
                int right=15+(int)((int64_t)345*(app->clip_start+app->clip_length)/app->clip_duration);
                                if(abs(x-left)<=8 && (abs(x-left)<=abs(x-right))) app->clip_drag=1;
                else if(abs(x-right)<=8) app->clip_drag=2;
                else app->clip_drag=3;
                int target=clip_clamp((int)((int64_t)(x-15)*app->clip_duration/345),0,app->clip_duration);
                app->clip_drag_offset=(x>=left&&x<=right)?target-app->clip_start:app->clip_length/2;
            }
        }
        if(app->clip_drag && (phase=='d'||phase=='m'||phase=='u')) {
            int target=clip_clamp((int)((int64_t)(x-15)*app->clip_duration/345),0,app->clip_duration);
            if(app->clip_drag==3) {
                app->clip_start=clip_clamp(target-app->clip_drag_offset,0,app->clip_duration-app->clip_length);
                app->clip_status=0;
            } else {
                app->clip_focus=app->clip_drag;
                clip_adjust(app,app->clip_focus-1,target);
            }
            if(phase=='u') app->clip_drag=0;
        } else if(phase=='u'&&y>=205&&y<233) {
            app->clip_focus=x<WIDTH/2?1:2; app->clip_edit=true;
        } else if(phase=='u'&&y>=251&&y<=303) {
            app->clip_focus=x<WIDTH/2?3:4;
            app->clip_edit=false; app->clip_request=app->clip_focus-2;
        }
    }
}
void app_input(struct app *app, const char *message, int64_t now, const struct tm *today)
{
    if(app->screen==RINGTONE) { clip_input(app,message,now); return; }
    if(app->screen==COUNTDOWN) { timer_input(app,message,now); return; }
    int travel;
    if (sscanf(message, "w %d", &travel) == 1) {
        wheel_move(app, travel, now);
        return;
    }

    if (app->screen == WORLD_CLOCK) {
        world_clock_input(app, message, now);
        return;
    }
    if (app->screen == WC_MAP) {
        world_clock_map_input(app, message);
        return;
    }
    struct layout layout;
    make_layout(app, &layout);
    char phase;
    int a, b;
    if (sscanf(message, "t %c %d %d", &phase, &a, &b) == 3) {
        int index = hit(&layout, a, b);
        if (phase == 'd') {
            app->wheel = 0;
            app->touch_button = index;
        }
        if (phase == 'u') {
            bool pressed = index >= 0 && index == app->touch_button;
            app->touch_button = -1;
            if (pressed) {
                app->focus = index;
                activate(app, layout.items[index].code, now, today);
            }
        }
    } else if (sscanf(message, "k %c %d", &phase, &a) == 2 && phase == 'u') {
        if (a == CENTRE_KEY || a == RETURN_KEY)
            app->wheel = 0;
        if (a == CENTRE_KEY && app->screen == STOPWATCH)
            activate(app,0,now,today);
        else if (a == CENTRE_KEY && app->focus < layout.count)
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
            else if (app->screen == POMODORO || app->screen == METRONOME ||
                     app->screen == COUNTDOWN)
                screen(app,TIME_GROUP);
            else if (app->screen == CONVERTER || app->screen == RANDOMIZER ||
                     app->screen == DATE_CALC || app->screen == ENGINEERING ||
                     app->screen == EVENT_COUNTER)
                screen(app,TOOL_GROUP);
            else
                screen(app, HOME);
        }
    }
}

#include "font.h"
#include "stopwatch_digits.h"

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

/* Twelve-pixel icon font: only 360 bytes of immutable monochrome masks. */
static void draw_font_icon(uint32_t *pixels,unsigned stride,int x,int y,
                           UtilityIcon icon,uint32_t color)
{
    if(icon<0||icon>=ICON_COUNT)return;
    for(int row=0;row<12;row++) {
        const uint16_t bits=icon_font_12x12[icon][row];
        for(int col=0;col<12;col++)
            if(bits & (1u<<(11-col)))
                fill(pixels,stride,x+col,y+row,1,1,color);
    }
}
static UtilityIcon menu_icon(enum screen item)
{
    switch(item) {
    case STOPWATCH:return ICON_STOPWATCH;
    case WORLD_CLOCK:return ICON_WORLD;
    case CALENDAR:return ICON_CALENDAR;
    case TODO:return ICON_TODO;
    case CALCULATOR:return ICON_CALC;
    case TIME_GROUP:return ICON_TIMERS;
    case TOOL_GROUP:return ICON_TOOLS;
    case POMODORO:return ICON_POMO;
    case METRONOME:return ICON_METRO;
    case COUNTDOWN:return ICON_COUNTDOWN;
    case CONVERTER:return ICON_CONVERTER;
    case RANDOMIZER:return ICON_RANDOM;
    case DATE_CALC:return ICON_DATE;
    case ENGINEERING:return ICON_ENGINEERING;
    case EVENT_COUNTER:return ICON_COUNTER;
    default:return ICON_TOOLS;
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

/* Copy text into out, cut with "..." so it fits max_width; tail keeps
 * the end instead (the task editor shows what was just typed). */
static void fit_text(char *out, size_t size, const char *text, int max_width, bool tail)
{
    size_t length = strlen(text);
    if (text_width(text) <= max_width || size < 5) {
        snprintf(out, size, "%s", text);
        return;
    }
    int room = max_width - text_width("..."), used = 0;
    size_t n = 0;
    while (n < length && n + 4 < size) {
        int w = f20_adv[glyph((unsigned char)text[tail ? length - 1 - n : n])];
        if (used + w > room)
            break;
        used += w;
        ++n;
    }
    if (tail)
        snprintf(out, size, "...%s", text + length - n);
    else
        snprintf(out, size, "%.*s...", (int)n, text);
}

/* Greedy word wrap into at most max_lines lines; a word wider than the
 * line breaks between letters, and overflow ends the last line in "...".
 * Returns the line count (at least 1). */
static int wrap_lines(const char *text, int width, char lines[][TASK_LENGTH + 4], int max_lines)
{
    int count = 0;
    while (count < max_lines) {
        while (*text == ' ')
            ++text;
        size_t n = 0, cut = 0;
        int used = 0;
        while (text[n]) {
            int w = f20_adv[glyph((unsigned char)text[n])];
            if (used + w > width)
                break;
            used += w;
            if (text[++n - 1] == ' ')
                cut = n - 1;
        }
        if (text[n] && text[n] != ' ' && cut)
            n = cut;
        if (!n && text[0])
            n = 1;
        snprintf(lines[count++], TASK_LENGTH + 4, "%.*s", (int)n, text);
        text += n;
        while (*text == ' ')
            ++text;
        if (!*text)
            return count;
    }
    /* More text than lines: mark the cut on the last line. */
    char last[TASK_LENGTH + 8];
    snprintf(last, sizeof last, "%s%s", lines[count - 1], text);
    fit_text(lines[count - 1], TASK_LENGTH + 4, last, width, false);
    return count;
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

/* Convex-quad fill (scanline): the pyramid body, face, base and the
 * pendulum's sliding weight are all quads, drawn with the firmware's
 * own fill primitive — no image files, like the World Clock map. */
static void fill_quad(uint32_t *pixels, unsigned stride,
                      int x0, int y0, int x1, int y1,
                      int x2, int y2, int x3, int y3, uint32_t color)
{
    const int xs[4] = {x0, x1, x2, x3}, ys[4] = {y0, y1, y2, y3};
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < 4; ++i) {
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
    }
    for (int y = ymin; y <= ymax; ++y) {
        int xmin = 1 << 30, xmax = -(1 << 30);
        for (int i = 0; i < 4; ++i) {
            int j = (i + 1) % 4;
            int ya = ys[i], yb = ys[j];
            if ((ya <= y && y < yb) || (yb <= y && y < ya)) {
                int x = xs[i] + (xs[j] - xs[i]) * (y - ya) / (yb - ya);
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
        }
        if (xmax >= xmin)
            fill(pixels, stride, xmin, y, xmax - xmin + 1, 1, color);
    }
}

static void segment(uint32_t *pixels, unsigned stride,
                    int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (!steps) {
        fill(pixels, stride, x0, y0, 1, 1, color);
        return;
    }
    for (int i = 0; i <= steps; ++i)
        fill(pixels, stride, x0 + dx * i / steps, y0 + dy * i / steps, 1, 1, color);
}

/* The pendulum rod: a 5 px wide gold bar from pivot to tip. */
static void rod(uint32_t *pixels, unsigned stride,
                int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (!steps) steps = 1;
    for (int i = 0; i <= steps; i += 2)
        fill(pixels, stride, x0 + dx * i / steps - 2, y0 + dy * i / steps - 2, 5, 5, color);
}

/* --- Classic metronome (Wittner pyramid) ---
 * Trapezoid body with a blue-tinted face, a dark slotted base, tempo
 * ticks down the face (40-208 BPM, log scale), and a gold pendulum
 * pivoted at the base that swings at the beat: its angle comes from
 * metro_pendulum_angle_deg, so the beat ticks as it crosses centre,
 * and the sliding weight sits on the current BPM's scale mark. The
 * palette is fixed mid-tone like the World Clock map's atlas colours
 * (blue face, gold rod) so it reads on the light and dark paper;
 * only the body and base follow the theme. No image files. */
enum {
    METRO_CX = 187, METRO_TOP_Y = 54, METRO_TOP_HALF = 31,
    METRO_BOT_Y = 180, METRO_BOT_HALF = 56,
    METRO_BASE_TOP_Y = 178, METRO_BASE_BOT_Y = 198, METRO_BASE_BOT_HALF = 62,
    METRO_PIVOT_Y = 192, METRO_ROD_LEN = 122,
    METRO_FACE_TOP_Y = 60, METRO_FACE_TOP_HALF = 26,
    METRO_FACE_BOT_Y = 174, METRO_FACE_BOT_HALF = 50,
    METRO_GOLD = 0xd9a821, METRO_GOLD_BRIGHT = 0xe8b93a, METRO_FACE_BLUE = 0x2a6ad8,
};

static void draw_metronome(struct app *app, uint32_t *pixels, unsigned stride,
                           int64_t now, const char *bpm_line, const char *note)
{
    draw_text_fitted(pixels, stride, 20, 30, 335, bpm_line, ACCENT);
    /* Body, then the inset face. */
    fill_quad(pixels, stride,
              METRO_CX - METRO_TOP_HALF, METRO_TOP_Y,
              METRO_CX + METRO_TOP_HALF, METRO_TOP_Y,
              METRO_CX + METRO_BOT_HALF, METRO_BOT_Y,
              METRO_CX - METRO_BOT_HALF, METRO_BOT_Y,
              blend(BG, INK, 24));
    fill_quad(pixels, stride,
              METRO_CX - METRO_FACE_TOP_HALF, METRO_FACE_TOP_Y,
              METRO_CX + METRO_FACE_TOP_HALF, METRO_FACE_TOP_Y,
              METRO_CX + METRO_FACE_BOT_HALF, METRO_FACE_BOT_Y,
              METRO_CX - METRO_FACE_BOT_HALF, METRO_FACE_BOT_Y,
              blend(BG, METRO_FACE_BLUE, 170));
    /* Tempo scale: ticks at the face edges for 40..208 BPM. */
    static const int marks[] = {40, 60, 80, 100, 120, 140, 160, 180, 208};
    for (unsigned i = 0; i < sizeof marks / sizeof marks[0]; ++i) {
        int y = metro_scale_y(marks[i], METRO_PIVOT_Y);
        int half = METRO_FACE_TOP_HALF +
                   (METRO_FACE_BOT_HALF - METRO_FACE_TOP_HALF) *
                   (y - METRO_FACE_TOP_Y) / (METRO_FACE_BOT_Y - METRO_FACE_TOP_Y);
        fill(pixels, stride, METRO_CX - half - 6, y, 9, 2, INK);
        fill(pixels, stride, METRO_CX + half - 3, y, 9, 2, INK);
    }
    /* Dark base with three vent slots, wider towards the bottom. */
    uint32_t base = SCHEME.paper == 0 ? 0x3a3a3a : 0x1c1c1c;
    fill_quad(pixels, stride,
              METRO_CX - METRO_BOT_HALF, METRO_BASE_TOP_Y,
              METRO_CX + METRO_BOT_HALF, METRO_BASE_TOP_Y,
              METRO_CX + METRO_BASE_BOT_HALF, METRO_BASE_BOT_Y,
              METRO_CX - METRO_BASE_BOT_HALF, METRO_BASE_BOT_Y,
              base);
    static const int vents[][2] = {{183, 56}, {188, 68}, {193, 80}};
    for (unsigned i = 0; i < sizeof vents / sizeof vents[0]; ++i)
        fill(pixels, stride, METRO_CX - vents[i][1] / 2, vents[i][0],
             vents[i][1], 3, BG);
    /* Outlines: body, then base. */
    segment(pixels, stride, METRO_CX - METRO_TOP_HALF, METRO_TOP_Y,
            METRO_CX + METRO_TOP_HALF, METRO_TOP_Y, INK);
    segment(pixels, stride, METRO_CX + METRO_TOP_HALF, METRO_TOP_Y,
            METRO_CX + METRO_BOT_HALF, METRO_BOT_Y, INK);
    segment(pixels, stride, METRO_CX - METRO_TOP_HALF, METRO_TOP_Y,
            METRO_CX - METRO_BOT_HALF, METRO_BOT_Y, INK);
    segment(pixels, stride, METRO_CX - METRO_BOT_HALF, METRO_BASE_TOP_Y,
            METRO_CX - METRO_BASE_BOT_HALF, METRO_BASE_BOT_Y, INK);
    segment(pixels, stride, METRO_CX + METRO_BOT_HALF, METRO_BASE_TOP_Y,
            METRO_CX + METRO_BASE_BOT_HALF, METRO_BASE_BOT_Y, INK);
    segment(pixels, stride, METRO_CX - METRO_BASE_BOT_HALF, METRO_BASE_BOT_Y,
            METRO_CX + METRO_BASE_BOT_HALF, METRO_BASE_BOT_Y, INK);
    /* Pendulum: at rest it hangs vertical; running, it swings at the
     * beat. The weight rides on the current BPM's scale mark. */
    const MetronomeState *m = &app->extra.metro;
    double angle = m->running
                       ? metro_pendulum_angle_deg(m->bpm, now - m->start_ms) : 0.0;
    double sx, cy;
    metro_rod_dir(angle, &sx, &cy);
    int tip_x = METRO_CX + (int)(METRO_ROD_LEN * sx);
    int tip_y = METRO_PIVOT_Y - (int)(METRO_ROD_LEN * cy);
    rod(pixels, stride, METRO_CX, METRO_PIVOT_Y, tip_x, tip_y, METRO_GOLD);
    disc(pixels, stride, tip_x, tip_y, 3, METRO_GOLD);
    double dist = metro_weight_dist(m->bpm);
    double wx = METRO_CX + dist * sx, wy = METRO_PIVOT_Y - dist * cy;
    /* Weight block: 18 px along the rod, 16 across it. */
    int wcx[4], wcy[4];
    const double along[4] = {-9, 9, 9, -9}, across[4] = {-8, -8, 8, 8};
    for (int i = 0; i < 4; ++i) {
        wcx[i] = (int)(wx + along[i] * sx + across[i] * cy + 0.5);
        wcy[i] = (int)(wy - along[i] * cy + across[i] * sx + 0.5);
    }
    fill_quad(pixels, stride, wcx[0], wcy[0], wcx[1], wcy[1],
              wcx[2], wcy[2], wcx[3], wcy[3], METRO_GOLD_BRIGHT);
    for (int i = 0; i < 4; ++i)
        segment(pixels, stride, wcx[i], wcy[i], wcx[(i + 1) % 4], wcy[(i + 1) % 4], INK);
    disc(pixels, stride, METRO_CX, METRO_PIVOT_Y, 4, INK);
    disc(pixels, stride, METRO_CX, METRO_PIVOT_Y, 2, METRO_GOLD);
    draw_text_fitted(pixels, stride, 20, 200, 335, note, MUTED);
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

/* Small-device display language:
 * One glanceable result above the fold; controls below.
 * Scale the existing anti-aliased Q2 font at render time rather than shipping
 * large bitmaps or a second font. No dynamic allocations. */
static void large_text(uint32_t *pixels,unsigned stride,int y,const char *label,
                       int desired_scale,uint32_t color,bool smooth);
static void large_centered(uint32_t *pixels,unsigned stride,int y,
                           const char *label,int desired_scale,uint32_t color)
{
    large_text(pixels,stride,y,label,desired_scale,color,false);
}
/* Cubic interpolation of the coverage mask for large stopwatch digits.
 * Preserve intermediate alpha values rather than clipping the edge ramp. */
static int cubic_alpha(int a, int b, int c, int d, int t, int scale)
{
    int t2 = t * t, t3 = t2 * t, s2 = scale * scale, s3 = s2 * scale;
    return (2*b*s3 + (-a+c)*t*s2 + (2*a-5*b+4*c-d)*t2*scale
            + (-a+3*b-3*c+d)*t3 + s3) / (2*s3);
}

static int mask_alpha(const unsigned char *bitmap, int width, int r, int c)
{
    if (r < 0 || r >= F20_H || c < 0 || c >= width) return 0;
    return bitmap[r*width+c];
}

static unsigned smooth_alpha(const unsigned char *bitmap, int width,
                             int r, int c, int ry, int cx, int scale)
{
    int rows[4];
    for (int i = 0; i < 4; ++i) {
        int y = r+i-1;
        rows[i] = cubic_alpha(mask_alpha(bitmap,width,y,c-1),
                              mask_alpha(bitmap,width,y,c),
                              mask_alpha(bitmap,width,y,c+1),
                              mask_alpha(bitmap,width,y,c+2),cx,scale);
    }
    int a = cubic_alpha(rows[0],rows[1],rows[2],rows[3],ry,scale);
    return (unsigned)(a < 0 ? 0 : a > 255 ? 255 : a);
}
static void large_text(uint32_t *pixels,unsigned stride,int y,const char *label,
                       int desired_scale,uint32_t color,bool smooth)
{
    if(!label || !*label)return;
    int scale=desired_scale;
    while(scale>1 && text_width(label)*scale>WIDTH-28)scale--;
    int w=text_width(label)*scale;
    int x=(WIDTH-w)/2;
    for(const unsigned char *ch=(const unsigned char *)label;*ch;ch++) {
        int index=glyph(*ch), advance=f20_adv[index];
        const unsigned char *bitmap=f20_px+f20_off[index];
        for(int r=0;r<F20_H*scale;r++) {
            if(y+r>=HEIGHT)break;
            for(int c=0;c<advance*scale;c++) {
                int src_r=r/scale,src_c=c/scale;
                int r2=src_r+1<F20_H?src_r+1:src_r;
                int c2=src_c+1<advance?src_c+1:src_c;
                /* Bilinear alpha interpolation keeps 2x enlarged
                 * bitmap-font numerals smooth, not pixel-blocky. */
                int rx=r%scale,cx=c%scale;
                unsigned a00=bitmap[src_r*advance+src_c];
                unsigned a01=bitmap[src_r*advance+c2];
                unsigned a10=bitmap[r2*advance+src_c];
                unsigned a11=bitmap[r2*advance+c2];
                unsigned alpha=(a00*(unsigned)(scale-rx)*(unsigned)(scale-cx)
                              +a01*(unsigned)(scale-rx)*(unsigned)cx
                              +a10*(unsigned)rx*(unsigned)(scale-cx)
                              +a11*(unsigned)rx*(unsigned)cx)/
                              (unsigned)(scale*scale);
                if(smooth)
                    alpha=smooth_alpha(bitmap,advance,src_r,src_c,rx,cx,scale);
                if(alpha) {
                    uint32_t *p=&pixels[(x+c)*stride+(HEIGHT-1-y-r)];
                    *p=0xff000000u|(alpha>=255?color:blend(*p,color,alpha));
                }
            }
        }
        x+=advance*scale;
    }
}
/* Secondary copy uses 15px raster glyphs generated from the existing
 * anti-aliased font, instead of squeezing 20px labels horizontally. */
static int small_text_width(const char *label)
{
    int total=0;
    for(const unsigned char *ch=(const unsigned char *)label;*ch;ch++)
        total+=(f20_adv[glyph(*ch)]*3+2)/4;
    return total;
}
static void small_line(uint32_t *pixels,unsigned stride,int x,int y,
                       int width,const char *label,uint32_t color)
{
    int tw=small_text_width(label);
    if(tw<width)x+=(width-tw)/2;
    int xmax=x+(tw<width?tw:width);
    for(const unsigned char *ch=(const unsigned char *)label;*ch;ch++) {
        int index=glyph(*ch);
        int source_w=f20_adv[index],out_w=(source_w*3+2)/4;
        const unsigned char *bitmap=f20_px+f20_off[index];
        if(x+out_w>xmax)break;
        for(int r=0;r<(F20_H*3+2)/4;r++) {
            int src_r=(r*4+1)/3;
            if(src_r>=F20_H)src_r=F20_H-1;
            for(int c=0;c<out_w;c++) {
                int src_c=(c*4+1)/3;
                if(src_c>=source_w)src_c=source_w-1;
                unsigned alpha=bitmap[src_r*source_w+src_c];
                if(alpha) {
                    uint32_t *p=&pixels[(x+c)*stride+(HEIGHT-1-y-r)];
                    *p=0xff000000u|(alpha==255?color:blend(*p,color,alpha));
                }
            }
        }
        x+=out_w;
    }
}
static void small_centered(uint32_t *pixels,unsigned stride,int y,
                           const char *label,uint32_t color)
{
    small_line(pixels,stride,12,y,WIDTH-24,label,color);
}

static int64_t tool_remaining(int64_t due,int64_t cached,int64_t now,bool running)
{
    if(!running)return cached;
    return due>now?due-now:0;
}
static void progress_bar(uint32_t *pixels,unsigned stride,int64_t rem,int64_t total)
{
    const int x=29,y=151,w=317,h=6;
    fill(pixels,stride,x,y,w,h,SCHEME.fill);
    if(total<=0) return;
    if(rem<0)rem=0;
    if(rem>total)rem=total;
    int active=(int)(rem*w/total);
    if(active>0)fill(pixels,stride,x,y,active,h,ACCENT);
}
static void tool_hero(struct app *app,uint32_t *pixels,unsigned stride,
                      int64_t now,const struct tm *today)
{
    ToolKind kind=app->screen==POMODORO?TOOL_POMODORO:
                  app->screen==METRONOME?TOOL_METRONOME:
                  app->screen==COUNTDOWN?TOOL_COUNTDOWN:
                  app->screen==CONVERTER?TOOL_CONVERTER:
                  app->screen==RANDOMIZER?TOOL_RANDOM:
                  app->screen==DATE_CALC?TOOL_DATE:
                  app->screen==ENGINEERING?TOOL_ENGINEERING:TOOL_COUNTER;
    char a[96],b[96],c[96];
    extra_display(&app->extra,kind,now,today,a,sizeof a,b,sizeof b,c,sizeof c);
    switch(kind) {
    case TOOL_POMODORO: {
        const PomodoroState *p=&app->extra.pomo;
        small_centered(pixels,stride,37,p->phase?"SHORT BREAK":"FOCUS SESSION",MUTED);
        const char *short_time=(a[0]=='0'&&a[1]=='0'&&a[2]==':')?a+3:a;
        large_centered(pixels,stride,68,short_time,2,ACCENT);
        small_centered(pixels,stride,126,p->running?"IN PROGRESS":"READY / PAUSED",INK);
        int64_t rem=tool_remaining(p->due_ms,p->remaining_ms,now,p->running);
        int64_t total=(int64_t)(p->phase?p->break_minutes:p->work_minutes)*60000;
        progress_bar(pixels,stride,rem,total);
        char line[64];
        snprintf(line,sizeof line,"FOCUS %dm  /  REST %dm  /  %d SESSIONS",
                 p->work_minutes,p->break_minutes,p->sessions);
        small_centered(pixels,stride,166,line,MUTED);
        break;
    }
    case TOOL_METRONOME: break; /* Drawn by classic pendulum renderer. */
    case TOOL_COUNTDOWN: {
        const CountdownState *t=&app->extra.countdown;
        small_centered(pixels,stride,37,"COUNTDOWN",MUTED);
        const char *short_time=(a[0]=='0'&&a[1]=='0'&&a[2]==':')?a+3:a;
        large_centered(pixels,stride,68,short_time,2,ACCENT);
        small_centered(pixels,stride,126,
                 t->running?"RUNNING":t->remaining_ms==0?"TIME ELAPSED":"READY / PAUSED",INK);
        progress_bar(pixels,stride,
                     tool_remaining(t->due_ms,t->remaining_ms,now,t->running),
                     t->preset_ms);
        small_centered(pixels,stride,165,"ADJUST DURATION BELOW",MUTED);
        break;
    }
    case TOOL_CONVERTER: {
        small_centered(pixels,stride,34,"FROM",MUTED);
        large_centered(pixels,stride,53,a,2,INK);
        fill(pixels,stride,32,102,WIDTH-64,2,SCHEME.fill);
        small_centered(pixels,stride,112,"CONVERTED VALUE",MUTED);
        const char *converted=(b[0]=='=' && b[1]==' ')?b+2:b;
        large_centered(pixels,stride,135,converted,2,ACCENT);
        break;
    }
    case TOOL_RANDOM: {
        small_centered(pixels,stride,37,b,MUTED);
        if(app->extra.random.result==0 && app->extra.random.kind==0)
            large_centered(pixels,stride,71,"--",2,ACCENT);
        else
            large_centered(pixels,stride,71,a,2,ACCENT);
        small_centered(pixels,stride,136,c,MUTED);
        fill(pixels,stride,32,166,WIDTH-64,2,SCHEME.fill);
        break;
    }
    case TOOL_DATE: {
        small_centered(pixels,stride,37,"CALCULATED DATE",MUTED);
        large_centered(pixels,stride,71,a,2,ACCENT);
        centered(pixels,stride,0,127,WIDTH,b,INK);
        small_centered(pixels,stride,164,"BASED ON Q2 LOCAL DATE",MUTED);
        break;
    }
    case TOOL_ENGINEERING: {
        small_centered(pixels,stride,36,
                 app->extra.engineering.mode==0?"OHM'S LAW":
                 app->extra.engineering.mode==1?"VOLTAGE DIVIDER":"BATTERY ESTIMATE",MUTED);
        large_centered(pixels,stride,64,a,2,ACCENT);
        draw_text_fitted(pixels,stride,22,124,WIDTH-44,b,INK);
        small_centered(pixels,stride,163,c,MUTED);
        break;
    }
    case TOOL_COUNTER: {
        const EventCounterState *e=&app->extra.counter;
        char name[32],number[28];
        snprintf(name,sizeof name,"COUNTER %c",'A'+e->selected);
        snprintf(number,sizeof number,"%d",e->values[e->selected]);
        small_centered(pixels,stride,38,
                 e->save_error?"SD CARD WRITE FAILED":name,
                 e->save_error?SCHEME.accent_red:MUTED);
        large_centered(pixels,stride,66,number,2,ACCENT);
        for(int i=0;i<4;i++) {
            int x=13+i*91, selected=i==e->selected;
            fill(pixels,stride,x,141,83,35,selected?SCHEME.fill:BG);
            if(selected)fill(pixels,stride,x,174,83,2,ACCENT);
            char label[28];snprintf(label,sizeof label,"%c %d",'A'+i,e->values[i]);
            draw_text_fitted(pixels,stride,x+5,148,73,label,
                             selected?INK:MUTED);
        }
        break;
    }
    default:break;
    }
}


/* Rasterize the timer's numerals at their final size, using 4x4 coverage.
 * A rounded segment face keeps strokes clean even on the small panel. */
/* Native-size coverage masks: never enlarge the small UI font. */
static void stopwatch_digits(uint32_t *pixels, unsigned stride, int y,
                             const char *value, uint32_t color)
{
    int height=72, width=SW72_W, total=0;
    const unsigned char *atlas=&sw72_px[0][0];
    for(int attempt=0;attempt<3;attempt++) {
        total=0;
        for(const char *p=value;*p;p++)
            total+=(*p==':'||*p=='.')?width/2:width+3;
        if(total<=WIDTH-20 || attempt==2) break;
        if(attempt==0) { height=60; width=SW60_W; atlas=&sw60_px[0][0]; }
        else { height=48; width=SW48_W; atlas=&sw48_px[0][0]; }
    }
    int x=(WIDTH-total)/2;
    y+=(72-height)/2;
    for(const char *p=value;*p;p++) {
        int index=*p>='0'&&*p<='9'?*p-'0':*p==':'?10:11;
        bool punctuation=index>=10;
        int first=punctuation?width/4:0;
        int count=punctuation?width/2:width;
        const unsigned char *glyph=atlas+index*height*width;
        for(int row=0;row<height;row++) for(int col=0;col<count;col++) {
            unsigned alpha=glyph[row*width+first+col];
            if(alpha && x+col>=0 && x+col<WIDTH && y+row<HEIGHT) {
                uint32_t *dst=&pixels[(x+col)*stride+HEIGHT-1-y-row];
                *dst=0xff000000u|(alpha==255?color:blend(*dst,color,alpha));
            }
        }
        x+=count+(punctuation?0:3);
    }
}

static void timer_digits(uint32_t *pixels,unsigned stride,int y,const char *value,uint32_t color)
{
    static const unsigned masks[]={63,6,91,79,102,109,125,7,127,111};
    static const int seg[7][4]={{8,5,28,5},{31,8,31,29},{31,35,31,56},
        {8,59,28,59},{5,35,5,56},{5,8,5,29},{8,32,28,32}};
    int x=(WIDTH-((int)strlen(value)*40-4))/2;
    for(const char *s=value;*s;s++,x+=40) {
        unsigned mask=*s>='0'&&*s<='9'?masks[*s-'0']:0;
        for(int r=0;r<65;r++) for(int c=0;c<36;c++) {
            unsigned hits=0;
            for(int sy=0;sy<4;sy++) for(int sx=0;sx<4;sx++) {
                int px=c*8+sx*2+1,py=r*8+sy*2+1;
                bool inside=false;
                if(*s==':') {
                    int dx=px-18*8,dy=py-(r<32?21:43)*8;
                    inside=dx*dx+dy*dy<=32*32;
                } else for(int k=0;k<7&&!inside;k++) if(mask&(1u<<k)) {
                    int x1=seg[k][0]*8,x2=seg[k][2]*8,y1=seg[k][1]*8,y2=seg[k][3]*8;
                    int dx=px-(px<x1?x1:px>x2?x2:px);
                    int dy=py-(py<y1?y1:py>y2?y2:py);
                    inside=dx*dx+dy*dy<=25*25;
                }
                hits+=inside;
            }
            if(hits) {
                uint32_t *dst=&pixels[(x+c)*stride+HEIGHT-1-y-r];
                *dst=0xff000000u|blend(*dst,color,hits*255/16);
            }
        }
    }
}

static void draw_timer(struct app *app,uint32_t *pixels,unsigned stride,int64_t now)
{
    CountdownState *t=&app->extra.countdown;
    if(app->timer_started && !t->running && t->remaining_ms==0) {
        app->timer_alarm=true;
        app->timer_edit=false;
        if(app->focus<3) app->focus=4;
    }
    if(app->timer_alarm || t->running || app->timer_started) {
        int64_t rem=tool_remaining(t->due_ms,t->remaining_ms,now,t->running);
        int sec=(int)((rem+999)/1000);
        char value[24];
        snprintf(value,sizeof value,"%02d:%02d:%02d",sec/3600,sec/60%60,sec%60);
        timer_digits(pixels,stride,58,value,app->timer_alarm?SCHEME.accent_red:ACCENT);
        small_centered(pixels,stride,169,app->timer_alarm?"TIME IS UP":t->running?"TIMER RUNNING":"PAUSED",INK);
        progress_bar(pixels,stride,rem,t->preset_ms);
    } else {
        int sec=(int)(t->remaining_ms/1000);
        int parts[3]={sec/3600,sec/60%60,sec%60};
        const char *units[]={"hr","min","sec"};
        fill(pixels,stride,12,101,351,37,SCHEME.fill);
        for(int i=0;i<3;i++) {
            int x=12+i*117,limit=i==0?24:60;
            char label[24];
            for(int j=-2;j<=2;j++) {
                int n=(parts[i]+j+limit)%limit;
                if(j==0) {
                    snprintf(label,sizeof label,"%02d %s",n,units[i]);
                    if(app->focus==i) paint_selection(pixels,stride,x+2,101,113,37);
                    centered(pixels,stride,x,108,117,label,app->focus==i?sel_text_color():INK);
                } else {
                    snprintf(label,sizeof label,"%02d",n);
                    uint32_t color=blend(BG,MUTED,j==1||j==-1?200:90);
                    centered(pixels,stride,x,109+j*30,117,label,color);
                }
            }
        }
        small_centered(pixels,stride,190,app->timer_edit?"Wheel adjusts":"Centre edits / Swipe",MUTED);
    }
    const char *labels[]={app->timer_alarm?"Dismiss":"Cancel",app->timer_alarm?"Done":t->running?"Pause":app->timer_started?"Resume":"Start"};
    for(int i=0;i<2;i++) {
        int x=i?205:20;
        uint32_t color=i?ACCENT:SCHEME.fill;
        /* Small rounded corners, without allocating another surface. */
        for(int row=0;row<54;row++) {
            int inset=row<6?6-row:row>=48?row-47:0;
            fill(pixels,stride,x+inset,218+row,150-2*inset,1,color);
        }
        if(app->focus==i+3) {
            fill(pixels,stride,x+9,218,132,2,INK);
            fill(pixels,stride,x+9,270,132,2,INK);
        }
        centered(pixels,stride,x,234,150,labels[i],i?0xffffffu:INK);
    }
}



/* Fixed-point dial geometry: no libm or per-frame heap allocations. */
static const int watch_sin[60]={0,107,213,316,416,512,602,685,761,828,887,935,974,1002,1018,1024,1018,1002,974,935,887,828,761,685,602,512,416,316,213,107,0,-107,-213,-316,-416,-512,-602,-685,-761,-828,-887,-935,-974,-1002,-1018,-1024,-1018,-1002,-974,-935,-887,-828,-761,-685,-602,-512,-416,-316,-213,-107};
static void watch_point(int cx,int cy,int r,int tick,int *x,int *y)
{
    tick=(tick%60+60)%60;
    *x=cx+r*watch_sin[tick]/1024;
    *y=cy-r*watch_sin[(tick+15)%60]/1024;
}
static void watch_hand(uint32_t *pixels,unsigned stride,int cx,int cy,int r,int tick,uint32_t color)
{
    int x,y;
    watch_point(cx,cy,r,tick,&x,&y);
    segment(pixels,stride,cx,cy,x,y,color);
    segment(pixels,stride,cx+1,cy,x+1,y,color);
    disc(pixels,stride,cx,cy,3,color);
}
static void watch_dial(uint32_t *pixels,unsigned stride,int cx,int cy,int r,int tick,bool big)
{
    uint32_t rim=blend(BG,INK,95),face=blend(BG,INK,16);
    disc(pixels,stride,cx,cy,r,rim);
    disc(pixels,stride,cx,cy,r-1,face);
    for(int i=0;i<60;i++) {
        if(!big && i%5) continue;
        int x,y,x2,y2;
        watch_point(cx,cy,r-4,i,&x,&y);
        watch_point(cx,cy,r-(i%5?8:13),i,&x2,&y2);
        segment(pixels,stride,x,y,x2,y2,i%5?MUTED:INK);
        if(big && i%10==0) {
            char label[8];
            snprintf(label,sizeof label,"%d",i?i:60);
            watch_point(cx,cy,r-24,i,&x,&y);
            centered(pixels,stride,x-15,y-11,30,label,INK);
        }
    }
    watch_hand(pixels,stride,cx,cy,r-17,tick,0xff942b);
}
static void watch_controls(struct app *app,uint32_t *pixels,unsigned stride)
{
    int cy=282;
    uint32_t action=app->running?0x932c32:0x14633c;
    uint32_t symbol=app->running?0xffb9bb:0x65ffac;
    disc(pixels,stride,64,cy,26,0x42464c);
    disc(pixels,stride,311,cy,26,action);
    if(app->running) {
        centered(pixels,stride,20,cy-11,88,"Lap",0xffffff);
        fill(pixels,stride,304,cy-9,5,18,symbol);
        fill(pixels,stride,313,cy-9,5,18,symbol);
    } else {
        centered(pixels,stride,20,cy-11,88,"Reset",0xffffff);
        /* Point the triangle to the right. */
        for(int x=0;x<17;x++) fill(pixels,stride,305+x,cy-(17-x)/2,1,17-x,symbol);
    }
    const char *modes[]={"Digital","Analog","Hybrid"};
    small_centered(pixels,stride,263,modes[app->stopwatch_mode],MUTED);
    for(int i=0;i<3;i++) disc(pixels,stride,177+i*10,292,3,i==app->stopwatch_mode?INK:MUTED);
}
static void draw_stopwatch(struct app *app,uint32_t *pixels,unsigned stride,int64_t now)
{
    int64_t ms=elapsed(app,now);
    char value[64];
    format_elapsed(value,sizeof value,ms);
    if(app->stopwatch_mode==1) {
        watch_dial(pixels,stride,187,139,106,(int)(ms/1000%60),true);
        watch_hand(pixels,stride,187,139,49,(int)(ms/60000%60),MUTED);
        centered(pixels,stride,110,177,154,value,INK);
    } else if(app->stopwatch_mode==2) {
        watch_dial(pixels,stride,78,78,37,(int)(ms/3600000%60),false);
        watch_dial(pixels,stride,187,78,37,(int)(ms/60000%60),false);
        watch_dial(pixels,stride,296,78,37,(int)(ms/1000%60),false);
        stopwatch_digits(pixels,stride,120,value,INK);
        fill(pixels,stride,30,219,315,1,MUTED);
        int lastx=0,lasty=0;
        unsigned count=app->lap_count<LAP_LIMIT?app->lap_count:LAP_LIMIT;
        int64_t max=1;
        for(unsigned i=0;i<count;i++) {
            int64_t v=app->laps[i]-(i+1<count?app->laps[i+1]:0);
            if(v>max) max=v;
        }
        for(unsigned i=0;i<count;i++) {
            int64_t v=app->laps[i]-(i+1<count?app->laps[i+1]:0);
            int x=46+(int)i*92,y=245-(int)(v*30/max);
            if(lastx) segment(pixels,stride,lastx,lasty,x,y,0xff942b);
            disc(pixels,stride,x,y,3,0xff942b);
            lastx=x;lasty=y;
        }
    } else {
        stopwatch_digits(pixels,stride,app->lap_count?40:84,value,INK);
        unsigned count=app->lap_count<LAP_LIMIT?app->lap_count:LAP_LIMIT;
        for(unsigned i=0;i<count;i++) {
            char lap[48],num[32];
            int64_t v=app->laps[i]-(i+1<count?app->laps[i+1]:0);
            format_elapsed(num,sizeof num,v);
            snprintf(lap,sizeof lap,"Lap %u",app->lap_count-i);
            int y=122+(int)i*30;
            fill(pixels,stride,20,y,335,1,blend(BG,INK,55));
            draw_text(pixels,stride,25,y+4,lap,INK);
            draw_text(pixels,stride,350-text_width(num),y+4,num,INK);
        }
    }
    watch_controls(app,pixels,stride);
}


/* Rounded coverage masks use four subpixel samples per axis at the edge.
 * The interior uses ordinary spans; no temporary framebuffer is needed. */
static unsigned clip_round_alpha(int x,int y,int w,int h,int radius)
{
    if(x<0||y<0||x>=w||y>=h) return 0;
    int cx=x<radius?radius:x>=w-radius?w-radius:-1;
    int cy=y<radius?radius:y>=h-radius?h-radius:-1;
    if(cx<0||cy<0) return 255;
    int hits=0;
    for(int sy=0;sy<4;sy++) for(int sx=0;sx<4;sx++) {
        int dx=x*8+sx*2+1-cx*8,dy=y*8+sy*2+1-cy*8;
        hits+=dx*dx+dy*dy<=radius*radius*64;
    }
    return (unsigned)(hits*255/16);
}
static void clip_pixel(uint32_t *pixels,unsigned stride,int x,int y,uint32_t color,unsigned alpha)
{
    if(!alpha||x<0||x>=WIDTH||y<0||y>=HEIGHT) return;
    uint32_t *dst=&pixels[x*stride+HEIGHT-1-y];
    *dst=0xff000000u|(alpha==255?color:blend(*dst,color,alpha));
}
static void clip_round(uint32_t *pixels,unsigned stride,int x,int y,int w,int h,int radius,uint32_t color)
{
    for(int row=0;row<h;row++) for(int col=0;col<w;col++)
        clip_pixel(pixels,stride,x+col,y+row,color,clip_round_alpha(col,row,w,h,radius));
}

static void draw_clip(struct app *app,uint32_t *pixels,unsigned stride)
{
    char label[96],title[80];
    fit_text(title,sizeof title,app->clip_title,WIDTH-24,false);
    centered(pixels,stride,0,39,WIDTH,title,INK);
    if(app->clip_duration<5) {
        small_centered(pixels,stride,75,"Song unavailable or shorter than 5s",MUTED);
        return;
    }
    snprintf(label,sizeof label,"%d:%02d",app->clip_duration/60,app->clip_duration%60);
    small_centered(pixels,stride,65,label,MUTED);
    int left=15+(int)((int64_t)345*app->clip_start/app->clip_duration);
    int right=15+(int)((int64_t)345*(app->clip_start+app->clip_length)/app->clip_duration);
    if(right-left<3) right=left+3;
    if(right>360) { right=360; if(left>357) left=357; }
    const uint32_t paper=0x354e61, selected=0x68b4e8;
    /* Both regions share the same rounded mask, including at song ends. */
    for(int y=0;y<112;y++) for(int x=0;x<345;x++) {
        unsigned alpha=clip_round_alpha(x,y,345,112,15);
        int px=x+15;
        clip_pixel(pixels,stride,px,y+87,px>=left&&px<right?selected:paper,alpha);
    }
    if(app->clip_wave_ready) {
        for(int i=0;i<75;i++) {
            int x=27+i*321/75;
            unsigned peak=0;
            int first=i*320/75,last=(i+1)*320/75;
            for(int j=first;j<last;j++) if(app->clip_wave[j]>peak) peak=app->clip_wave[j];
            int height=3+(int)peak*86/255;
            clip_round(pixels,stride,x,143-height/2,3,height,1,
                       x+1>=left&&x+1<right?0xf1faff:0x91a9b8);
        }
    } else small_centered(pixels,stride,135,"Waveform unavailable",0xe0edf5);
    for(int y=89;y<197;y++) {
        unsigned l=clip_round_alpha(left-15,y-87,345,112,15);
        unsigned r=clip_round_alpha(right-1-15,y-87,345,112,15);
        clip_pixel(pixels,stride,left,y,0xd9f2ff,l);
        clip_pixel(pixels,stride,right-1,y,0xd9f2ff,r);
    }
    int lx=clip_clamp(left-4,15,350),rx=clip_clamp(right-5,15,350);
    /* Separate vertically when a very short selection makes handles overlap. */
    int ly=129,ry=129;
    if(rx-lx<10) { ly=111; ry=151; }
    clip_round(pixels,stride,lx,ly,10,28,5,0xffffff);
    clip_round(pixels,stride,rx,ry,10,28,5,0xffffff);
    if(app->clip_focus==0)
        small_centered(pixels,stride,202,app->clip_edit?"Wheel moves selection":"",MUTED);
    else if(app->clip_focus<3)
        fill(pixels,stride,app->clip_focus==2?210:15,231,150,2,app->clip_edit?0x68b4e8:MUTED);
    snprintf(label,sizeof label,"Start %d:%02d",app->clip_start/60,app->clip_start%60);
    draw_text_fitted(pixels,stride,15,209,160,label,INK);
    int end=app->clip_start+app->clip_length;
    snprintf(label,sizeof label,"End %d:%02d",end/60,end%60);
    draw_text_fitted(pixels,stride,210,209,150,label,INK);
    snprintf(label,sizeof label,"%d seconds%s",app->clip_length,app->clip_edit?"  /  Wheel adjusts":"");
    small_centered(pixels,stride,233,app->clip_status<0?(app->clip_error[0]?app->clip_error:"Audio operation failed"):app->clip_status==1?"Preview requested":app->clip_status==2?"Preparing audio...":label,ACCENT);
    for(int i=0;i<2;i++) {
        int x=15+i*177;
        uint32_t color=i?0x68b4e8:0x30363d;
        if(app->clip_focus==i+3) clip_round(pixels,stride,x-2,251,172,47,13,0x80baff);
        clip_round(pixels,stride,x,253,168,43,11,color);
        if(!i) for(int j=0;j<10;j++) fill(pixels,stride,x+30+j,274-(10-j)/2,1,10-j,0xffffff);
        centered(pixels,stride,x+(i?0:12),263,168-(i?0:12),i?"Save MP3":"Preview",i?0x122536:0xffffff);
    }
}
void app_draw(struct app *app, uint32_t *pixels, unsigned stride, int64_t now, time_t utc, const struct tm *today)
{
    extra_tick(&app->extra,now);
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
    else if(app->screen==RINGTONE)
        snprintf(title,sizeof title,"Edit Alarm Sound");
    else
        snprintf(title, sizeof title, "%s", titles[app->screen]);
    if (app->screen == EDIT_TASK && app->draft[0])
        snprintf(title, sizeof title, "New task  %u/%u",
                 (unsigned)strlen(app->draft) % 1000u, (unsigned)TASK_LENGTH);
    if (SCHEME.caps_bar)
        upper(title);
    centered(pixels, stride, 0, 3, WIDTH, title, INK);
    char value[64];
    if(app->screen==RINGTONE) { draw_clip(app,pixels,stride); return; }
    if(app->screen==COUNTDOWN) { draw_timer(app,pixels,stride,now); return; }

    switch (app->screen) {
    case POMODORO: case METRONOME: case COUNTDOWN: case CONVERTER:
    case RANDOMIZER: case DATE_CALC: case ENGINEERING: case EVENT_COUNTER: {
        ToolKind kind=app->screen==POMODORO?TOOL_POMODORO:
                      app->screen==METRONOME?TOOL_METRONOME:
                      app->screen==COUNTDOWN?TOOL_COUNTDOWN:
                      app->screen==CONVERTER?TOOL_CONVERTER:
                      app->screen==RANDOMIZER?TOOL_RANDOM:
                      app->screen==DATE_CALC?TOOL_DATE:
                      app->screen==ENGINEERING?TOOL_ENGINEERING:TOOL_COUNTER;
        char first[96],second[96],third[96];
        extra_display(&app->extra,kind,now,today,first,sizeof first,
                      second,sizeof second,third,sizeof third);
        if(kind==TOOL_METRONOME) {
            /* The classic pyramid replaces the text lines and the old
             * beat dots: the swinging pendulum is the visual beat. */
            (void)second;
            draw_metronome(app,pixels,stride,now,first,third);
        } else {
            tool_hero(app,pixels,stride,now,today);
        }
        break;
    }
    case STOPWATCH:
        draw_stopwatch(app,pixels,stride,now);
        return;
    case CALENDAR:
        draw_calendar(app, pixels, stride, today);
        break;
    case TODO:
        if (!app->task_count)
            draw_text(pixels, stride, 18, 44, "No tasks yet", MUTED);
        break;
    case TASK_ACTIONS: {
        char lines[TASK_LINES][TASK_LENGTH + 4];
        int count = wrap_lines(app->tasks[app->task_index].text, WIDTH - 40, lines, TASK_LINES);
        for (int i = 0; i < count; ++i)
            draw_text(pixels, stride, 20, TASK_TEXT_Y + i * TASK_LINE_H, lines[i], INK);
        break;
    }
    case EDIT_TASK: {
        /* The field: long names keep their end, where typing happens, in
         * view; the caret follows the text, or underlines the letter a
         * multi-tap can still change. */
        int pending = t9_pending(app, now);
        char shown[TASK_LENGTH + 8];
        fit_text(shown, sizeof shown, app->draft[0] ? app->draft : "Task name", 327, true);
        fill(pixels, stride, KB_X, 32, WIDTH - 2 * KB_X, 30, PANEL);
        draw_text_fitted(pixels, stride, 20, 36, 327, shown, app->draft[0] ? INK : MUTED);
        int caret = 20 + (app->draft[0] ? text_width(shown) : 0);
        if (pending >= 0 && app->draft[0]) {
            int w = f20_adv[glyph((unsigned char)shown[strlen(shown) - 1])];
            fill(pixels, stride, caret - w, 57, w, 2, ACCENT);
        } else
            fill(pixels, stride, caret + 1, 36, 2, 22, ACCENT);
        /* The open key's letters, the one in the text selected. */
        if (pending >= 0) {
            char set[8];
            t9_letters(app, pending, set, sizeof set);
            for (int i = 0; set[i]; ++i) {
                char one[2] = {set[i], 0};
                int x = 20 + i * 26;
                bool on = i == app->t9_tap;
                if (on)
                    paint_selection(pixels, stride, x, 65, 24, 24);
                centered(pixels, stride, x, 66, 24, one, on ? sel_text_color() : INK);
            }
        }
        break;
    }
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
    case RINGTONE: case HOME: case TIME_GROUP: case TOOL_GROUP:
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
        bool list = app->screen == HOME || app->screen == TIME_GROUP ||
                    app->screen == TOOL_GROUP || app->screen == TASK_ACTIONS ||
                    (app->screen == TODO && item->code >= 0);
        if (list) {
            /* System list rows: theme selection painter, theme text
             * edge, chevron on the selected row only. */
            paint_list_row(pixels, stride, item, selected);
            char full[sizeof item->label], label[sizeof item->label];
            snprintf(full, sizeof full, "%s", item->label);
            if (SCHEME.caps_rows)
                upper(full);
            uint32_t color = selected ? sel_text_color() : INK;
            const bool with_icon=app->screen==HOME || app->screen==TIME_GROUP ||
                                 app->screen==TOOL_GROUP;
            int label_x=SCHEME.text_x;
            if(with_icon) {
                draw_font_icon(pixels,stride,label_x,y+2,
                               menu_icon((enum screen)item->code),color);
                label_x+=19;
            }
            /* Task names (up to TASK_LENGTH) end in "..." rather than
             * squeezing; menu labels keep draw_text_fitted's narrowing. */
            if (app->screen == TODO)
                fit_text(label, sizeof label, full, WIDTH - label_x - 40, false);
            else
                snprintf(label, sizeof label, "%s", full);
            draw_text_fitted(pixels,stride,label_x,y,
                             WIDTH-label_x-40,label,color);
            if (selected)
                draw_text(pixels, stride, 322, y, ">",
                          SCHEME.painter == Q2POD_SEL_CARD ? SCHEME.chevron
                                                           : sel_text_color());
        } else {
            /* Dedicated tool controls: a clear primary action and calm
             * secondary keys, preserving Q2 Pod's selection painter. */
            bool tool=app->screen>=POMODORO && app->screen<=EVENT_COUNTER &&
                      app->screen!=METRONOME; /* Preserve PR #9 controls. */
            uint32_t color=selected?sel_text_color():INK;
            if(selected)
                paint_selection(pixels,stride,item->x,item->y,item->w,item->h);
            else if(app->screen==EDIT_TASK) {
                /* T9 keys, as Q2's: letter keys light, function keys
                 * the theme's darker fill, Done the accent. */
                if(item->code==T9_DONE) {
                    fill(pixels,stride,item->x,item->y,item->w,item->h,SCHEME.accent_top);
                    color=0xffffffu;
                } else if(t9_function(item->code)) {
                    /* Q2's slate function keys, white-lettered. */
                    bool light=(BG&0xffu)>0x80u;
                    fill(pixels,stride,item->x,item->y,item->w,item->h,
                         light?0x7e8693u:0x3a3d42u);
                    color=0xffffffu;
                } else {
                    fill(pixels,stride,item->x,item->y,item->w,item->h,PANEL);
                    uint32_t edge=SCHEME.paper==0?0x414141u:0xc6c6c6u;
                    outline(pixels,stride,item->x,item->y,item->w,item->h,edge);
                }
            } else if(tool) {
                if(item->code==0 || (app->screen==DATE_CALC && item->code==4)) {
                    fill(pixels,stride,item->x,item->y,item->w,item->h,
                         SCHEME.accent_top);
                    color=0xffffffu;
                } else {
                    fill(pixels,stride,item->x,item->y,item->w,item->h,SCHEME.fill);
                    uint32_t edge=SCHEME.paper==0?0x414141u:0xc6c6c6u;
                    outline(pixels,stride,item->x,item->y,item->w,item->h,edge);
                }
            } else
                fill(pixels,stride,item->x,item->y,item->w,item->h,PANEL);
            /* Some labels (e.g. +/-10 BPM) are wider than the four-key
             * secondary strips. Fit those labels without clipping. */
            int label_x=item->x+(item->w-text_width(item->label))/2;
            if(label_x<item->x+4)label_x=item->x+4;
            if(tool && item->y>=250)
                small_line(pixels,stride,item->x+3,
                           item->y+(item->h-15)/2,item->w-6,
                           item->label,color);
            else
                draw_text_fitted(pixels,stride,label_x,y,
                                 item->w-(label_x-item->x)-4,item->label,color);
        }
    }
}
