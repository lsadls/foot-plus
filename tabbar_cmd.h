#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

struct wl_window;

/*
 * Renders the stdout of a user-configured command in the right half
 * of the tab bar. The command is spawned once per window (not per
 * tab); new output is appended to an internal buffer, and the tab bar
 * is refreshed at most once per second.
 */

struct tabbar_cmd {
    pid_t pid;       /* -1 if not running */
    int stdout_fd;   /* -1 if not running */
    int refresh_fd;  /* timerfd, 1s period; -1 if not armed */
    char *text;      /* last N bytes of output, NUL-terminated */
    size_t text_len;
    char *last_line; /* last line as of the previous render; used to
                        skip tab bar redraws when output is unchanged */
    bool changed;    /* last_line changed since the last refresh */
};

void tabbar_cmd_init(struct wl_window *win);
void tabbar_cmd_destroy(struct wl_window *win);

/* Text to draw (may be NULL); owned by tabbar_cmd */
const char *tabbar_cmd_text(const struct wl_window *win);

/* The most recent non-empty output line (caller frees), or NULL */
char *tabbar_cmd_last_line(struct wl_window *win);
