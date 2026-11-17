/* SPDX-License-Identifier: GPL-3.0-only
 * Framebuffer and input integration after q2-groovebox by Diamond Bond,
 * which follows q2-fidget (Buddy).
 */
#include "app.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define INPUT_SOCKET "/tmp/q2-utilities.input.sock"
#define ERROR_FILE "/mnt/mmc/.utilities/launch-error.txt"
#define TASK_FILE "/mnt/mmc/.utilities/tasks.tsv"

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

static void load_theme(int hour)
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
}

int main(void)
{
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
            load_theme(theme_hour);
        }
        int64_t now = monotonic_ms();
        if (can_pan) {
            unsigned back = visible ? 0 : WIDTH;
            app_draw(&app, memory + back * stride, stride, now, &today);
            var.yoffset = back;
            if (ioctl(fb, FBIOPAN_DISPLAY, &var) == 0)
                visible = back;
            else
                can_pan = false;
        }
        if (!can_pan)
            app_draw(&app, memory + visible * stride, stride, now, &today);
        int timeout = app.screen == STOPWATCH && app.running ? 100 :
                      1000;
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
                msg[n] = '\0';
                app_input(&app, msg, monotonic_ms(), &today);
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
