/* SPDX-License-Identifier: GPL-3.0-only */
#include "browse.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

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
        for (unsigned i = 0; i < sizeof tools / sizeof tools[0]; ++i)
            fprintf(out, "screen\t%s\t%s\t\t\t\n", tools[i].uri, tools[i].title);
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
