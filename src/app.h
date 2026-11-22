/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef UTILITIES_APP_H
#define UTILITIES_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum { WIDTH = 375, HEIGHT = 320, TASK_LIMIT = 32, TASK_LENGTH = 24, LAP_LIMIT = 4 };

enum screen { HOME, STOPWATCH, CALENDAR, TODO, CALCULATOR, WORLD_CLOCK, WC_DETAIL, EDIT_TASK, TASK_ACTIONS };

struct task {
    bool done;
    char text[TASK_LENGTH + 1];
};

struct app {
    enum screen screen;
    int focus, home_focus, wheel, wheel_units;
    int64_t wheel_at;
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
    int wc_top, wc_selected;
    double total;
    char op;
    bool entering, error;
    char number[32];
};

void app_wheel_config(struct app *app, const char *config);
void app_theme(const char *config, int hour);
void app_init(struct app *app, const char *task_path);
void app_input(struct app *app, const char *message, int64_t now, const struct tm *today);
/* Draw an opaque frame into a portrait framebuffer. Stride is in pixels.
 * utc is the current UTC epoch (seconds) for World Clock; today is the
 * device's local date, for the calendar and day rollover. */
void app_draw(struct app *app, uint32_t *pixels, unsigned stride, int64_t now, time_t utc, const struct tm *today);

#endif
