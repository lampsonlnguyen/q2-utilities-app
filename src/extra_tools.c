/* SPDX-License-Identifier: GPL-3.0-only */
#include "extra_tools.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

#define MINUTE_MS 60000LL
#define MAX_TIMER_MS (23LL*3600000LL+59LL*60000LL)
#define COUNTER_LIMIT 9999999

static int bounded(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}
static int64_t bounded_ms(int64_t value) {
    return value < 1000 ? 1000 : value > MAX_TIMER_MS ? MAX_TIMER_MS : value;
}
static int64_t left_ms(int64_t due, int64_t now) {
    return due > now ? due - now : 0;
}
static void duration(char *out, size_t size, int64_t milliseconds) {
    int64_t total=(milliseconds+999)/1000;
    snprintf(out,size,"%02lld:%02lld:%02lld",
             (long long)(total/3600),(long long)(total/60%60),(long long)(total%60));
}
static bool counter_save(EventCounterState *s) {
    if(!s->path[0]) return false;
    char temp[sizeof s->path+8];
    if(snprintf(temp,sizeof temp,"%s.tmp",s->path)>=(int)sizeof temp) return false;
    FILE *f=fopen(temp,"wb");
    if(!f) return false;
    bool ok=true;
    for(int i=0;i<4;i++)
        if(fprintf(f,"%d\n",s->values[i])<0) ok=false;
    if(fflush(f)!=0)ok=false;
    if(ok && fsync(fileno(f))!=0)ok=false;
    if(fclose(f)!=0)ok=false;
    if(ok && rename(temp,s->path)==0) return true;
    unlink(temp);
    return false;
}
void extra_init(ExtraTools *s, const char *task_path) {
    memset(s,0,sizeof *s);
    s->pomo.work_minutes=25;
    s->pomo.break_minutes=5;
    s->pomo.remaining_ms=25*MINUTE_MS;
    s->metro.bpm=120;
    s->metro.beats_per_bar=4;
    s->countdown.remaining_ms=5*MINUTE_MS;
    s->countdown.preset_ms=5*MINUTE_MS;
    s->converter.input=10;
    s->random.seed=(uint32_t)time(NULL)^0x9e3779b9u;
    if(!s->random.seed)s->random.seed=1;
    s->random.lo=1;s->random.hi=100;s->random.result=0;
    s->engineering.a=100;s->engineering.b=0.02;s->engineering.c=1000;
    if(task_path) {
        const char *slash=strrchr(task_path,'/');
        if(slash) {
            size_t len=(size_t)(slash-task_path);
            if(len+14<sizeof s->counter.path) {
                memcpy(s->counter.path,task_path,len);
                strcpy(s->counter.path+len,"/counters.tsv");
            }
        }
    }
    FILE *f=s->counter.path[0]?fopen(s->counter.path,"rb"):NULL;
    if(f) {
        int v[4], count=0;
        for(;count<4 && fscanf(f,"%d",&v[count])==1;count++)
            if(v[count]<0||v[count]>COUNTER_LIMIT)break;
        int extra;
        do {extra=fgetc(f);} while(extra==' ' || extra=='\t' || extra=='\r' || extra=='\n');
        fclose(f);
        if(count==4 && extra==EOF)memcpy(s->counter.values,v,sizeof v);
    }
}
void extra_enter(ExtraTools *s, ToolKind kind, int64_t now) {
    (void)now;
    if(kind==TOOL_METRONOME && s->metro.bpm==0)s->metro.bpm=120;
}
void extra_tick(ExtraTools *s, int64_t now) {
    if(s->countdown.running && now>=s->countdown.due_ms) {
        s->countdown.running=false;
        s->countdown.remaining_ms=0;
    }
    if(s->pomo.running && now>=s->pomo.due_ms) {
        /* Advance across elapsed intervals without spinning forever after
         * a long suspend. All timing uses Q2's monotonic clock. */
        for(int i=0;i<8 && s->pomo.running && now>=s->pomo.due_ms;i++) {
            if(s->pomo.phase==0)s->pomo.sessions++;
            s->pomo.phase^=1;
            int minutes=s->pomo.phase?s->pomo.break_minutes:s->pomo.work_minutes;
            s->pomo.due_ms+=minutes*MINUTE_MS;
        }
        if(now>=s->pomo.due_ms) {
            int minutes=s->pomo.phase?s->pomo.break_minutes:s->pomo.work_minutes;
            s->pomo.due_ms=now+minutes*MINUTE_MS;
        }
    }
}
static uint32_t next_random(RandomState *s) {
    uint32_t x=s->seed;
    x^=x<<13;x^=x>>17;x^=x<<5;
    s->seed=x?x:0x51633e2du;
    return s->seed;
}
static void random_roll(RandomState *s) {
    if(s->kind==1)s->result=(int)(next_random(s)%6u)+1;
    else if(s->kind==2)s->result=(int)(next_random(s)&1u);
    else {
        unsigned span=(unsigned)((int64_t)s->hi-s->lo+1);
        s->result=s->lo+(int)(next_random(s)%span);
    }
}
/* Howard Hinnant civil-date algorithm; Gregorian, no local time/DST
 * arithmetic, for dates ranging 1900-2100. */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y-=m<=2;
    const int era=(y>=0?y:y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400);
    const int mp=(int)m+(m>2?-3:9);
    const unsigned doy=(153u*(unsigned)mp+2)/5+d-1;
    const unsigned yrc=yoe*365+yoe/4-yoe/100+doy;
    return (int64_t)era*146097+(int64_t)yrc-719468;
}
static void civil_from_days(int64_t days, int *y, unsigned *m, unsigned *d) {
    days+=719468;
    const int era=(int)((days>=0?days:days-146096)/146097);
    const unsigned doe=(unsigned)(days-(int64_t)era*146097);
    const unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    int year=(int)yoe+era*400;
    const unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    const unsigned mp=(5*doy+2)/153;
    *d=doy-(153*mp+2)/5+1;
    *m=(unsigned)((int)mp+(mp<10?3:-9));
    year+=*m<=2;
    *y=year;
}
static void pomodoro_action(PomodoroState *s, int code, int64_t now) {
    int minutes=s->phase?s->break_minutes:s->work_minutes;
    if(code==0) {
        if(s->running) {
            s->remaining_ms=left_ms(s->due_ms,now);
            s->running=false;
        } else {
            if(s->remaining_ms<1000)s->remaining_ms=(int64_t)minutes*MINUTE_MS;
            s->due_ms=now+s->remaining_ms;s->running=true;
        }
    } else if(code==1) {
        s->running=false;s->remaining_ms=(int64_t)minutes*MINUTE_MS;
    } else if(code==2) {
        s->running=false;s->phase^=1;
        s->remaining_ms=(int64_t)(s->phase?s->break_minutes:s->work_minutes)*MINUTE_MS;
    } else if(code==3 || code==4) {
        s->work_minutes=bounded(s->work_minutes+(code==4?1:-1),1,120);
        if(!s->phase){s->running=false;s->remaining_ms=(int64_t)s->work_minutes*MINUTE_MS;}
    } else if(code==5 || code==6) {
        s->break_minutes=bounded(s->break_minutes+(code==6?1:-1),1,60);
        if(s->phase){s->running=false;s->remaining_ms=(int64_t)s->break_minutes*MINUTE_MS;}
    }
}
static void metronome_action(MetronomeState *s,int code,int64_t now) {
    if(code==0) {s->running=!s->running;s->start_ms=now;}
    if(code==1||code==2||code==3||code==4) {
        int delta=code==1?-1:code==2?1:code==3?-10:10;
        s->bpm=bounded(s->bpm+delta,30,300);s->start_ms=now;
    }
    if(code==5) {s->beats_per_bar=s->beats_per_bar==3?4:s->beats_per_bar==4?6:3;s->start_ms=now;}
    if(code==6) {
        if(s->last_tap_ms>0 && now>s->last_tap_ms) {
            int64_t elapsed=now-s->last_tap_ms;
            if(elapsed>=200 && elapsed<=2000)
                s->bpm=bounded((int)(60000/elapsed),30,300);
        }
        s->last_tap_ms=now;s->start_ms=now;
    }
}
static void countdown_action(CountdownState *s,int code,int64_t now) {
    if(code==0) {
        if(s->running){s->remaining_ms=left_ms(s->due_ms,now);s->running=false;}
        else if(s->remaining_ms>0){s->due_ms=now+s->remaining_ms;s->running=true;}
    }
    if(code==1) {s->running=false;s->remaining_ms=s->preset_ms;}
    if(code>=2 && code<=5 && !s->running) {
        int64_t delta=code==2?-MINUTE_MS:code==3?MINUTE_MS:
                      code==4?-10000:10000;
        s->remaining_ms=bounded_ms(s->remaining_ms+delta);
        s->preset_ms=s->remaining_ms;
    }
}
static void converter_action(ConverterState *s,int code) {
    if(code==0) {s->kind=(s->kind+1)%6;s->input=10;s->reverse=0;}
    if(code==1) s->reverse^=1;
    if(code>=2) {
        int delta=code==2?-10:code==3?-1:code==4?1:10;
        s->input+=delta;
        if(s->kind!=2 && s->input<0)s->input=0;
        if(s->input>1000000)s->input=1000000;
        if(s->kind==2 && s->input<(s->reverse?-459.67:-273.15))
            s->input=s->reverse?-459.67:-273.15;
    }
}
static void random_action(RandomState *s,int code) {
    if(code==0)random_roll(s);
    if(code==1) {s->kind=(s->kind+1)%3;random_roll(s);}
    if(s->kind!=0)return;
    if(code==2)s->lo=bounded(s->lo-1,-10000,s->hi);
    if(code==3)s->lo=bounded(s->lo+1,-10000,s->hi);
    if(code==4)s->hi=bounded(s->hi-1,s->lo,10000);
    if(code==5)s->hi=bounded(s->hi+1,s->lo,10000);
}
static void engineering_action(EngineeringState *s,int code) {
    if(code==0) {
        s->mode=(s->mode+1)%3;s->field=0;
        if(s->mode==0){s->a=100;s->b=0.02;s->c=1000;}
        if(s->mode==1){s->a=5;s->b=1000;s->c=1000;}
        if(s->mode==2){s->a=3000;s->b=200;s->c=1000;}
        return;
    }
    if(code==1){s->field=(s->field+1)%(s->mode==1?3:2);return;}
    if(code<2||code>5)return;
    int sign=code==2||code==4?-1:1;
    int factor=code==4||code==5?10:1;
    double *value=s->field==0?&s->a:s->field==1?&s->b:&s->c;
    double step=1;
    if(s->mode==0)step=s->field==0?10:0.005;
    if(s->mode==1)step=s->field==0?1:100;
    if(s->mode==2)step=s->field==0?100:10;
    *value+=sign*factor*step;
    if(*value<step)*value=step;
    if(*value>1000000)*value=1000000;
}
void extra_action(ExtraTools *s,ToolKind kind,int code,int64_t now,const struct tm *today) {
    extra_tick(s,now);
    switch(kind) {
    case TOOL_POMODORO: pomodoro_action(&s->pomo,code,now);break;
    case TOOL_METRONOME: metronome_action(&s->metro,code,now);break;
    case TOOL_COUNTDOWN: countdown_action(&s->countdown,code,now);break;
    case TOOL_CONVERTER: converter_action(&s->converter,code);break;
    case TOOL_RANDOM: random_action(&s->random,code);break;
    case TOOL_DATE:
        if(code==4)s->date.delta=0;
        else s->date.delta=bounded(s->date.delta+(code==0?-30:code==1?-1:code==2?1:30),-36525,36525);
        (void)today;
        break;
    case TOOL_ENGINEERING: engineering_action(&s->engineering,code);break;
    case TOOL_COUNTER: {
        EventCounterState *e=&s->counter;
        if(code==0 && e->values[e->selected]<COUNTER_LIMIT)e->values[e->selected]++;
        if(code==1 && e->values[e->selected]>0)e->values[e->selected]--;
        if(code==2)e->values[e->selected]=0;
        if(code==3)e->selected=(e->selected+1)%4;
        if(code==4)e->selected=(e->selected+3)%4;
        if(code>=0 && code<=2)e->save_error=!counter_save(e);
        break;
    }
    default:break;
    }
}
int extra_button_count(ToolKind kind) {
    switch(kind) {
    case TOOL_POMODORO:return 7;
    case TOOL_METRONOME:return 7;
    case TOOL_CONVERTER:return 6;
    case TOOL_RANDOM:return 6;
    case TOOL_COUNTDOWN:return 6;
    case TOOL_DATE:return 5;
    case TOOL_ENGINEERING:return 6;
    case TOOL_COUNTER:return 5;
    default:return 0;
    }
}
const char *extra_button_label(const ExtraTools *s,ToolKind kind,int code) {
    static const char *const pomo[]={"Start","Reset","Work/Rest","Work -","Work +","Rest -","Rest +"};
    static const char *const metro[]={"Start","BPM -","BPM +","-10 BPM","+10 BPM","Beats","Tap"};
    static const char *const count[]={"Start","Reset","-1 min","+1 min","-10 sec","+10 sec"};
    static const char *const conv[]={"Units","Swap","-10","-1","+1","+10"};
    static const char *const random[]={"Generate","Mode","Min -","Min +","Max -","Max +"};
    static const char *const date[]={"-30 days","-1 day","+1 day","+30 days","Today"};
    static const char *const eng[]={"Mode","Select","-Small","+Small","-Large","+Large"};
    static const char *const counter[]={"+1","-1","Reset","Next","Previous"};
    if(code<0||code>=extra_button_count(kind))return "";
    if(kind==TOOL_POMODORO && code==0)return s->pomo.running?"Pause":"Start";
    if(kind==TOOL_METRONOME && code==0)return s->metro.running?"Stop":"Start";
    if(kind==TOOL_COUNTDOWN && code==0)return s->countdown.running?"Pause":"Start";
    switch(kind) {
    case TOOL_POMODORO:return pomo[code];
    case TOOL_METRONOME:return metro[code];
    case TOOL_CONVERTER:return conv[code];
    case TOOL_RANDOM:return random[code];
    case TOOL_COUNTDOWN:return count[code];
    case TOOL_DATE:return date[code];
    case TOOL_ENGINEERING:return eng[code];
    case TOOL_COUNTER:return counter[code];
    default:return "";
    }
}
void extra_display(const ExtraTools *s,ToolKind kind,int64_t now,
                   const struct tm *today,char *a,size_t na,char *b,size_t nb,char *c,size_t nc) {
    if(na)a[0]=0;
    if(nb)b[0]=0;
    if(nc)c[0]=0;
    switch(kind) {
    case TOOL_POMODORO: {
        const PomodoroState *p=&s->pomo;
        duration(a,na,p->running?left_ms(p->due_ms,now):p->remaining_ms);
        snprintf(b,nb,"%s  |  %s",p->phase?"Short break":"Focus",
                 p->running?"Running":"Paused");
        snprintf(c,nc,"Work %dm  Rest %dm  Sessions %d",
                 p->work_minutes,p->break_minutes,p->sessions);
        break;
    }
    case TOOL_METRONOME: {
        const MetronomeState *m=&s->metro;
        int beat=1;
        if(m->running && now>=m->start_ms)
            beat=(int)(((uint64_t)(now-m->start_ms)*(unsigned)m->bpm/60000u)%
                       (unsigned)m->beats_per_bar)+1;
        snprintf(a,na,"%d BPM   %d/%d",m->bpm,beat,m->beats_per_bar);
        snprintf(b,nb,"%s",m->running?"VISUAL BEAT ACTIVE":"Stopped");
        snprintf(c,nc,"On-screen pulse; audio pending Q2 API");
        break;
    }
    case TOOL_COUNTDOWN: {
        const CountdownState *t=&s->countdown;
        duration(a,na,t->running?left_ms(t->due_ms,now):t->remaining_ms);
        snprintf(b,nb,"%s",t->running?"Countdown running":
                 t->remaining_ms==0?"Time elapsed":"Paused / ready");
        snprintf(c,nc,"Keeps time while Utilities stays open");
        break;
    }
    case TOOL_CONVERTER: {
        static const char *const from[]={"km","kg","C","km/h","cm","MiB"};
        static const char *const to[]={"mi","lb","F","mph","in","MB"};
        const ConverterState *q=&s->converter;
        const double factor[]={0.621371192,2.20462262185,0,0.621371192,
                               0.3937007874,1.048576};
        double value=q->input;
        double result=q->kind==2?(q->reverse?(value-32)*5/9:value*9/5+32):
                      q->reverse?value/factor[q->kind]:value*factor[q->kind];
        snprintf(a,na,"%.6g %s",value,q->reverse?to[q->kind]:from[q->kind]);
        snprintf(b,nb,"= %.6g %s",result,q->reverse?from[q->kind]:to[q->kind]);
        snprintf(c,nc,"Offline conversion; Units changes type");
        break;
    }
    case TOOL_RANDOM: {
        const RandomState *r=&s->random;
        if(r->kind==0)snprintf(a,na,"%d",r->result);
        else if(r->kind==1)snprintf(a,na,"D6: %d",r->result);
        else snprintf(a,na,"%s",r->result?"Heads":"Tails");
        snprintf(b,nb,"%s",r->kind==0?"Random integer":r->kind==1?"Six-sided die":"Coin toss");
        snprintf(c,nc,r->kind==0?"Range %d .. %d":"Not cryptographically secure",r->lo,r->hi);
        break;
    }
    case TOOL_DATE: {
        if(!today)break;
        int y; unsigned m,d;
        const int year=today->tm_year+1900;
        int64_t day=days_from_civil(year,(unsigned)today->tm_mon+1,
                                    (unsigned)today->tm_mday)+s->date.delta;
        civil_from_days(day,&y,&m,&d);
        snprintf(a,na,"%04d-%02u-%02u",y,m,d);
        snprintf(b,nb,"%+d days from today",s->date.delta);
        snprintf(c,nc,"Q2 local date | Gregorian calendar");
        break;
    }
    case TOOL_ENGINEERING: {
        const EngineeringState *e=&s->engineering;
        if(e->mode==0) {
            snprintf(a,na,"V=%.5g V  P=%.5g W",e->a*e->b,e->a*e->b*e->b);
            snprintf(b,nb,"R=%.5g Ohm   I=%.5g A",e->a,e->b);
            snprintf(c,nc,"Ohm's law | Editing %s",e->field?"Current":"Resistance");
        } else if(e->mode==1) {
            snprintf(a,na,"Vout = %.5g V",e->a*e->c/(e->b+e->c));
            snprintf(b,nb,"Vin %.4gV  R1 %.4g  R2 %.4g",e->a,e->b,e->c);
            snprintf(c,nc,"Divider | Editing %s",e->field==0?"Vin":e->field==1?"R1":"R2");
        } else {
            snprintf(a,na,"Runtime %.5g h",e->a/e->b);
            snprintf(b,nb,"Battery %.5g mAh | %.5g mA",e->a,e->b);
            snprintf(c,nc,"Ideal estimate | Editing %s",e->field?"Load":"Capacity");
        }
        break;
    }
    case TOOL_COUNTER: {
        const EventCounterState *e=&s->counter;
        snprintf(a,na,"%c: %d",'A'+e->selected,e->values[e->selected]);
        snprintf(b,nb,"A:%d  B:%d  C:%d  D:%d",e->values[0],e->values[1],
                 e->values[2],e->values[3]);
        snprintf(c,nc,"%s",e->save_error?"Storage write failed":"Saved to SD card");
        break;
    }
    default:break;
    }
}
