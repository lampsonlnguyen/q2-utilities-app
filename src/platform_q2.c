/* SPDX-License-Identifier: GPL-3.0-only
 * Framebuffer and input integration after q2-groovebox by Diamond Bond,
 * which follows q2-fidget (Buddy).
 */
#include "app.h"
#include "browse.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define INPUT_SOCKET "/tmp/q2-utilities-app.input.sock"
#define ERROR_FILE "/mnt/mmc/.utilities-app/launch-error.txt"
#define TASK_FILE "/mnt/mmc/.utilities-app/tasks.tsv"

static volatile sig_atomic_t stopping;

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int input_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = INPUT_SOCKET };
    unlink(INPUT_SOCKET);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) == 0)
        return fd;
    close(fd);
    return -1;
}

static int launch_error(const char *message)
{
    FILE *file = fopen(ERROR_FILE, "w");
    if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
    }
    return 1;
}

static void load_config(struct app *app, int hour)
{
    char config[16384];
    size_t length=0;
    FILE *file=fopen("/mnt/data/config.ini", "r");
    if(file) {
        length=fread(config, 1, sizeof config-1, file);
        fclose(file);
    }
    config[length]='\0';
    app_theme(config, hour);
    app_wheel_config(app, config);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--menu"))
        return menu_serve();
    const char *uri = getenv("Q2_URI");
    enum screen selected = HOME;
    if (uri && !menu_screen(uri, &selected))
        return 1;
    unlink(ERROR_FILE);
    struct sigaction action = { .sa_handler = stop };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0)
        return launch_error("Could not open /dev/fb0");
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int result = 1;
    const char *error = "Could not read framebuffer settings";
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
        goto close_fb;
    error = "Unsupported framebuffer dimensions or pixel format";
    if (var.xres != HEIGHT || var.yres != WIDTH || var.xoffset ||
        var.bits_per_pixel != 32 || var.red.offset != 16 ||
        fix.line_length % 4 || fix.line_length < HEIGHT * 4)
        goto close_fb;
    unsigned stride = fix.line_length / 4, front = var.yoffset, visible = front;
    unsigned rows = fix.smem_len / fix.line_length;
    if (rows > var.yres_virtual)
        rows = var.yres_virtual;
    if (rows < WIDTH || front > rows - WIDTH)
        goto close_fb;
    bool can_pan = rows >= 2 * WIDTH && (front == 0 || front == WIDTH);
    error = "Could not map the framebuffer";
    uint32_t *memory = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (memory == MAP_FAILED)
        goto close_fb;

    error = "Could not bind the Q2 Pod input socket";
    int sock = input_socket();
    if (sock < 0)
        goto unmap;
    struct app app;
    app_init(&app, TASK_FILE);
    if (uri) {
        app.screen = selected;
        if(selected==RINGTONE) ringtone_load(&app,getenv("Q2_CONTEXT"));
        if (selected == CALENDAR) {
            time_t date = time(NULL);
            struct tm today;
            if (localtime_r(&date, &today)) {
                app.year = today.tm_year + 1900;
                app.month = today.tm_mon + 1;
            }
        }
    }

    int theme_hour = -1;
    error = "Q2 Pod input socket stopped responding";
    while (!stopping && !app.quit) {
        time_t date = time(NULL);
        struct tm today = { .tm_year = 70, .tm_mday = 1 };
        struct tm local;
        if (localtime_r(&date, &local))
            today = local;
        if (theme_hour != today.tm_hour) {
            theme_hour = today.tm_hour;
            load_config(&app, theme_hour);
        }
        int64_t now = monotonic_ms();
        if (can_pan) {
            unsigned back = visible ? 0 : WIDTH;
            app_draw(&app, memory + back * stride, stride, now, date, &today);
            var.yoffset = back;
            if (ioctl(fb, FBIOPAN_DISPLAY, &var) == 0)
                visible = back;
            else
                can_pan = false;
        }
        if (!can_pan)
            app_draw(&app, memory + visible * stride, stride, now, date, &today);
        int timeout = app.screen == STOPWATCH && app.running ? 100 : 1000;
        if (app.screen == COUNTDOWN && app.extra.countdown.running) {
            /* The display shows whole seconds: redraw at the next boundary. */
            int64_t left = app.extra.countdown.due_ms - now;
            timeout = left <= 0 ? 1 : (int)((left - 1) % 1000 + 1);
        }
        if (app.screen == METRONOME && app.extra.metro.running) {
            /* Redraw on beat boundaries rather than refreshing the entire
             * LCD 20-30 times/s. The rational BPM math prevents long-term
             * drift at tempos which do not divide 60,000 evenly. */
            int64_t elapsed = now - app.extra.metro.start_ms;
            if (elapsed >= 0) {
                int bpm = app.extra.metro.bpm;
                int64_t beat = elapsed * bpm / 60000;
                int64_t next = ((beat + 1) * 60000 + bpm - 1) / bpm - elapsed;
                timeout = (int)(next < 1 ? 1 : next > 1000 ? 1000 : next);
            } else
                timeout = 10;
        }
        struct pollfd poll_input = {.fd = sock, .events = POLLIN};
        int events = poll(&poll_input, 1, timeout);
        if (events < 0 && errno != EINTR)
            break;
        if (events > 0 && !(poll_input.revents & POLLIN))
            break;
        if (events > 0 && (poll_input.revents & POLLIN)) {
            char msg[64];
            ssize_t n = recv(sock, msg, sizeof msg - 1, MSG_DONTWAIT);
            if (n > 0) {
                bool was_running=app.extra.countdown.running;
                int64_t old_due=app.extra.countdown.due_ms;
                msg[n] = '\0';
                app_input(&app, msg, monotonic_ms(), &today);
                if(selected==COUNTDOWN && (was_running!=app.extra.countdown.running || old_due!=app.extra.countdown.due_ms))
                    alarm_deadline(app.extra.countdown.running?app.extra.countdown.due_ms:0);
                if(selected==RINGTONE && app.clip_request) {
                    bool save=app.clip_request==2;
                    bool ok=ringtone_apply(&app,save);
                    app.clip_request=0;
                    app.clip_status=ok?1:-1;
                    if(ok&&save) app.quit=true;
                }
                if (uri && app.screen == HOME)
                    app.quit = true;
            }
        }
    }
    result = (stopping || app.quit) ? 0 : 1;
    if (visible != front) {
        var.yoffset = front;
        ioctl(fb, FBIOPAN_DISPLAY, &var);
    }
    close(sock);
    unlink(INPUT_SOCKET);
unmap:
    munmap(memory, fix.smem_len);
close_fb:
    close(fb);
    return result ? launch_error(error) : 0;
}
