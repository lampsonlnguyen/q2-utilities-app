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
    tap(55, 140, 100);
    tap(174, 140, 1600);
    tap(55, 140, 2400);
    assert(!app.running && app.elapsed == 2300 && app.laps[0] == 1500);
    tap(55, 140, 3000);
    tap(55, 140, 3200);
    assert(app.elapsed == 2500);
    tap(282, 140, 4000);
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

static void worldclock(void)
{
    tap(50, 74, 0); /* World Clock row: y=58..89 */
    assert(app.screen == WORLD_CLOCK && app.focus == 0 && app.wc_top == 0);
    /* Classic Blue: gloss band top line on the selected 52 px row. */
    app_theme("[IPOD]\nTHEME=1\nCLASSIC_LIGHT_ACCENT=0\nLIGHT=1\n", 12);
    app_draw(&app, pixels, HEIGHT, 0, UTC, &today);
    assert(pixels[20 * HEIGHT + HEIGHT - 1 - 26] == 0xff94c7de);
    send("w -90", 0); /* 10 steps down the 16-city list */
    assert(app.focus == 10 && app.wc_top == 6);
    send("w 90", 0);
    assert(app.focus == 0 && app.wc_top == 0);
    tap(50, 42, 0); /* first city row -> detail (Los Angeles) */
    assert(app.screen == WC_DETAIL && app.wc_selected == 0);
    back(); /* -> WORLD_CLOCK */
    assert(app.screen == WORLD_CLOCK);
    /* World map footer button: y=286..319. Opens on Los Angeles. */
    tap(187, 303, 0);
    assert(app.screen == WC_MAP && app.wc_map == 1);
    send("w -9", 0); /* one city east in longitude order: LAX -> DEN */
    assert(app.wc_map == 2);
    tap(328, 88, 0); /* Tokyo on the map: lon 139.7, lat 35.7 */
    assert(app.wc_map == 13);
    send("k u 218", 0); /* Centre -> Tokyo detail */
    assert(app.screen == WC_DETAIL && app.wc_selected == 13);
    back(); /* -> WORLD_CLOCK, focused on Tokyo */
    assert(app.screen == WORLD_CLOCK && app.focus == 13);
    back(); /* -> HOME */
    assert(app.screen == HOME);
}

static void tasks(const char *path)
{
    tap(50, 138, 0); /* To-do row: y=122..153 */
    tap(50, 250, 0);
    assert(app.screen == EDIT_TASK);
    tap(25, 105, 0); /* A */
    tap(295, 250, 0);
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
    stopwatch();
    calendar();
    worldclock();
    tasks(path);
    calculator();
    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0);
    puts("Utilities checks passed");
    return 0;
}
