/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

/* _GNU_SOURCE enables statx but newer build headers also redirect parsing
 * to C23 symbols unavailable in the firmware's glibc 2.28. Bind explicitly
 * to the older ABI on Q2. All current formats have C99 semantics. */
#if defined(__mips__) && defined(__GLIBC__)
extern int q2_sscanf(const char *, const char *, ...) __asm__("__isoc99_sscanf")
    __attribute__((format(scanf, 2, 3)));
extern int q2_fscanf(FILE *, const char *, ...) __asm__("__isoc99_fscanf")
    __attribute__((format(scanf, 2, 3)));
#else
#define q2_sscanf sscanf
#define q2_fscanf fscanf
#endif

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
    if(!strcmp(uri,"ringtone")) { *out=RINGTONE; return true; }
    for (unsigned i = 0; i < sizeof tools / sizeof tools[0]; ++i)
        if (!strcmp(uri, tools[i].uri)) {
            *out = tools[i].screen;
            return true;
        }
    return false;
}


#define CARD "/mnt/mmc"
#define SONG_FILE CARD "/.utilities-app/alarm-song"
#define CLIP CARD "/.utilities-app/alarm.mp3"
#define OLD_CLIP CARD "/.utilities-app/alarm.wav"
#define PREVIEW CARD "/.utilities-app/preview.wav"
#define OFFSET CARD "/.utilities-app/alarm-offset"
#define QUEUE CARD "/.utilities-app/alarm.m3u"
#define PLAY "/tmp/q2-utilities-app.play"
#define STATE "/tmp/q2-utilities-app.state"
#define DEADLINE "/tmp/q2-utilities-app.timer"
#define PATH_SIZE 512

/* statx is exported by the Q2's glibc 2.28, unlike stat.
 * Its fixed-width kernel ABI avoids legacy _STAT_VER and stat layouts. */
#if defined(__mips__) && defined(__GLIBC__)
static int file_info(const char *path, unsigned *mode, uint64_t *size)
{
    struct statx st;
    if (statx(AT_FDCWD, path, 0, STATX_TYPE | STATX_SIZE, &st)) return -1;
    if ((st.stx_mask & (STATX_TYPE | STATX_SIZE)) != (STATX_TYPE | STATX_SIZE)) return -1;
    *mode = st.stx_mode;
    *size = st.stx_size;
    return 0;
}
#else
static int file_info(const char *path, unsigned *mode, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st)) return -1;
    *mode = st.st_mode;
    *size = (uint64_t)st.st_size;
    return 0;
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
    unsigned mode;
    uint64_t size;
    if(strlen(path)>=PATH_SIZE || strpbrk(path,"\r\n\t") ||
       !realpath(path,resolved) ||
       (strcmp(resolved,CARD) && strncmp(resolved,CARD "/",sizeof CARD)) ||
       file_info(resolved,&mode,&size)) return false;
    return directory?S_ISDIR(mode):S_ISREG(mode)&&audio_file(resolved);
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
    int status=0;
    struct timespec begin, current;
    clock_gettime(CLOCK_MONOTONIC,&begin);
    bool exited=false;
    while(!stopped) {
        clock_gettime(CLOCK_MONOTONIC,&current);
        if(current.tv_sec-begin.tv_sec>=120) break;
        struct pollfd p={.fd=pipefd[0],.events=POLLIN};
        if(poll(&p,1,100)>0) {
            char buf[512];
            ssize_t n=read(pipefd[0],buf,sizeof buf);
            if(n>0 && log && used<size-1) {
                size_t copy=(size_t)n<size-1-used?(size_t)n:size-1-used;
                memcpy(log+used,buf,copy); used+=copy;
            }
        }
        if(waitpid(child,&status,WNOHANG)==child) {
            exited=true;
            /* Drain remaining diagnostics after exit; duration can be in the tail. */
            char tail[512]; ssize_t n;
            while((n=read(pipefd[0],tail,sizeof tail))>0) {
                if(log && used<size-1) {
                    size_t copy=(size_t)n<size-1-used?(size_t)n:size-1-used;
                    memcpy(log+used,tail,copy); used+=copy;
                }
            }
            break;
        }
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
    if(!d||q2_sscanf(d+10,"%d:%d:%lf",&h,&m,&s)!=3) return 0;
    return h*3600+m*60+(int)s;
}
static bool make_clip(const char *song,int start,int length,const char *dest,bool mp3,char *error,size_t error_size)
{
    char offset[24],duration[24],temp[PATH_SIZE];
    if(start<0||length<5||length>30) return false;
    snprintf(offset,sizeof offset,"%d",start);
    snprintf(duration,sizeof duration,"%d",length);
    if(snprintf(temp,sizeof temp,"%s.tmp.%s",dest,mp3?"mp3":"wav")>=(int)sizeof temp) return false;
    /* Some stock FFmpeg builds ship libshine instead of libmp3lame.
     * Try both MP3 encoders, but never silently export a WAV as MP3. */
    char *args[]={"ffmpeg","-nostdin","-v","error","-y","-ss",offset,"-i",(char *)song,
                  "-t",duration,"-map","0:a:0","-vn","-ac","1","-ar","44100",
                  "-c:a",mp3?"libmp3lame":"pcm_s16le","-b:a","96k","-map_metadata","-1",temp,NULL};
    unsigned mode;
    uint64_t size;
    char log[2048];
    int result=ffmpeg(args,log,sizeof log);
    if(mp3 && result!=0 && strstr(log,"Unknown encoder")) {
        for(unsigned i=0;args[i];i++)
            if(!strcmp(args[i],"libmp3lame")) { args[i]="libshine"; break; }
        unlink(temp);
        result=ffmpeg(args,log,sizeof log);
    }
    if(result==0 && !file_info(temp,&mode,&size) && S_ISREG(mode) && size>44 && !rename(temp,dest)) return true;
    if(error&&error_size) {
        const char *reason=strstr(log,"Unknown encoder")?"MP3 encoder unavailable":
            strstr(log,"Permission denied")?"Storage permission denied":
            strstr(log,"No space left")?"SD card full":
            result==127?"FFmpeg unavailable":result<0?"Audio conversion timed out":
            result==0?"Cannot save audio file":"Audio conversion failed";
        snprintf(error,error_size,"%s",reason);
        put(CARD "/.utilities-app/ringtone-error.txt",log);
    }
    unlink(temp);
    return false;
}static int saved_offset(int *length)
{
    FILE *f=fopen(OFFSET,"r");
    int n=0,l=10;
    if(f) { if(q2_fscanf(f,"%d %d",&n,&l)<1) n=0; fclose(f); }
    if(length) *length=l<5?5:l>30?30:l;
    return n<0?0:n;
}static bool play_path(const char *song)
{
    char line[128],list[PATH_SIZE+2];
    if(access(song,R_OK)) return false;
    if(snprintf(list,sizeof list,"%s\n",song)>=(int)sizeof list) return false;
    if(!put(QUEUE,list)) return false;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    snprintf(line,sizeof line,"%ld%09ld %s\n",(long)ts.tv_sec,ts.tv_nsec,QUEUE);
    return put(PLAY,line);
}

bool ringtone_load(struct app *app,const char *context)
{
    char song[PATH_SIZE];
    if(context && *context) {
        if(!valid_path(context,false)) return false;
        snprintf(song,sizeof song,"%s",context);
    } else if(!selected_song(song,sizeof song)) return false;
    snprintf(app->clip_path,sizeof app->clip_path,"%s",song);
    snprintf(app->clip_title,sizeof app->clip_title,"%.79s",base(song));
    int duration=song_length(song);
    if(duration<5||duration>86400) return false;
    app->clip_duration=duration;
    char old[PATH_SIZE];
    app->clip_length=10;
    app->clip_start=selected_song(old,sizeof old)&&!strcmp(old,song)?saved_offset(&app->clip_length):0;
    if(app->clip_length>duration) app->clip_length=duration;
    if(app->clip_start>duration-app->clip_length) app->clip_start=duration-app->clip_length;
    /* Downsample on-device to 1 kHz mono PCM; stream into 80 peak bins.
     * The temporary file avoids retaining decoded audio in RAM. */
    char raw[80];
    snprintf(raw,sizeof raw,"/tmp/q2-utilities-wave.%ld.raw",(long)getpid());
    char *args[]={"ffmpeg","-nostdin","-v","error","-y","-i",song,
        "-map","0:a:0","-vn","-ac","1","-ar","1000","-f","s16le",(char *)raw,NULL};
    if(ffmpeg(args,NULL,0)!=0) { unlink(raw); app->clip_status=-1; return true; }
    FILE *f=fopen(raw,"rb");
    if(f) {
        unsigned char samples[2048]; size_t n; uint64_t pos=0;
        unsigned peaks[320]={0},peak=1;
        while((n=fread(samples,1,sizeof samples,f))>0) {
            for(size_t i=0;i+1<n;i+=2,pos++) {
                unsigned bin=(unsigned)(pos*320/((uint64_t)duration*1000));
                if(bin>=320) bin=319;
                int v=(int16_t)(samples[i]|((unsigned)samples[i+1]<<8));
                unsigned mag=(unsigned)(v<0?-v:v);
                if(mag>peaks[bin]) peaks[bin]=mag;
                if(mag>peak) peak=mag;
            }
        }
        fclose(f);
        for(int i=0;i<320;i++) app->clip_wave[i]=(unsigned char)(peaks[i]*255/peak);
        app->clip_wave_ready=pos>0;
    } else app->clip_status=-1;
    unlink(raw);
    return true;
}
bool ringtone_apply(struct app *app,bool save)
{
    char value[48];
    app->clip_error[0]=0;
    if(!valid_path(app->clip_path,false)||app->clip_start<0||app->clip_length<5||
       app->clip_length>30||app->clip_start>app->clip_duration-app->clip_length) return false;
    if(!save) {
        if(!make_clip(app->clip_path,app->clip_start,app->clip_length,PREVIEW,false,app->clip_error,sizeof app->clip_error)) return false;
        if(play_path(PREVIEW)) return true;
        snprintf(app->clip_error,sizeof app->clip_error,"Cannot request playback");
        return false;
    }
    /* Commit the audio last. An encoding failure never replaces the alarm. */
    const char *candidate=CARD "/.utilities-app/alarm.pending.mp3";
    if(!make_clip(app->clip_path,app->clip_start,app->clip_length,candidate,true,app->clip_error,sizeof app->clip_error)) return false;
    snprintf(value,sizeof value,"%d %d",app->clip_start,app->clip_length);
    if(!put(SONG_FILE,app->clip_path)||!put(OFFSET,value)||rename(candidate,CLIP)) {
        unlink(candidate);
        snprintf(app->clip_error,sizeof app->clip_error,"Cannot save ringtone on SD card");
        return false;
    }
    unlink(OLD_CLIP);
    unlink(CARD "/.utilities-app/ringtone-error.txt");
    return true;
}
bool alarm_play(void)
{
    if(access(CLIP,R_OK)==0) return play_path(CLIP);
    if(access(OLD_CLIP,R_OK)==0) return play_path(OLD_CLIP);
    return false;
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
    int ok=q2_fscanf(f,"%lld",&due);
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
/* Avoid atoi/strtol: newer glibc inlines atoi through __isoc23_strtol.
 * Protocol escapes are exactly two hex digits, so parse them directly. */
static int hex_digit(unsigned char c)
{
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool decode(char *s)
{
    char *w=s;
    for(char *p=s;*p;p++) {
        if(*p=='%') {
            if(!p[1]||!p[2]) return false;
            int hi=hex_digit((unsigned char)p[1]);
            int lo=hex_digit((unsigned char)p[2]);
            if(hi<0||lo<0||(hi==0&&lo==0)) return false;
            int n=hi*16+lo;
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
    bool saved=access(CLIP,R_OK)==0||access(OLD_CLIP,R_OK)==0;
    row(out,"info","","Timer alarm",saved?"Saved ringtone":"No ringtone saved","");
    if(saved && selected_song(path,sizeof path)) row(out,"info","","Source song",base(path),"");
    row(out,"list","folder","Choose song / Make ringtone","",CARD);
    if(saved && selected_song(path,sizeof path)) row(out,"screen","ringtone","Edit saved ringtone","","");
    if(saved) {
        row(out,"action","preview","Preview saved ringtone","","");
        row(out,"action","clear","Clear timer ringtone","","");
    }
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
        row(out,entries[i].dir?"list":"screen",entries[i].dir?"folder":"ringtone",entries[i].name,"",full);
    }
    if(!n) row(out,"info","","No music or folders","","");
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
        q2_sscanf(request + 9, "%23[0-9]\t%23[^\t\r\n]", id, op) != 2)
        return;
    FILE *out = fdopen(dup(fd), "w");
    if (!out) return;
    if (!strcmp(op, "menu")) {
        fprintf(out, "Q2SPOT/1\n%s\tok\t\t\n", id);
        for (unsigned i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
            row(out,"screen",tools[i].uri,tools[i].title,"","");
        }
        row(out,"list","sound","Ringtone Editor","","");
    } else if(!strcmp(op,"timer")) {
        header(out,id,"","");
        row(out,"screen","timer","Set Timer","","");
        row(out,"list","sound","Ringtone Editor","","");
    } else if(!strcmp(op,"sound")) {
        sound_menu(out,id);
    } else if(!strcmp(op,"preview")) {
        header(out,id,alarm_play()?"Playback requested":"No saved ringtone","");
    } else if(!strcmp(op,"clear")) {
        unlink(SONG_FILE); unlink(CLIP); unlink(OLD_CLIP); unlink(OFFSET);
        header(out,id,"Selection cleared","");
    } else if(!strcmp(op,"folder")) {
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
                folder(out,id,arg,cursor);
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
