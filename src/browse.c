/* SPDX-License-Identifier: GPL-3.0-only */
#include "browse.h"
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <poll.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define SOCK "/tmp/q2-utilities-app.browse.sock"
static volatile sig_atomic_t stopped;
static void stop_menu(int sig) { (void)sig; stopped = 1; }

static const struct { const char *uri, *title; enum screen screen; } tools[] = {
    {"stopwatch", "Stopwatch", STOPWATCH},
    {"timer", "Timer", COUNTDOWN},
    {"world-clock", "World Clock", WORLD_CLOCK},
    {"calendar", "Calendar", CALENDAR},
    {"todo", "To-Do List", TODO},
    {"calculator", "Calculator", CALCULATOR},
};

bool menu_screen(const char *uri, enum screen *out)
{
    for (unsigned i = 0; i < sizeof tools / sizeof tools[0]; ++i)
        if (!strcmp(uri, tools[i].uri)) {
            *out = tools[i].screen;
            return true;
        }
    return false;
}


#define CARD "/mnt/mmc"
#define SONG_FILE CARD "/.utilities-app/alarm-song"
#define CLIP CARD "/.utilities-app/alarm.wav"
#define PREVIEW CARD "/.utilities-app/preview.wav"
#define OFFSET CARD "/.utilities-app/alarm-offset"
#define QUEUE CARD "/.utilities-app/alarm.m3u"
#define PLAY "/tmp/q2-utilities-app.play"
#define STATE "/tmp/q2-utilities-app.state"
#define DEADLINE "/tmp/q2-utilities-app.timer"
#define PATH_SIZE 512

/* The target glibc 2.28 exports __xstat, not the libc_nonshared stat wrapper. */
#if defined(__mips__) && defined(__GLIBC__)
extern int __xstat(int, const char *, struct stat *);
static int file_stat(const char *path, struct stat *st)
{
    return __xstat(_STAT_VER, path, st);
}
#else
static int file_stat(const char *path, struct stat *st)
{
    return stat(path, st);
}
#endif

static bool audio_file(const char *path)
{
    const char *ext=strrchr(path,'.');
    const char *types[]={".mp3",".flac",".wav",".aac",".m4a",".ogg",".opus",".ape",".wma",".aiff",".alac",".dsf",".dff"};
    if(!ext) return false;
    for(unsigned i=0;i<sizeof types/sizeof types[0];i++)
        if(!strcasecmp(ext,types[i])) return true;
    return false;
}
static bool valid_path(const char *path, bool directory)
{
    char resolved[PATH_MAX];
    struct stat st;
    if(strlen(path)>=PATH_SIZE || strpbrk(path,"\r\n\t") ||
       !realpath(path,resolved) ||
       (strcmp(resolved,CARD) && strncmp(resolved,CARD "/",sizeof CARD)) ||
       file_stat(resolved,&st)) return false;
    return directory?S_ISDIR(st.st_mode):S_ISREG(st.st_mode)&&audio_file(resolved);
}
static bool put(const char *path,const char *value)
{
    char temp[PATH_SIZE];
    if(snprintf(temp,sizeof temp,"%s.%ld.tmp",path,(long)getpid()) >= (int)sizeof temp) return false;
    FILE *f=fopen(temp,"w");
    if(!f) return false;
    bool ok=fputs(value,f)>=0;
    if(fclose(f)) ok=false;
    if(ok && !rename(temp,path)) return true;
    unlink(temp);
    return false;
}
static bool selected_song(char *path,size_t size)
{
    FILE *f=fopen(SONG_FILE,"r");
    if(!f) return false;
    bool ok=fgets(path,(int)size,f)!=NULL;
    fclose(f);
    if(!ok) return false;
    path[strcspn(path,"\r\n")]=0;
    return valid_path(path,false);
}
static const char *base(const char *p)
{
    const char *s=strrchr(p,'/');
    return s?s+1:p;
}

/* The stock Q2 ships ffmpeg 4.2; use argv, never a shell command. */
static int ffmpeg(char *const args[], char *log, size_t size)
{
    int pipefd[2];
    if(pipe(pipefd)) return -1;
    pid_t child=fork();
    if(child==0) {
        close(pipefd[0]);
        dup2(pipefd[1],STDERR_FILENO);
        close(pipefd[1]);
        execv("/usr/bin/ffmpeg",args);
        _exit(127);
    }
    close(pipefd[1]);
    if(child<0) { close(pipefd[0]); return -1; }
    size_t used=0;
    int status=0, elapsed=0;
    bool exited=false;
    while(elapsed<8000 && !stopped) {
        struct pollfd p={.fd=pipefd[0],.events=POLLIN};
        if(poll(&p,1,100)>0) {
            char buf[512];
            ssize_t n=read(pipefd[0],buf,sizeof buf);
            if(n>0 && log && used<size-1) {
                size_t copy=(size_t)n<size-1-used?(size_t)n:size-1-used;
                memcpy(log+used,buf,copy); used+=copy;
            }
        }
        if(waitpid(child,&status,WNOHANG)==child) { exited=true; break; }
        elapsed+=100;
    }
    if(!exited) { kill(child,SIGKILL); waitpid(child,&status,0); }
    if(log && size) log[used]=0;
    close(pipefd[0]);
    return exited&&WIFEXITED(status)?WEXITSTATUS(status):-1;
}
static int song_length(const char *path)
{
    char log[8192];
    char *args[]={"ffmpeg","-nostdin","-hide_banner","-i",(char *)path,NULL};
    ffmpeg(args,log,sizeof log);
    const char *d=strstr(log,"Duration: ");
    int h,m; double s;
    if(!d||sscanf(d+10,"%d:%d:%lf",&h,&m,&s)!=3) return 0;
    return h*3600+m*60+(int)s;
}
static bool make_clip(const char *song,int start,const char *dest)
{
    char offset[24],temp[PATH_SIZE];
    snprintf(offset,sizeof offset,"%d",start);
    if(snprintf(temp,sizeof temp,"%s.tmp.wav",dest)>=(int)sizeof temp) return false;
    char *args[]={"ffmpeg","-nostdin","-v","error","-y","-ss",offset,"-i",(char *)song,
                  "-t","10","-map","0:a:0","-vn","-ac","2","-ar","44100",
                  "-c:a","pcm_s16le",temp,NULL};
    struct stat st;
    if(ffmpeg(args,NULL,0)==0 && !file_stat(temp,&st) && st.st_size>44 && !rename(temp,dest)) return true;
    unlink(temp);
    return false;
}
static int saved_offset(void)
{
    FILE *f=fopen(OFFSET,"r");
    int n=0;
    if(f) { if(fscanf(f,"%d",&n)!=1) n=0; fclose(f); }
    return n<0?0:n;
}
static bool play_path(const char *song)
{
    char line[128],list[PATH_SIZE+2];
    snprintf(list,sizeof list,"%s\n",song);
    if(!put(QUEUE,list)) return false;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    snprintf(line,sizeof line,"%ld%09ld %s\n",(long)ts.tv_sec,ts.tv_nsec,QUEUE);
    return put(PLAY,line);
}
bool alarm_play(void)
{
    char song[PATH_SIZE];
    if(!selected_song(song,sizeof song)) return false;
    if(access(CLIP,R_OK)==0) return play_path(CLIP);
    return play_path(song);
}
void alarm_deadline(int64_t due)
{
    if(due<=0) { unlink(DEADLINE); return; }
    char text[48];
    snprintf(text,sizeof text,"%lld\n",(long long)due);
    put(DEADLINE,text);
}
static void alarm_poll(void)
{
    FILE *f=fopen(DEADLINE,"r");
    if(!f) { unlink(STATE); return; }
    long long due=0;
    int ok=fscanf(f,"%lld",&due);
    fclose(f);
    if(ok!=1 || due<=0) { unlink(DEADLINE); unlink(STATE); return; }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    int64_t now=(int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
    if(now>=due) {
        unlink(DEADLINE);
        alarm_play();
        unlink(STATE);
    } else if(access(STATE,F_OK)) put(STATE,"hold=1\n");
}
static void escaped(FILE *out,const char *s)
{
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if(c=='%'||c=='\t'||c=='\r'||c=='\n') fprintf(out,"%%%02X",c);
        else fputc(c,out);
    }
}
static bool decode(char *s)
{
    char *w=s;
    for(char *p=s;*p;p++) {
        if(*p=='%') {
            if(!p[1]||!p[2]) return false;
            char hex[3]={p[1],p[2],0},*end;
            long n=strtol(hex,&end,16);
            if(*end||n<=0||n>255) return false;
            *w++=(char)n; p+=2;
        } else *w++=*p;
    }
    *w=0;
    return true;
}
static void row(FILE *out,const char *kind,const char *uri,const char *title,const char *sub,const char *ctx)
{
    const char *fields[]={kind,uri,title,sub,ctx,""};
    for(int i=0;i<6;i++) { if(i) fputc('\t',out); escaped(out,fields[i]); }
    fputc('\n',out);
}
static void header(FILE *out,const char *id,const char *message,const char *cursor)
{
    fprintf(out,"Q2SPOT/1\n%s\tok\t",id);
    escaped(out,message); fputc('\t',out); escaped(out,cursor); fputc('\n',out);
}
static void sound_menu(FILE *out,const char *id)
{
    char path[PATH_SIZE];
    header(out,id,"","");
    row(out,"info","","Selected song",selected_song(path,sizeof path)?base(path):"None","");
    row(out,"list","folder","Choose from Library","",CARD);
    row(out,"list","clip","Edit 10-second clip","","");
    row(out,"action","preview","Preview alarm","","");
    row(out,"action","clear","Clear selection","","");
}
/* Read only one directory. Bound memory to 48 names, and page by a stable
 * lexicographic cursor rather than loading the entire music collection. */
static void folder(FILE *out,const char *id,const char *path,const char *cursor)
{
    struct entry { char name[256]; bool dir; } entries[48];
    unsigned n=0; bool more=false;
    if(!valid_path(path,true)) { header(out,id,"",""); row(out,"info","","Folder unavailable","",""); return; }
    DIR *d=opendir(path);
    if(!d) { header(out,id,"",""); row(out,"info","","Folder unavailable","",""); return; }
    struct dirent *e;
    while((e=readdir(d))) {
        if(e->d_name[0]=='.'||strcmp(e->d_name,cursor)<=0||strpbrk(e->d_name,"\r\n\t")) continue;
        char full[PATH_SIZE];
        if(snprintf(full,sizeof full,"%s/%s",path,e->d_name)>=(int)sizeof full) continue;
        bool dir=valid_path(full,true);
        if(!dir&&!valid_path(full,false)) continue;
        unsigned i=0;
        while(i<n&&strcmp(entries[i].name,e->d_name)<0) i++;
        if(i>=48) { more=true; continue; }
        if(n==48) more=true; else n++;
        memmove(entries+i+1,entries+i,(n-i-1)*sizeof entries[0]);
        snprintf(entries[i].name,sizeof entries[i].name,"%s",e->d_name);
        entries[i].dir=dir;
    }
    closedir(d);
    header(out,id,"",more&&n?entries[n-1].name:"");
    for(unsigned i=0;i<n;i++) {
        char full[PATH_SIZE];
        if(snprintf(full,sizeof full,"%s/%s",path,entries[i].name)>=(int)sizeof full) continue;
        row(out,entries[i].dir?"list":"action",entries[i].dir?"folder":"choose",entries[i].name,"",full);
    }
    if(!n) row(out,"info","","No music or folders","","");
}

static int draft_start=-1;

static void clip_menu(FILE *out,const char *id,int start)
{
    char song[PATH_SIZE],range[64],arg[32];
    if(!selected_song(song,sizeof song)) {
        header(out,id,"",""); row(out,"info","","Choose a song first","",""); return;
    }
    int duration=song_length(song);
    if(duration<10) {
        header(out,id,"",""); row(out,"info","","Song shorter than 10 seconds or unreadable","",""); return;
    }
    if(start<0) start=0;
    if(start>duration-10) start=duration-10;
    snprintf(range,sizeof range,"%02d:%02d - %02d:%02d",start/60,start%60,(start+10)/60,(start+10)%60);
    snprintf(arg,sizeof arg,"%d",start);
    header(out,id,"","");
    row(out,"info","",base(song),range,"");
    /* Firmware lists provide the theme, selection and native wheel input. */
    row(out,"field","clipseek","Start time (MM:SS)",range,arg);
    row(out,"action","clipback","Back 10 seconds","",arg);
    row(out,"action","clipnext","Forward 10 seconds","",arg);
    row(out,"action","clippreview","Preview 10 seconds","",arg);
    row(out,"action","clipsave","Save alarm clip","",arg);
}

static void reply(int fd)
{
    char request[8193], id[24], op[24];
    size_t used = 0;
    struct timeval timeout = { .tv_sec = 2 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    while (used < sizeof request - 1) {
        ssize_t n = read(fd, request + used, sizeof request - 1 - used);
        if (n < 0) return;
        if (!n) break;
        used += (size_t)n;
    }
    request[used] = 0;
    if (strncmp(request, "Q2SPOT/1\n", 9) ||
        sscanf(request + 9, "%23[0-9]\t%23[^\t\r\n]", id, op) != 2)
        return;
    FILE *out = fdopen(dup(fd), "w");
    if (!out) return;
    if (!strcmp(op, "menu")) {
        fprintf(out, "Q2SPOT/1\n%s\tok\t\t\n", id);
        for (unsigned i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
            if(tools[i].screen==COUNTDOWN) row(out,"list","timer","Timer","","");
            else row(out,"screen",tools[i].uri,tools[i].title,"","");
        }
    } else if(!strcmp(op,"timer")) {
        header(out,id,"","");
        row(out,"screen","timer","Set Timer","","");
        row(out,"list","sound","Alarm Sound","","");
    } else if(!strcmp(op,"sound")) {
        sound_menu(out,id);
    } else if(!strcmp(op,"clip")) {
        if(draft_start<0) draft_start=saved_offset();
        clip_menu(out,id,draft_start);
    } else if(!strcmp(op,"clipseek")||!strcmp(op,"clipback")||!strcmp(op,"clipnext")||
              !strcmp(op,"clippreview")||!strcmp(op,"clipsave")) {
        char song[PATH_SIZE],*arg=strchr(request+9,'\t');
        if(arg) arg=strchr(arg+1,'\t');
        int start=arg?atoi(arg+1):0;
        int duration=selected_song(song,sizeof song)?song_length(song):0;
        if(!strcmp(op,"clipseek")) {
            char *value=arg?strchr(arg+1,'\t'):NULL;
            int m=0,s=0;
            if(value && sscanf(value+1,"%d:%d",&m,&s)==2 && m>=0 && m<=100000 && s>=0 && s<60)
                start=m*60+s;
        } else if(!strcmp(op,"clipback")) start-=10;
        else if(!strcmp(op,"clipnext")) start+=10;
        if(start<0) start=0;
        if(duration>=10 && start>duration-10) start=duration-10;
        draft_start=start;
        if(!strcmp(op,"clipseek")) clip_menu(out,id,start);
        else if(!strcmp(op,"clipback")||!strcmp(op,"clipnext")) header(out,id,"","");
        else if(duration<10) header(out,id,"Song unavailable","");
        else if(!strcmp(op,"clippreview")) header(out,id,make_clip(song,start,PREVIEW)&&play_path(PREVIEW)?"Playing clip":"Preview failed","");
        else {
            char value[24]; snprintf(value,sizeof value,"%d",start);
            header(out,id,make_clip(song,start,CLIP)&&put(OFFSET,value)?"Alarm clip saved":"Unable to save clip","");
        }
    } else if(!strcmp(op,"preview")) {
        header(out,id,alarm_play()?"Playing selected song":"Choose a song first","");
    } else if(!strcmp(op,"clear")) {
        unlink(SONG_FILE); unlink(CLIP); unlink(OFFSET); draft_start=-1;
        header(out,id,"Selection cleared","");
    } else if(!strcmp(op,"folder")||!strcmp(op,"choose")) {
        char *arg=request+9;
        arg=strchr(arg,'\t');
        if(arg) arg=strchr(arg+1,'\t');
        if(arg) {
            arg++;
            char *end=strpbrk(arg,"\r\n");
            if(end) *end=0;
            char *cursor=strchr(arg,'\t');
            if(cursor) *cursor++=0;
            if(!cursor) cursor="";
            if(decode(arg)&&(!*cursor||decode(cursor))) {
                if(!strcmp(op,"folder")) folder(out,id,arg,cursor);
                else {
                    bool ok=valid_path(arg,false)&&put(SONG_FILE,arg);
                    if(ok) { unlink(CLIP); unlink(OFFSET); draft_start=-1; }
                    header(out,id,ok?"Alarm song saved":"Unable to save song","");
                }
            } else header(out,id,"Invalid path","");
        } else header(out,id,"Missing path","");
    } else {
        fprintf(out, "Q2SPOT/1\n%s\tunsupported\t\t\n", id);
    }
    fclose(out);
}

int menu_serve(void)
{
    struct sigaction sa = { .sa_handler = stop_menu };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = SOCK };
    unlink(SOCK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 4)) {
        close(fd);
        unlink(SOCK);
        return 1;
    }
    while (!stopped) {
        alarm_poll();
        struct pollfd waitfd={.fd=fd,.events=POLLIN};
        if(poll(&waitfd,1,250)<=0) continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        reply(client);
        close(client);
    }
    close(fd);
    unlink(SOCK);
    return 0;
}
