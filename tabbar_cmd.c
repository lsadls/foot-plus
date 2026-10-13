#include "tabbar_cmd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <stddef.h>

#define LOG_MODULE "tabbar_cmd"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "fdm.h"
#include "render.h"
#include "spawn.h"
#include "terminal.h"
#include "wayland.h"
#include "xmalloc.h"

#define TEXT_MAX 8192

static void refresh_tab_bar(struct wl_window *win)
{
    /* Skip the (relatively expensive) bar re-render while the command
     * output is unchanged; output_text() clears the flag on change */
    if (!win->tabbar_cmd.changed)
        return;
    win->tabbar_cmd.changed = false;
    LOG_INFO("tabbar refresh: last_line=%s",
             win->tabbar_cmd.last_line ? win->tabbar_cmd.last_line : "(null)");
    render_refresh_tab_bar(win);
}

static void
append_text(struct wl_window *win, const char *buf, size_t n)
{
    struct tabbar_cmd *cmd = &win->tabbar_cmd;

    size_t total = cmd->text_len + n;
    if (total > TEXT_MAX) {
        const size_t drop = total - TEXT_MAX;
        const size_t keep = cmd->text_len > drop ? cmd->text_len - drop : 0;
        memmove(cmd->text, cmd->text + drop, keep);
        cmd->text_len = keep;
        total = TEXT_MAX;
    }

    char *new_text = realloc(cmd->text, total + 1);
    if (new_text == NULL)
        return;
    cmd->text = new_text;
    memcpy(cmd->text + cmd->text_len, buf, n);
    cmd->text_len += n;
    cmd->text[cmd->text_len] = '\0';
    cmd->changed = true;
    /* Show new output immediately instead of waiting for the next
     * periodic refresh tick */
    render_refresh_tab_bar(win);
}

static bool
on_stdout(struct fdm *fdm, int fd, int events, void *data)
{
    struct wl_window *win = data;

    if (events & EPOLLHUP) {
        /* Short-lived commands (e.g. "date") exit immediately: drain the
         * remaining output before dropping the fd, or it is lost. */
        drain: ;
        char buf[512];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            LOG_DBG("drained %zd bytes: %.*s", n, (int)n, buf);
            append_text(win, buf, n);
        }

        LOG_DBG("HUP on fd=%d, fd dropped", fd);

        fdm_del(fdm, fd);  /* closes fd */
        win->tabbar_cmd.stdout_fd = -1;
        refresh_tab_bar(win);
        return true;
    }

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return true;
        LOG_ERRNO("tab bar command: read");
        return false;
    }
    if (n == 0)
        goto drain;

    append_text(win, buf, n);

    return true;
}

static bool
on_refresh_timer(struct fdm *fdm, int fd, int events, void *data)
{
    struct wl_window *win = data;

    uint64_t ticks;
    ssize_t n = read(fd, &ticks, sizeof(ticks));
    (void)n;

    /* Coalesce output into display lines once per second */
    refresh_tab_bar(win);
    return true;
}

static void
on_reap(struct reaper *reaper, pid_t pid, int status, void *data)
{
    struct wl_window *win = data;
    LOG_DBG("tab bar command pid=%d exited with status=%d", pid, status);
    win->tabbar_cmd.stdout_fd = -1;
    win->tabbar_cmd.pid = -1;
}

static void
stop(struct wl_window *win)
{
    struct tabbar_cmd *cmd = &win->tabbar_cmd;
    struct fdm *fdm = win->term->fdm;

    if (cmd->pid > 0) {
        /* The child called setsid() (see spawn()), so it leads its own
         * process group; signal the whole group so any children it
         * spawned (e.g. a shell running `status`) exit too. */
        if (kill(-cmd->pid, SIGTERM) < 0 && errno != ESRCH)
            LOG_ERRNO("tabbar_cmd: kill");
        cmd->pid = -1;
    }
    if (cmd->stdout_fd >= 0) {
        fdm_del(fdm, cmd->stdout_fd);  /* closes fd */
        cmd->stdout_fd = -1;
    }
    if (cmd->refresh_fd >= 0) {
        fdm_del(fdm, cmd->refresh_fd);
        cmd->refresh_fd = -1;
    }
}

void
tabbar_cmd_init(struct wl_window *win)
{
    struct tabbar_cmd *cmd = &win->tabbar_cmd;
    struct terminal *term = win->term;
    const struct config *conf = term->conf;

    cmd->pid = -1;
    cmd->stdout_fd = -1;
    cmd->refresh_fd = -1;
    cmd->text = NULL;
    cmd->text_len = 0;
    cmd->last_line = NULL;
    cmd->changed = false;

    if (conf->tabs.bar_command.argv.args == NULL ||
        conf->tabs.bar_command.argv.args[0] == NULL)
        return;

    cmd->refresh_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (cmd->refresh_fd < 0) {
        LOG_ERRNO("tabbar_cmd: timerfd_create");
        return;
    }

    static const struct itimerspec timer = {
        .it_interval = {.tv_sec = 1, .tv_nsec = 0},
        .it_value = {.tv_sec = 1, .tv_nsec = 0},
    };
    if (timerfd_settime(cmd->refresh_fd, 0, &timer, NULL) < 0) {
        LOG_ERRNO("tabbar_cmd: timerfd_settime");
        goto err;
    }
    if (!fdm_add(term->fdm, cmd->refresh_fd, EPOLLIN, &on_refresh_timer, win))
        goto err;

    int out_pipe[2] = {-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) < 0) {
        LOG_ERRNO("tabbar_cmd: pipe2");
        goto err;
    }

    const pid_t pid = spawn(term->reaper, term->cwd,
                            conf->tabs.bar_command.argv.args,
                            -1, out_pipe[1], -1, &on_reap, win, NULL);
    close(out_pipe[1]);  /* child owns its end now (O_CLOEXEC anyway) */
    if (pid < 0) {
        LOG_ERR("tabbar_cmd: failed to spawn command");
        close(out_pipe[0]);
        goto err;
    }
    LOG_DBG("tab bar command spawned pid=%d read_fd=%d", pid, out_pipe[0]);
    cmd->pid = pid;

    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    if (!fdm_add(term->fdm, out_pipe[0], EPOLLIN, &on_stdout, win)) {
        close(out_pipe[0]);
        goto err;
    }
    cmd->stdout_fd = out_pipe[0];
    return;

err:
    stop(win);
}

void
tabbar_cmd_destroy(struct wl_window *win)
{
    stop(win);
    free(win->tabbar_cmd.text);
    win->tabbar_cmd.text = NULL;
    win->tabbar_cmd.text_len = 0;
    free(win->tabbar_cmd.last_line);
    win->tabbar_cmd.last_line = NULL;
    win->tabbar_cmd.changed = false;
}

/* The most recent non-empty line of the bar command's output, or NULL */
/* Not const: caches the line (last_line/changed) for the refresh skip */
char *
tabbar_cmd_last_line(struct wl_window *win)
{
    const char *text = win->tabbar_cmd.text;
    if (text == NULL || text[0] == '\0')
        return NULL;

    /* "20:45\n" → "20:45" */
    const char *end = text + strlen(text);
    while (end > text && end[-1] == '\n')
        end--;

    const char *start = text;
    for (const char *p = text; p < end; p++)
        if (*p == '\n')
            start = p + 1;

    if (start >= end)
        return NULL;

    const size_t len = (size_t)(end - start);

    /* Remember the line so the periodic refresh can skip unchanged
     * output (see refresh_tab_bar()) */
    struct tabbar_cmd *cmd = &win->tabbar_cmd;
    if (cmd->last_line == NULL || strlen(cmd->last_line) != len ||
        memcmp(cmd->last_line, start, len) != 0)
    {
        free(cmd->last_line);
        cmd->last_line = xmalloc(len + 1);
        memcpy(cmd->last_line, start, len);
        cmd->last_line[len] = '\0';
        cmd->changed = true;
    }

    char *line = xmalloc(len + 1);
    memcpy(line, start, len);
    line[len] = '\0';
    return line;
}

const char *
tabbar_cmd_text(const struct wl_window *win)
{
    return win->tabbar_cmd.text;
}
