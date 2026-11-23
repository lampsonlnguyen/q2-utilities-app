/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef Q2_UTILITY_EXTRA_TOOLS_H
#define Q2_UTILITY_EXTRA_TOOLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    TOOL_POMODORO, TOOL_METRONOME, TOOL_CONVERTER, TOOL_RANDOM,
    TOOL_COUNTDOWN, TOOL_DATE, TOOL_ENGINEERING, TOOL_COUNTER,
    TOOL_COUNT
} ToolKind;

typedef struct {
    int64_t due_ms, remaining_ms;
    int work_minutes, break_minutes, sessions, phase;
    bool running;
} PomodoroState;

typedef struct {
    int bpm, beats_per_bar;
    int64_t start_ms, last_tap_ms;
    bool running;
} MetronomeState;

typedef struct {
    int64_t due_ms, remaining_ms;
    bool running;
} CountdownState;

typedef struct {
    int kind, reverse;
    double input;
} ConverterState;

typedef struct {
    uint32_t seed;
    int kind, lo, hi, result;
} RandomState;

typedef struct {
    int delta;
} DateState;

typedef struct {
    int mode, field;
    double a, b, c;
} EngineeringState;

typedef struct {
    int selected, values[4];
    char path[320];
    bool save_error;
} EventCounterState;

typedef struct {
    PomodoroState pomo;
    MetronomeState metro;
    CountdownState countdown;
    ConverterState converter;
    RandomState random;
    DateState date;
    EngineeringState engineering;
    EventCounterState counter;
} ExtraTools;

void extra_init(ExtraTools *tools, const char *task_path);
void extra_enter(ExtraTools *tools, ToolKind kind, int64_t now);
void extra_tick(ExtraTools *tools, int64_t now);
void extra_action(ExtraTools *tools, ToolKind kind, int code, int64_t now,
                  const struct tm *today);
int extra_button_count(ToolKind kind);
const char *extra_button_label(const ExtraTools *tools, ToolKind kind, int button);
void extra_display(const ExtraTools *tools, ToolKind kind, int64_t now,
                   const struct tm *today, char *line1, size_t size1,
                   char *line2, size_t size2, char *line3, size_t size3);
#endif
