/* SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct app app;
static uint32_t pixels[WIDTH * HEIGHT];
static const struct tm today = {.tm_year = 124, .tm_mon = 1, .tm_mday = 29};
/* 2024-02-29 12:00 UTC, the same day as the `today` fixture. */
static const time_t UTC = 1709208000;

static void send(const char *message, int64_t now)
{
    app_input(&app, message, now, &today);
    app_draw(&app, pixels, HEIGHT, now, UTC, &today);
}

static void tap(int x, int y, int64_t now)
{
    char message[40];
    snprintf(message, sizeof message, "t d %d %d", x, y);
    send(message, now);
    snprintf(message, sizeof message, "t u %d %d", x, y);
    send(message, now);
}

static void back(void)
{
    send("k u 170", 5000);
}

static void stopwatch(void)
{
    send("k u 218", 0);
    assert(app.screen == STOPWATCH);
    /* Buttons: y=190..235. */
    tap(55, 210, 100);
    tap(174, 210, 1600);
    tap(55, 210, 2400);
    assert(!app.running && app.elapsed == 2300 && app.laps[0] == 1500);
    /* The 4x readout's digits reach y=100..110; the old 18 px text ended near y=96. */
    uint32_t paper = pixels[2 * HEIGHT + HEIGHT - 1 - 120];
    int ink = 0;
    for (int y = 100; y < 110; ++y)
        for (int x = 0; x < WIDTH; ++x)
            ink += pixels[x * HEIGHT + HEIGHT - 1 - y] != paper;
    assert(ink > 50);
    tap(55, 210, 3000);
    tap(55, 210, 3200);
    assert(app.elapsed == 2500);
    tap(282, 210, 4000);
    assert(!app.elapsed && !app.lap_count);
    back();
}

static void calendar(void)
{
    tap(50, 106, 0); /* Calendar row: y=90..121 */
    assert(app.screen == CALENDAR && app.year == 2024 && app.month == 1);
    /* Minimal dark: today is the card fill (0x2b2b2b) in its new grid cell. */
    assert(pixels[235 * HEIGHT + HEIGHT - 1 - 176] == 0xff2b2b2b);
    for (int i = 0; i < 10; ++i)
        tap(50, 252, 0);
    assert(app.year == 2023 && app.month == 3);
    tap(187, 252, 0);
    assert(app.year == 2024 && app.month == 1);
    back();
}

static void wheel(void)
{
    send("w -27", 0);
    assert(app.focus == 0);
    send("w -1", 10);
    assert(app.focus == 1);
    send("w -27", 20);
    send("w 28", 30);
    assert(app.focus == 0);
    send("w 28", 40);
    assert(app.focus == 0); /* Pins at the first row. */

    app_wheel_config(&app, "[IPOD]\nWHEELSENSITIVITY=10\n[Q2POD]\r\nWHEELSENSITIVITY=160\r\n");
    send("w -8", 50);
    assert(app.focus == 0);
    send("w -1", 60);
    assert(app.focus == 1);
    send("w -26", 70); /* 35 units total, exactly four ticks at 160%. */
    assert(app.focus == 4);
    send("w -200", 80);
    assert(app.focus == 4); /* Pins at the last visible row. */

    app_wheel_config(&app, "[Q2POD]\nWHEELSENSITIVITY=1600\n");
    send("w 27", 90);
    send("w 1", 1090); /* Old residual travel expires. */
    assert(app.focus == 4);
    send("w 27", 1100);
    assert(app.focus == 3);
    send("w 200", 1110);
    assert(app.focus == 0);
}

static void worldclock(void)
{
    tap(50, 74, 0); /* World Clock row: y=58..89 */
    assert(app.screen == WORLD_CLOCK && app.focus == 0 && app.wc_top == 0);
    /* Classic Blue: gloss band top line on the selected 52 px row. */
    app_theme("[IPOD]\nTHEME=1\nCLASSIC_LIGHT_ACCENT=0\nLIGHT=1\n", 12);
    app_draw(&app, pixels, HEIGHT, 0, UTC, &today);
    assert(pixels[20 * HEIGHT + HEIGHT - 1 - 26] == 0xff94c7de);
    send("w -280", 0); /* 10 steps down the 16-city list */
    assert(app.focus == 10 && app.wc_top == 6);
    send("w 280", 0);
    assert(app.focus == 0 && app.wc_top == 0);
    tap(50, 42, 0); /* first city row -> detail (Los Angeles) */
    assert(app.screen == WC_DETAIL && app.wc_selected == 0);
    back(); /* -> WORLD_CLOCK */
    assert(app.screen == WORLD_CLOCK);
    /* World map footer button: y=286..319. Opens on Los Angeles. */
    tap(187, 303, 0);
    assert(app.screen == WC_MAP && app.wc_map == 1);
    send("w -28", 0); /* one city east in longitude order: LAX -> DEN */
    assert(app.wc_map == 2);
    tap(328, 88, 0); /* Tokyo on the map: lon 139.7, lat 35.7 */
    assert(app.wc_map == 13);
    /* The wheel walks every time zone: 2 more east to Auckland, a full
     * sweep west end to end to Honolulu, and back east to Tokyo. */
    send("w -56", 0);
    assert(app.wc_map == 15);
    send("w 420", 0);
    assert(app.wc_map == 0);
    send("w -364", 0);
    assert(app.wc_map == 13);
    send("k u 218", 0); /* Centre -> Tokyo detail */
    assert(app.screen == WC_DETAIL && app.wc_selected == 13);
    back(); /* -> WORLD_CLOCK, focused on Tokyo */
    assert(app.screen == WORLD_CLOCK && app.focus == 13);
    back(); /* -> HOME */
    assert(app.screen == HOME);
}

/* T9 keypad key k: 4x4 from (12, 92), 86x52 keys, 2 px / 3 px apart.
 * 0 ,  1 abc  2 def  3 DEL / 4 ghi 5 jkl 6 mno 7 ABC /
 * 8 pqrs 9 tuv 10 wxyz 11 123 / 12 .?! 13 space 14 Done 15 CLR */
static void key(int k, int64_t now)
{
    tap(12 + k % 4 * 88 + 43, 92 + k / 4 * 55 + 26, now);
}

static void t9(void)
{
    tap(50, 138, 0); /* To-do row */
    tap(50, 250, 0); /* + New task */
    assert(app.screen == EDIT_TASK && app.focus == 1); /* wheel on abc */
    key(1, 1000);
    key(1, 1500);           /* multi-tap: a -> b */
    assert(!strcmp(app.draft, "b"));
    key(1, 3000);           /* after the 1.2 s pause: a new letter */
    assert(!strcmp(app.draft, "ba"));
    key(2, 3100);           /* another key: a new letter at once */
    key(2, 3200);
    key(2, 3300);
    key(2, 3400);           /* d e f d: wraps round */
    assert(!strcmp(app.draft, "bad"));
    key(7, 5000);           /* ABC */
    key(4, 5100);
    assert(!strcmp(app.draft, "badG"));
    key(11, 6000);          /* 123: each press is a new digit */
    key(4, 6100);
    key(4, 6200);
    assert(!strcmp(app.draft, "badG44"));
    key(13, 7000);          /* space */
    key(3, 7100);           /* DEL */
    key(3, 7200);
    assert(!strcmp(app.draft, "badG4"));
    key(15, 8000);          /* CLR */
    assert(!app.draft[0]);
    key(14, 8100);          /* Done on an empty name keeps the editor */
    assert(app.screen == EDIT_TASK);
    back();
    assert(app.screen == TODO);
    back();
}

static void tasks(const char *path)
{
    tap(50, 138, 0); /* To-do row: y=122..153 */
    tap(50, 250, 0);
    assert(app.screen == EDIT_TASK);
    key(7, 0);  /* ABC */
    key(1, 0);  /* A */
    key(14, 0); /* Done */
    assert(app.screen == TODO && app.task_count == 1);
    tap(50, 42, 0); /* task row 0: y=26..57 */
    tap(50, 82, 0);  /* Mark done row: y=66..97 */
    app_init(&app, path);
    assert(app.task_count == 1 && app.tasks[0].done && !strcmp(app.tasks[0].text, "A"));

    tap(50, 138, 0);
    tap(50, 42, 0);
    tap(50, 114, 0); /* Delete task row: y=98..129 */
    app_init(&app, path);
    assert(app.task_count == 0);

    /* Names hold TASK_LENGTH (140) characters; the 141st is ignored,
     * and the full name survives a save and reload. */
    tap(50, 138, 0);
    tap(50, 250, 0);
    key(11, 0); /* 123: one "2" per press */
    for (int i = 0; i <= TASK_LENGTH; ++i)
        key(1, 0);
    assert(strlen(app.draft) == TASK_LENGTH);
    key(14, 0); /* Done */
    app_init(&app, path);
    assert(app.task_count == 1 && strlen(app.tasks[0].text) == TASK_LENGTH);
    /* 140 "2"s (10 px each) wrap onto five 335 px lines, so the options
     * rows start at y=34+5*22+8=152: Mark done 152..183, Delete 184..215. */
    tap(50, 138, 0);
    tap(50, 42, 0);
    assert(app.screen == TASK_ACTIONS);
    tap(50, 168, 0);
    assert(app.tasks[0].done);
    tap(50, 42, 0);
    tap(50, 200, 0);
    app_init(&app, path);
    assert(app.task_count == 0);
}

static void expanded_utilities(void)
{
    /* Unfinished categories are hidden on HOME but implementation stays
     * testable by entering their screens directly. */
    tap(50,203,0);
    assert(app.screen==HOME);
    tap(50,235,0);
    assert(app.screen==HOME);
    app.screen=TIME_GROUP;
    app.focus=0;
    tap(50,42,0);
    assert(app.screen==POMODORO);
    tap(50,212,1000);
    assert(app.extra.pomo.running);
    back();assert(app.screen==TIME_GROUP);
    tap(50,78,5000);
    assert(app.screen==METRONOME);
    tap(70,240,6000); /* Start: metronome keys sit at y=226..253 now */
    assert(app.extra.metro.running);
    back();assert(app.screen==TIME_GROUP);
    tap(50,114,7000);
    assert(app.screen==COUNTDOWN);
    tap(50,212,7000);
    assert(app.extra.countdown.running);
    back();assert(app.screen==TIME_GROUP);
    back();assert(app.screen==HOME);

    app.screen=TOOL_GROUP;
    app.focus=0;
    static const enum screen types[]={CONVERTER,RANDOMIZER,DATE_CALC,ENGINEERING,EVENT_COUNTER};
    for(int i=0;i<5;i++) {
        tap(50,43+i*36,9000+i);
        assert(app.screen==types[i]);
        if(app.screen==EVENT_COUNTER) {
            tap(50,212,10000); /* +1 */
            assert(app.extra.counter.values[0]==1 && !app.extra.counter.save_error);
        }
        back();
        assert(app.screen==TOOL_GROUP);
    }
    back();assert(app.screen==HOME);
}

static bool gold_near(int cx, int cy, int r)
{
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            uint32_t p = pixels[x * HEIGHT + HEIGHT - 1 - y];
            if ((p >> 16 & 255) > 180 && (p >> 8 & 255) > 120 && (p & 255) < 110)
                return true;
        }
    return false;
}

static void metronome_classic(void)
{
    /* Classic pyramid in classic-light: at rest the rod hangs
     * vertical on the centre line, the face is blue-tinted, the base
     * is dark, and the weight sits on the 120 BPM mark (y=127). */
    app_theme("[IPOD]\nTHEME=1\nCLASSIC_LIGHT_ACCENT=0\nLIGHT=1\n", 12);
    app.screen = TIME_GROUP; /* Hidden menu; verify retained metronome screen. */
    app.focus = 0;
    tap(50, 78, 5000); /* Metronome row */
    assert(app.screen == METRONOME);
    /* expanded_utilities() above left it running; stop it so the
     * at-rest frame really is at rest. */
    if (app.extra.metro.running)
        tap(70, 240, 5000);
    assert(!app.extra.metro.running);
    app_draw(&app, pixels, HEIGHT, 5000, UTC, &today);
    assert(gold_near(187, 120, 4));
    assert(gold_near(187, 127, 10)); /* weight on the 120 BPM mark */
    uint32_t face = pixels[160 * HEIGHT + HEIGHT - 1 - 100];
    assert((face & 255) > (face >> 16 & 255) + 20); /* blue, not grey */
    uint32_t base = pixels[140 * HEIGHT + HEIGHT - 1 - 190];
    assert((base >> 16 & 255) < 80 && (base >> 8 & 255) < 80 && (base & 255) < 80);
    tap(70, 240, 6000); /* Start */
    assert(app.extra.metro.running);
    /* 125 ms into the 500 ms beat: pendulum at the right extreme. */
    app_draw(&app, pixels, HEIGHT, 6125, UTC, &today);
    assert(gold_near(237, 80, 5));
    assert(!gold_near(137, 80, 5));
    /* 375 ms in: left extreme. */
    app_draw(&app, pixels, HEIGHT, 6375, UTC, &today);
    assert(gold_near(137, 80, 5));
    assert(!gold_near(237, 80, 5));
    tap(70, 240, 7000); /* Stop */
    assert(!app.extra.metro.running);
    back(); /* -> TIME_GROUP */
    assert(app.screen == TIME_GROUP);
    back(); /* -> HOME */
    assert(app.screen == HOME);
}

static void calculator(void)
{
    tap(50, 170, 0); /* Calculator row: y=154..185 */
    tap(50, 105, 0);  /* 7 */
    tap(300, 105, 0); /* / */
    tap(50, 180, 0);  /* 1 */
    tap(210, 225, 0); /* = */
    assert(!strcmp(app.number, "7"));
    tap(300, 105, 0); /* / */
    tap(50, 225, 0);  /* 0 */
    tap(210, 225, 0); /* = */
    assert(app.error && !strcmp(app.number, "Divide by zero"));
    tap(50, 265, 0);  /* C */
    assert(!app.error && !strcmp(app.number, "0"));
    back();
    back();
    assert(app.quit);
}

static void theme(void)
{
    app.screen=HOME;
    app.focus=0;
    app_theme("[OTHER]\nLIGHT=0\n[IPOD]\r\nTHEME=2\r\nACCENT=4\r\nLIGHT=1\r\n", 12);
    app_draw(&app,pixels,HEIGHT,0,UTC,&today);
    assert(pixels[0]==0xfff4f4f4);
    /* Modern light: selected HOME row is the flat Cobalt bar (y=26..57). */
    assert(pixels[20*HEIGHT+HEIGHT-1-42]==0xff2a6ad8);
    /* Modern light bar is flat 0xdedede at the top. */
    assert(pixels[187*HEIGHT+HEIGHT-1]==0xffdedede);
    app_theme("[IPOD]\nTHEME=1\nCLASSIC_LIGHT_ACCENT=0\nLIGHT=2\n",7);
    app_draw(&app,pixels,HEIGHT,0,UTC,&today);
    assert(pixels[0]==0xffffffff);
    /* Classic Blue: gloss bar on top, gloss band top/bottom lines on the
     * selected row (y=26 top line, y=57 bottom line). */
    assert(pixels[187*HEIGHT+HEIGHT-1]==0xffffffff);
    assert(pixels[20*HEIGHT+HEIGHT-1-26]==0xff94c7de);
    assert(pixels[20*HEIGHT+HEIGHT-1-57]==0xff004184);
    app_theme("[IPOD]\nLIGHT=2\n",19);
    app_draw(&app,pixels,HEIGHT,0,UTC,&today);
    assert(pixels[0]==0xff000000);
    app_theme("",12);
}

int main(void)
{
    char dir[] = "/tmp/q2-utilities-test.XXXXXX";
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof path, "%s/tasks.tsv", dir);
    app_init(&app, path);
    theme();
    wheel();
    stopwatch();
    t9();
    calendar();
    worldclock();
    tasks(path);
    expanded_utilities();
    metronome_classic();
    calculator();
    assert(unlink(path) == 0);
    char counter_path[160];
    snprintf(counter_path,sizeof counter_path,"%s/counters.tsv",dir);
    assert(unlink(counter_path) == 0);
    assert(rmdir(dir) == 0);
    puts("Utilities checks passed");
    return 0;
}
