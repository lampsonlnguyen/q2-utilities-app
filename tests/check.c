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

static void send(const char *message, int64_t now)
{
    app_input(&app, message, now, &today);
    app_draw(&app, pixels, HEIGHT, now, &today);
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
    send("w -9", 0);
    send("k u 218", 0);
    assert(app.screen == CALENDAR && app.year == 2024 && app.month == 1);
    assert(pixels[220 * HEIGHT + HEIGHT - 1 - 199] == 0xff84ead1);
    for (int i = 0; i < 10; ++i)
        tap(50, 252, 0);
    assert(app.year == 2023 && app.month == 3);
    tap(187, 252, 0);
    assert(app.year == 2024 && app.month == 1);
    back();
}

static void tasks(const char *path)
{
    tap(50, 180, 0);
    tap(50, 250, 0);
    assert(app.screen == EDIT_TASK);
    tap(25, 105, 0); /* A */
    tap(295, 250, 0);
    assert(app.screen == TODO && app.task_count == 1);
    tap(50, 55, 0);
    tap(50, 121, 0);
    app_init(&app, path);
    assert(app.task_count == 1 && app.tasks[0].done && !strcmp(app.tasks[0].text, "A"));

    tap(50, 180, 0);
    tap(50, 55, 0);
    tap(50, 180, 0);
    app_init(&app, path);
    assert(app.task_count == 0);
}

static void calculator(void)
{
    tap(50, 235, 0);
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

int main(void)
{
    char dir[] = "/tmp/q2-utilities-test.XXXXXX";
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof path, "%s/tasks.tsv", dir);
    app_init(&app, path);
    stopwatch();
    calendar();
    tasks(path);
    calculator();
    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0);
    puts("Utilities checks passed");
    return 0;
}
