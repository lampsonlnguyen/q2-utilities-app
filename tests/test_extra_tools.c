/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/extra_tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void show(ExtraTools *s,ToolKind kind,int64_t now,const struct tm *today,
                 char *a,char *b,char *c) {
    extra_display(s,kind,now,today,a,96,b,96,c,96);
}
int main(void) {
    char dir[]="/tmp/q2-extra-suite-XXXXXX";
    assert(mkdtemp(dir));
    char tasks[320],counters[320];
    snprintf(tasks,sizeof tasks,"%s/tasks.tsv",dir);
    snprintf(counters,sizeof counters,"%s/counters.tsv",dir);
    ExtraTools s;
    extra_init(&s,tasks);
    assert(s.pomo.work_minutes==25 && s.pomo.break_minutes==5);
    assert(s.metro.bpm==120 && s.countdown.remaining_ms==300000);
    assert(extra_button_count(TOOL_POMODORO)==7);
    char a[96],b[96],c[96];
    const struct tm leap={.tm_year=124,.tm_mon=1,.tm_mday=29};
    extra_action(&s,TOOL_POMODORO,0,1000,&leap); /* start 25 minutes */
    extra_tick(&s,1501000);
    assert(s.pomo.sessions==1 && s.pomo.phase==1);
    show(&s,TOOL_POMODORO,1501000,&leap,a,b,c);
    assert(strstr(b,"break"));
    extra_action(&s,TOOL_POMODORO,0,1502000,&leap); /* pause */
    assert(!s.pomo.running);
    extra_action(&s,TOOL_POMODORO,2,1503000,&leap); /* back to focus */
    assert(s.pomo.phase==0 && s.pomo.remaining_ms==25*60000);

    extra_action(&s,TOOL_COUNTDOWN,3,3000,&leap);
    assert(s.countdown.remaining_ms==360000);
    extra_action(&s,TOOL_COUNTDOWN,0,4000,&leap);
    extra_tick(&s,364000);
    assert(!s.countdown.running && s.countdown.remaining_ms==0);
    extra_action(&s,TOOL_COUNTDOWN,1,365000,&leap);
    assert(s.countdown.remaining_ms==300000);

    extra_action(&s,TOOL_METRONOME,2,1000,&leap);
    assert(s.metro.bpm==121);
    extra_action(&s,TOOL_METRONOME,5,2000,&leap);
    assert(s.metro.beats_per_bar==6);
    extra_action(&s,TOOL_METRONOME,6,3000,&leap);
    extra_action(&s,TOOL_METRONOME,6,3500,&leap);
    assert(s.metro.bpm==120);
    extra_action(&s,TOOL_METRONOME,0,3600,&leap);
    show(&s,TOOL_METRONOME,4100,&leap,a,b,c);
    assert(strstr(a,"BPM") && strstr(b,"ACTIVE"));

    show(&s,TOOL_CONVERTER,0,&leap,a,b,c);
    assert(strstr(a,"km") && strstr(b,"mi"));
    extra_action(&s,TOOL_CONVERTER,0,0,&leap);
    show(&s,TOOL_CONVERTER,0,&leap,a,b,c);
    assert(strstr(a,"kg") && strstr(b,"lb"));

    extra_action(&s,TOOL_RANDOM,0,1000,&leap);
    assert(s.random.result>=1 && s.random.result<=100);
    extra_action(&s,TOOL_RANDOM,1,1000,&leap);
    assert(s.random.result>=1 && s.random.result<=6);
    extra_action(&s,TOOL_RANDOM,1,1000,&leap);
    assert(s.random.result==0 || s.random.result==1);

    extra_action(&s,TOOL_DATE,2,1000,&leap);
    show(&s,TOOL_DATE,0,&leap,a,b,c);
    assert(!strcmp(a,"2024-03-01"));
    extra_action(&s,TOOL_DATE,1,1000,&leap);
    assert(s.date.delta==0);
    extra_action(&s,TOOL_DATE,0,1000,&leap);
    show(&s,TOOL_DATE,0,&leap,a,b,c);
    assert(!strcmp(a,"2024-01-30"));
    extra_action(&s,TOOL_DATE,4,1000,&leap);
    assert(s.date.delta==0);

    show(&s,TOOL_ENGINEERING,0,&leap,a,b,c);
    assert(strstr(a,"2 V") && strstr(a,"0.04 W"));
    extra_action(&s,TOOL_ENGINEERING,0,0,&leap);
    show(&s,TOOL_ENGINEERING,0,&leap,a,b,c);
    assert(strstr(a,"2.5 V"));
    extra_action(&s,TOOL_ENGINEERING,0,0,&leap);
    show(&s,TOOL_ENGINEERING,0,&leap,a,b,c);
    assert(strstr(a,"15 h"));

    extra_action(&s,TOOL_COUNTER,0,0,&leap);
    extra_action(&s,TOOL_COUNTER,0,0,&leap);
    assert(s.counter.values[0]==2 && !s.counter.save_error);
    extra_action(&s,TOOL_COUNTER,3,0,&leap);
    extra_action(&s,TOOL_COUNTER,0,0,&leap);
    assert(s.counter.values[1]==1);
    ExtraTools second;
    extra_init(&second,tasks);
    assert(second.counter.values[0]==2 && second.counter.values[1]==1);
    extra_action(&second,TOOL_COUNTER,2,0,&leap);
    assert(second.counter.values[0]==0);
    assert(unlink(counters)==0);
    assert(rmdir(dir)==0);
    puts("Eight offline tools: timers, converters, RNG, Gregorian dates, engineering and SD counters passed");
    return 0;
}
