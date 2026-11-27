/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef UTILITIES_BROWSE_H
#define UTILITIES_BROWSE_H
#include "app.h"
bool menu_screen(const char *uri, enum screen *out);
int menu_serve(void);
bool alarm_play(void);
bool ringtone_load(struct app *app,const char *context);
bool ringtone_apply(const struct app *app,bool save);
void alarm_deadline(int64_t due);
#endif
