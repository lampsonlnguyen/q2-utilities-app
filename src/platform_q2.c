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
#define TASK_FILE "/mnt/mmc/.utilities/tasks.tsv"

static volatile sig_atomic_t stopping;
static uint32_t canvas[WIDTH * HEIGHT];

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

static void paint(uint32_t *page, unsigned stride)
{
    for (int y = 0; y < WIDTH; ++y)
        for (int x = 0; x < HEIGHT; ++x)
            page[y * stride + x] = canvas[(HEIGHT - 1 - x) * WIDTH + y];
}

int main(void)
{
    struct sigaction action = { .sa_handler = stop };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0)
        return 1;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int result = 1;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
        goto close_fb;
    if (var.xres != HEIGHT || var.yres != WIDTH || var.yres_virtual < 2 * WIDTH ||
        (var.yoffset != 0 && var.yoffset != WIDTH) || var.xoffset ||
        var.bits_per_pixel != 32 || var.red.offset != 16 || var.green.offset != 8 ||
        var.blue.offset != 0 || fix.line_length % 4 || fix.line_length < HEIGHT * 4 ||
        (uint64_t)2 * WIDTH * fix.line_length > fix.smem_len)
        goto close_fb;
    unsigned stride = fix.line_length / 4, front = var.yoffset, back = front ? 0 : WIDTH;
    uint32_t *memory = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (memory == MAP_FAILED)
        goto close_fb;

    int sock = input_socket();
    if (sock < 0)
        goto unmap;
    struct app app;
    app_init(&app, TASK_FILE);

    while (!stopping && !app.quit) {
        time_t date = time(NULL);
        struct tm today = { .tm_year = 70, .tm_mday = 1 };
        struct tm local;
        if (localtime_r(&date, &local))
            today = local;
        app_draw(&app, canvas, monotonic_ms(), &today);
        paint(memory + back * stride, stride);
        var.yoffset = back;
        if (ioctl(fb, FBIOPAN_DISPLAY, &var) != 0)
            break;
        back = back ? 0 : WIDTH;
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
    var.yoffset = front;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    close(sock);
    unlink(INPUT_SOCKET);
unmap:
    munmap(memory, fix.smem_len);
close_fb:
    close(fb);
    return result;
}
