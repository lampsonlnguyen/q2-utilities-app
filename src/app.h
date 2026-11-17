/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef UTILITIES_APP_H
#define UTILITIES_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum { WIDTH = 375, HEIGHT = 320, TASK_LIMIT = 32, TASK_LENGTH = 24, LAP_LIMIT = 4 };

enum screen { HOME, STOPWATCH, CALENDAR, TODO, CALCULATOR, EDIT_TASK, TASK_ACTIONS };

struct task {
    bool done;
    char text[TASK_LENGTH + 1];
};

struct app {
    enum screen screen;
    int focus, home_focus, wheel;
    bool quit;
    int touch_button;
    bool running;
    int64_t started, elapsed, laps[LAP_LIMIT];
    unsigned lap_count;
    int year, month;
    struct task tasks[TASK_LIMIT];
    int task_count, task_index, task_page;
    bool save_failed, load_failed, symbols;
    char draft[TASK_LENGTH + 1];
    const char *task_path;
    double total;
    char op;
    bool entering, error;
    char number[32];
};

void app_init(struct app *app, const char *task_path);
void app_input(struct app *app, const char *message, int64_t now, const struct tm *today);
void app_draw(struct app *app, uint32_t *pixels, int64_t now, const struct tm *today);

#endif
