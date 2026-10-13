#include "tabs.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "tabs"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "config.h"
#include "debug.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "search.h"
#include "tabbar_cmd.h"
#include "terminal.h"
#include "url-mode.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

static void
tab_shutdown_cb(void *data, int exit_code)
{
    /* Tabs own a private copy of the configuration */
    struct config *conf = data;
    config_free(conf);
    free(conf);
}

bool
tab_new(struct terminal *term)
{
    struct wl_window *win = term->window;

    if (!win->is_configured)
        return false;

    /*
     * The configuration may be owned by another terminal instance
     * (e.g. a footclient instance with command line overrides), that
     * may exit before this tab does.
     */
    struct config *conf = config_clone(term->conf);

    /* New tabs always start in the home directory, not the current
     * tab's CWD (which would inherit whatever directory the shell in
     * the existing tab happens to be in) */
    const char *tab_cwd = getenv("HOME");
    if (tab_cwd == NULL)
        tab_cwd = "/";

    struct terminal *tab = term_init(
        conf, term->fdm, term->reaper, term->wl, term->foot_exe, tab_cwd,
        NULL, NULL, 0, NULL, NULL, win, &tab_shutdown_cb, conf);

    if (tab == NULL) {
        LOG_ERR("failed to instantiate new tab");
        config_free(conf);
        free(conf);
        return false;
    }

    /* term_init() appended the tab; move it next to the current one */
    if (term->conf->tabs.new_position == TABS_NEW_POSITION_AFTER_CURRENT) {
        /*
         * Bubble it towards the front, until it's right after the
         * current tab (swapping items; the list nodes stay put).
         */
        xassert(tll_back(win->tabs) == tab);

        tll_rforeach(win->tabs, it) {
            if (it->prev == NULL || it->prev->item == term)
                break;

            it->item = it->prev->item;
            it->prev->item = tab;
        }
    }

    /* Size the grid before we start reading from the PTY */
    tab_activate(tab);
    term_window_configured(tab);
    return true;
}

static void
switch_seat_focus(struct terminal *old, struct terminal *new)
{
    bool had_kbd_focus = false;

    new->active_surface = old->active_surface;
    old->active_surface = TERM_SURF_NONE;

    tll_foreach(new->wl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->kbd_focus == old) {
            ime_reset_preedit(seat);
            seat->kbd_focus = new;
            had_kbd_focus = true;
        }

        if (seat->mouse_focus == old) {
            seat->mouse_focus = new;

            /* The new tab's grid geometry may differ */
            if (new->active_surface == TERM_SURF_GRID && !new->shutdown.in_progress)
                mouse_coord_pixel_to_cell(seat, new, seat->mouse.x, seat->mouse.y);
        }

        if (seat->ime_focus == old) {
            ime_disable(seat);
            seat->ime_focus = new;
            ime_enable(seat);
        }
    }

    if (old->visual_focus)
        term_visual_focus_in(new);
    else
        term_visual_focus_out(new);

    if (had_kbd_focus)
        term_kbd_focus_in(new);

    /* A tab being closed has already closed its PTY */
    if (!old->shutdown.in_progress) {
        if (had_kbd_focus)
            term_kbd_focus_out(old);
        term_visual_focus_out(old);
    }
}

void
tab_activate(struct terminal *new)
{
    struct wl_window *win = new->window;
    struct terminal *old = win->term;

    if (new == old)
        return;

    LOG_DBG("activating tab %p (previous: %p)", (void *)new, (void *)old);

    new->tab.activated = ++win->tab_activations;
    new->tab.attention = false;
    new->tab.output_pending = false;
    tab_update_comm(new);

    /* The window's size, which the new tab will be resized to */
    const float logical_width = old->scale > 0. ? old->width / old->scale : 0.;
    const float logical_height = old->scale > 0. ? old->height / old->scale : 0.;

    /* These modes use window-global surfaces */
    if (old->tab.rename.active)
        tab_rename_cancel(old);
    render_flush_pending_resize(old);
    if (old->is_searching)
        search_cancel(old);
    if (urls_mode_is_active(old))
        urls_reset(old);

    win->term = new;

    /* Font size changes in one tab apply to all tabs. Must be done
     * before resizing, since it changes the cell size */
    if (new->conf->tabs.shared_font_size && !new->shutdown.in_progress)
        term_font_size_copy(new, old);

    /* A new tab doesn't have a grid until it has been sized; must be
     * done before giving it keyboard focus */
    if (!new->shutdown.in_progress && logical_width > 0. && logical_height > 0.)
        wayl_win_tab_sync_size(new, logical_width, logical_height, true);

    switch_seat_focus(old, new);

    /* The window surface holds the previous tab's content */
    if (old != NULL && old != new && !old->shutdown.in_progress)
        term_release_render_buffers(old);

    /* E.g. the window is being closed, and all tabs are shutting down */
    if (new->shutdown.in_progress)
        return;

    /* The window surface holds the previous tab's content */
    render_refresh_full(new);
    render_refresh_csd(new);
    render_refresh_title(new);
    render_refresh_app_id(new);
    render_refresh_icon(new);
    render_refresh_tab_bar(win);
    wayl_win_alpha_changed(win);

    tll_foreach(new->wl->seats, it) {
        if (it->item.mouse_focus == new)
            term_xcursor_update_for_seat(new, &it->item);
    }
}

struct terminal *
tab_at_index(const struct wl_window *win, size_t idx)
{
    size_t i = 0;
    tll_foreach(win->tabs, it) {
        if (i++ == idx)
            return it->item;
    }
    return NULL;
}

void
tab_activate_index(struct wl_window *win, size_t idx)
{
    struct terminal *tab = tab_at_index(win, idx);
    if (tab != NULL)
        tab_activate(tab);
}

void
tab_activate_named(struct wl_window *win, unsigned char name)
{
    LOG_INFO("tab-goto: name=%c", name);
    tll_foreach(win->tabs, it) {
        LOG_INFO("tab-goto: tab name=%c hidden=%d comm=%s",
                 it->item->tab.name ? it->item->tab.name : '-',
                 it->item->tab.hidden,
                 it->item->tab.comm ? it->item->tab.comm : "(null)");
        if (it->item->tab.name == name) {
            tab_activate(it->item);
            return;
        }
    }
    LOG_INFO("tab-goto: no tab named %c", name);
}

void
tab_activate_last(struct wl_window *win)
{
    tab_activate_index(win, tll_length(win->tabs) - 1);
}

/* The most recently active tab, other than the active one, that isn't
 * being closed */
static struct terminal *
last_used_tab(const struct wl_window *win)
{
    struct terminal *last = NULL;

    tll_foreach(win->tabs, it) {
        struct terminal *tab = it->item;

        if (tab == win->term || tab->shutdown.in_progress)
            continue;

        if (last == NULL || tab->tab.activated > last->tab.activated)
            last = tab;
    }

    return last;
}

void
tab_activate_last_used(struct wl_window *win)
{
    struct terminal *tab = last_used_tab(win);
    if (tab != NULL)
        tab_activate(tab);
}

void
tab_mark_attention(struct terminal *term)
{
    struct wl_window *win = term->window;

    /* The active tab is already being looked at */
    if (win == NULL || win->term == term || term->tab.attention)
        return;

    term->tab.attention = true;
    render_refresh_tab_bar(win);
}

void
tab_move(struct wl_window *win, int direction)
{
    tll_foreach(win->tabs, it) {
        if (it->item != win->term)
            continue;

        /* Doesn't wrap around */
        __typeof__(it) other = direction < 0 ? it->prev : it->next;
        if (other == NULL)
            return;

        it->item = other->item;
        other->item = win->term;
        render_refresh_tab_bar(win);
        return;
    }
}

/* Moves the active tab to position 'idx' */
void
tab_move_to(struct wl_window *win, size_t idx)
{
    const size_t count = tll_length(win->tabs);
    const size_t target = min(idx, count - 1);

    size_t current = 0;
    tll_foreach(win->tabs, it) {
        if (it->item == win->term)
            break;
        current++;
    }

    for (; current < target; current++)
        tab_move(win, 1);
    for (; current > target; current--)
        tab_move(win, -1);
}

void
tab_move_last(struct wl_window *win)
{
    tab_move_to(win, tll_length(win->tabs) - 1);
}

/*
 * Moves the active tab to the tab bar position under 'x' (which may
 * be outside the tab bar, while dragging).
 */
void
tab_drag(struct wl_window *win, int x)
{
    const int count = tll_length(win->tabs);

    if (count < 2)
        return;

    const int idx = tab_bar_tab_at(win, x);
    if (idx < 0)
        return;

    tab_move_to(win, idx);
}

/*
 * The tab's label: basename of the foreground process' comm, falling
 * back to the user assigned name, the window title, or "foot".
 */
const char *
tab_title(const struct terminal *term)
{
    if (term->tab.comm != NULL)
        return term->tab.comm;
    if (term->tab.title != NULL)
        return term->tab.title;
    return term->window_title != NULL ? term->window_title : "foot";
}

/* Basename of the foreground process group's comm, or NULL */
static char *
read_foreground_comm(int ptmx)
{
    const pid_t pgrp = tcgetpgrp(ptmx);
    if (pgrp <= 0)
        return NULL;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pgrp);

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return NULL;

    char buf[256];
    char *comm = fgets(buf, sizeof(buf), f) != NULL
        ? xstrdup(buf) : NULL;
    fclose(f);
    if (comm == NULL)
        return NULL;

    /* Strip trailing newline */
    size_t len = strlen(comm);
    while (len > 0 && (comm[len - 1] == '\n' || comm[len - 1] == '\r'))
        comm[--len] = '\0';

    /* Basename. Note: comm can contain '/' (e.g. kthreadd names) */
    char *slash = strrchr(comm, '/');
    if (slash != NULL && slash[1] != '\0')
        memmove(comm, slash + 1, strlen(slash + 1) + 1);

    if (comm[0] == '\0') {
        free(comm);
        return NULL;
    }
    return comm;
}

/*
 * Refresh the cached foreground process name. Called when the tab
 * produces output, and when it is activated.
 */
void
tab_update_comm(struct terminal *term)
{
    __typeof__(term->tab) *tab = &term->tab;

    if (term->ptmx < 0) {
        free(tab->comm);
        tab->comm = NULL;
        tab->comm_pid = -1;
        return;
    }

    const pid_t pgrp = tcgetpgrp(term->ptmx);
    if (pgrp <= 0)
        return;
    if (pgrp == tab->comm_pid && tab->comm != NULL)
        return;

    char *raw = read_foreground_comm(term->ptmx);
    if (raw == NULL)
        return;

    /* Login shells are named "-bash" */
    const char *name = raw[0] == '-' && raw[1] != '\0' ? raw + 1 : raw;

    char *comm = xstrdup(name);
    free(raw);

    free(tab->comm);
    tab->comm = comm;
    tab->comm_pid = pgrp;
    if (term->window != NULL)
        render_refresh_tab_bar(term->window);
}

static void
rename_append(struct terminal *term, char32_t wc)
{
    static const size_t max_len = 256;
    __typeof__(term->tab.rename) *r = &term->tab.rename;

    if (r->len >= max_len)
        return;

    if (r->len + 1 >= r->sz) {
        r->sz = r->sz == 0 ? 64 : r->sz * 2;
        r->buf = xrealloc(r->buf, r->sz * sizeof(r->buf[0]));
    }

    r->buf[r->len++] = wc;
    r->buf[r->len] = U'\0';
}

static void
rename_end(struct terminal *term)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;

    free(r->buf);
    r->buf = NULL;
    r->len = r->sz = 0;
    r->active = false;

    /* A single tab's bar was only shown while renaming */
    render_refresh_tab_bar(term->window);
}

void
tab_rename_start(struct terminal *term)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;

    if (r->active || term->shutdown.in_progress)
        return;

    if (term->is_searching)
        search_cancel(term);
    urls_reset(term);

    r->active = true;

    /* Start with the current label, so that it can be edited */
    const char *title = term->tab.title != NULL
        ? term->tab.title : term->window_title;
    char32_t *wcs = ambstoc32(title);

    if (wcs != NULL) {
        for (const char32_t *wc = wcs; *wc != U'\0'; wc++)
            rename_append(term, *wc);
        free(wcs);
    }

    render_refresh_tab_bar(term->window);
}

void
tab_rename_commit(struct terminal *term)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;
    xassert(r->active);

    free(term->tab.title);
    term->tab.title = r->len > 0 ? ac32tombs(r->buf) : NULL;
    rename_end(term);
}

void
tab_rename_cancel(struct terminal *term)
{
    xassert(term->tab.rename.active);
    rename_end(term);
}

void
tab_rename_input(struct seat *seat, struct terminal *term, uint32_t key,
                 xkb_keysym_t sym)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;
    xassert(r->active);

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        tab_rename_commit(term);
        return;

    case XKB_KEY_Escape:
        tab_rename_cancel(term);
        return;

    case XKB_KEY_BackSpace:
        if (seat->kbd.ctrl) {
            /* Delete previous word */
            while (r->len > 0 && r->buf[r->len - 1] == U' ')
                r->len--;
            while (r->len > 0 && r->buf[r->len - 1] != U' ')
                r->len--;
        } else if (r->len > 0)
            r->len--;

        if (r->buf != NULL)
            r->buf[r->len] = U'\0';
        render_refresh_tab_bar(term->window);
        return;
    }

    if (seat->kbd.ctrl) {
        switch (sym) {
        case XKB_KEY_u:
            /* Clear */
            r->len = 0;
            if (r->buf != NULL)
                r->buf[0] = U'\0';
            render_refresh_tab_bar(term->window);
            break;

        case XKB_KEY_c:
        case XKB_KEY_g:
            tab_rename_cancel(term);
            break;
        }

        /* Ignore all other control sequences */
        return;
    }

    struct xkb_compose_state *compose = seat->kbd.xkb_compose_state;
    const enum xkb_compose_status compose_status = compose != NULL
        ? xkb_compose_state_get_status(compose)
        : XKB_COMPOSE_NOTHING;

    char buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(compose, buf, sizeof(buf));
        xkb_compose_state_reset(compose);
    } else if (compose_status == XKB_COMPOSE_NOTHING)
        count = xkb_state_key_get_utf8(seat->kbd.xkb_state, key, buf, sizeof(buf));

    if (count <= 0 || count >= (int)sizeof(buf))
        return;

    char32_t *wcs = ambstoc32(buf);
    if (wcs == NULL)
        return;

    for (const char32_t *wc = wcs; *wc != U'\0'; wc++) {
        /* Skip control characters */
        if (*wc < 0x20 || (*wc >= 0x7f && *wc < 0xa0))
            continue;
        rename_append(term, *wc);
    }

    free(wcs);
    render_refresh_tab_bar(term->window);
}

void
tab_cycle(struct wl_window *win, int direction)
{
    const size_t count = tll_length(win->tabs);
    if (count < 2)
        return;

    /* Cycle over non-hidden tabs only */
    size_t idx = 0;
    tll_foreach(win->tabs, it) {
        if (it->item == win->term)
            break;
        idx++;
    }

    xassert(idx < count);

    for (size_t steps = 0; steps < count; steps++) {
        long next = ((long)idx + direction) % (long)count;
        if (next < 0)
            next += count;
        idx = (size_t)next;

        struct terminal *tab = tab_at_index(win, idx);
        if (tab != NULL && !tab->tab.hidden) {
            tab_activate(tab);
            return;
        }
    }
}

static bool
in_close_scope(enum tab_close_scope scope, bool is_active, bool after_active)
{
    switch (scope) {
    case TAB_CLOSE_NONE:   return false;
    case TAB_CLOSE_ALL:    return true;
    case TAB_CLOSE_LEFT:   return !is_active && !after_active;
    case TAB_CLOSE_RIGHT:  return after_active;
    case TAB_CLOSE_OTHERS: return !is_active;
    }

    BUG("unhandled tab close scope: %d", scope);
    return false;
}

size_t
tab_close_count(const struct wl_window *win, enum tab_close_scope scope)
{
    size_t count = 0;
    bool after_active = false;

    tll_foreach(win->tabs, it) {
        const struct terminal *tab = it->item;
        const bool is_active = tab == win->term;

        if (!tab->shutdown.in_progress &&
            in_close_scope(scope, is_active, after_active))
        {
            count++;
        }

        if (is_active)
            after_active = true;
    }

    return count;
}

void
tab_close(struct wl_window *win, enum tab_close_scope scope)
{
    bool after_active = false;

    /* term_shutdown() is asynchronous; the tabs are detached later */
    tll_foreach(win->tabs, it) {
        struct terminal *tab = it->item;
        const bool is_active = tab == win->term;

        if (in_close_scope(scope, is_active, after_active))
            term_shutdown(tab);

        if (is_active)
            after_active = true;
    }
}

void
tab_request_close(struct wl_window *win, enum tab_close_scope scope)
{
    struct terminal *term = win->term;
    xassert(scope != TAB_CLOSE_NONE);

    const bool confirm = scope == TAB_CLOSE_ALL
        ? term->conf->tabs.confirm_close
        : term->conf->tabs.confirm_close_multiple;

    /* Requesting the same close while asking, closes immediately */
    if (!confirm ||
        win->confirm_close == scope ||
        tab_close_count(win, scope) < 2)
    {
        tab_close(win, scope);
        return;
    }

    /* All keyboard input goes to the prompt; leave other input modes */
    if (term->tab.rename.active)
        tab_rename_cancel(term);
    if (term->is_searching)
        search_cancel(term);
    urls_reset(term);

    win->confirm_close = scope;
    render_refresh(term);
}

void
tab_request_close_window(struct wl_window *win)
{
    tab_request_close(win, TAB_CLOSE_ALL);
}

static void
confirm_close_dismiss(struct wl_window *win)
{
    win->confirm_close = TAB_CLOSE_NONE;
    render_refresh(win->term);
}

void
tab_confirm_close_input(struct terminal *term, xkb_keysym_t sym)
{
    struct wl_window *win = term->window;
    const enum tab_close_scope scope = win->confirm_close;
    xassert(scope != TAB_CLOSE_NONE);

    switch (sym) {
    case XKB_KEY_y:
    case XKB_KEY_Y:
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        /* Unless closing the window, the active tab stays open */
        if (scope != TAB_CLOSE_ALL)
            confirm_close_dismiss(win);
        tab_close(win, scope);
        break;

    case XKB_KEY_n:
    case XKB_KEY_N:
    case XKB_KEY_Escape:
        confirm_close_dismiss(win);
        break;
    }
}

bool
tab_detach(struct terminal *term)
{
    struct wl_window *win = term->window;
    const bool tab_bar_was_visible = tab_bar_visible(win);

    size_t idx = 0;
    bool found = false;
    tll_foreach(win->tabs, it) {
        if (it->item == term) {
            tll_remove(win->tabs, it);
            found = true;
            break;
        }
        idx++;
    }

    xassert(found);
    (void)found;

    if (tll_length(win->tabs) == 0) {
        xassert(win->term == term);
        return true;
    }

    if (win->term == term) {
        struct terminal *last_used =
            term->conf->tabs.activate_on_close == TABS_ACTIVATE_ON_CLOSE_LAST_USED
                ? last_used_tab(win) : NULL;

        if (last_used != NULL)
            tab_activate(last_used);
        else {
            /* Like most tabbed applications, prefer the tab to the right */
            tab_activate_index(win, min(idx, tll_length(win->tabs) - 1));
        }
    } else if (tab_bar_was_visible && !tab_bar_visible(win)) {
        /* The tab bar was hidden; give its space to the grid */
        const struct terminal *active = win->term;
        wayl_win_tab_sync_size(
            win->term, active->width / active->scale,
            active->height / active->scale, true);
    }

    xassert(win->term != term);

    if (win->confirm_close == TAB_CLOSE_ALL) {
        if (tll_length(win->tabs) < 2)
            confirm_close_dismiss(win);
    } else if (win->confirm_close != TAB_CLOSE_NONE) {
        /* Tabs closed by themselves; update, or drop, the question */
        if (tab_close_count(win, win->confirm_close) < 2)
            confirm_close_dismiss(win);
        else
            render_refresh(win->term);
    }

    tll_foreach(term->wl->seats, it) {
        xassert(it->item.kbd_focus != term);
        xassert(it->item.mouse_focus != term);
        xassert(it->item.ime_focus != term);
    }

    render_refresh_tab_bar(win);
    return false;
}

bool
tab_bar_visible(const struct wl_window *win)
{
    switch (win->term->conf->tabs.show_bar) {
    case TABS_SHOW_BAR_AUTO:   return tll_length(win->tabs) > 1;
    case TABS_SHOW_BAR_ALWAYS: return true;
    case TABS_SHOW_BAR_NEVER:  return false;
    }

    BUG("unhandled show-bar value: %d", win->term->conf->tabs.show_bar);
    return false;
}

bool
tab_bar_shown(const struct wl_window *win)
{
    return tab_bar_visible(win) || win->term->tab.rename.active;
}

int
tab_bar_strip_height(const struct terminal *term)
{
    /* Default: half the CSD title height */
    return term_pt_or_px_as_pixels(term, &term->conf->tabs.bar_height);
}

int
tab_bar_height(const struct terminal *term)
{
    return tab_bar_visible(term->window) ? tab_bar_strip_height(term) : 0;
}

/*
 * Layout of the tab bar:
 *
 *   [ tab 0 | tab 1 | ... ]  [            command output ]
 *   \______ tabs_width ____/ \_____ status_width ________/
 *
 * Each tab is as wide as its label (up to the configured
 * bar-tab-max-width, and never wider than the tab region itself). The
 * tabs' side takes just as much room as the tabs need (up to the
 * point where the status output would not fit anymore). When there
 * are more tabs than fit, only a window around the active tab is
 * shown (scrolling the bar to keep the active tab centered).
 */

static int
tab_bar_margin(const struct wl_window *win)
{
    struct fcft_font *font = win->tab_bar.font;
    if (font == NULL)
        return 0;

    const struct fcft_glyph *M = fcft_rasterize_char_utf32(
        font, U'M', win->term->font_subpixel);
    return M != NULL ? M->advance.x : font->max_advance.x;
}

/* Width of the command output drawn on the right hand side */
static int
tab_bar_status_width(struct wl_window *win, int width)
{
    if (win->term->conf->tabs.bar_command.argv.args == NULL ||
        win->tab_bar.font == NULL)
        return 0;

    char *line = tabbar_cmd_last_line(win);
    if (line == NULL)
        return 0;

    char32_t *text = ambstoc32(line);
    free(line);
    if (text == NULL)
        return 0;

    const int text_width = render_text_draw_width(
        win->term, win->tab_bar.font, text);
    free(text);

    return text_width;
}

/* Width of a single tab's label, including its margins. 'vidx' is the
 * tab's position among the visible tabs (hidden tabs are unnamed and
 * not shown anyway) */
static int
tab_bar_tab_label_width(
    const struct wl_window *win, const struct terminal *tab, size_t vidx)
{
    char *title = xasprintf(
        "%s%zu:%s", tab->tab.attention ? "\u2022 " : "",
        vidx + 1, tab_title(tab));
    char32_t *label = ambstoc32(title);
    free(title);

    if (label == NULL)
        return 2 * tab_bar_margin(win);

    const int width = render_text_width(win->term, win->tab_bar.font, label);
    free(label);
    return width + tab_bar_margin(win);
}

int
tab_bar_tabs_width(struct wl_window *win, int width)
{
    if (win->term->conf->tabs.bar_command.argv.args == NULL)
        return width;

    /* Tab region = total width minus the command output region */
    return max(0, width - tab_bar_status_width(win, width));
}

/* Configured maximum tab width, in pixels; 'none' → tab region width.
 * Clamped to [0, tabs_width] */
static int
tab_bar_max_tab_width(struct wl_window *win, int tabs_width)
{
    const struct config *conf = win->term->conf;
    int max_w;
    switch (conf->tabs.tab_max_width.type) {
    case TAB_MAX_WIDTH_NONE:     max_w = tabs_width; break;
    case TAB_MAX_WIDTH_PERCENT:  max_w = (int)roundf(
        conf->tabs.tab_max_width.percent * tabs_width); break;
    case TAB_MAX_WIDTH_ABSOLUTE: max_w = term_pt_or_px_as_pixels(
        win->term, &conf->tabs.tab_max_width.length); break;
    default:                     max_w = tabs_width; break;
    }
    return max(0, min(max_w, tabs_width));
}

/* Slot width of the tab with visible index 'vidx': its label width
 * (which includes one margin of padding), clamped to
 * [margin, min(configured max, tab region width)] */
int
tab_bar_tab_width(struct wl_window *win, int tabs_width,
                  const struct terminal *tab, size_t vidx)
{
    const int margin = tab_bar_margin(win);
    const int max_w = max(margin, min(tab_bar_max_tab_width(win, tabs_width),
                                      tabs_width));
    return max(margin, min(tab_bar_tab_label_width(win, tab, vidx), max_w));
}

/* Hidden tabs don't occupy a slot in the bar; the boundaries of the
 * returned range may sit on hidden tabs — all consumers skip hidden
 * tabs when translating indices to slots */
static struct tab_bar_range
tab_bar_visible_range(struct wl_window *win, int width)
{
    struct tab_bar_range r = {0, 0};

    /* Locate the anchor: the active tab, or the first non-hidden one.
     * Widths depend on the visible index, so track vidx during the
     * scan. */
    __typeof__(win->tabs.head) anchor = NULL;
    size_t anchor_vidx = 0;
    {
        size_t vidx = 0;
        tll_foreach(win->tabs, it) {
            if (it->item->tab.hidden)
                continue;
            if (anchor == NULL || it->item == win->term) {
                anchor = it;
                anchor_vidx = vidx;
                if (it->item == win->term)
                    break;
            }
            vidx++;
        }
    }

    if (anchor == NULL) {
        r.first = 1;
        r.last = 0; /* empty range */
        return r;
    }

    const int bar_width = tab_bar_tabs_width(win, width);
    const int half = bar_width / 2;

    int used = tab_bar_tab_width(win, bar_width, anchor->item, anchor_vidx);
    __typeof__(win->tabs.head) left = anchor, right = anchor;
    size_t left_vidx = anchor_vidx, right_vidx = anchor_vidx;

    /* Expand around the anchor: fill the left half first, then the
     * full width to the right, then use up whatever is left on the
     * left. Widths are measured per tab; hidden tabs are skipped. */
    while (left->prev != NULL) {
        __typeof__(win->tabs.head) prev = left->prev;
        if (prev->item->tab.hidden) {
            left = prev;
            continue;
        }
        const int w = tab_bar_tab_width(
            win, bar_width, prev->item, left_vidx - 1u);
        if (used + w > half)
            break;
        used += w;
        left = prev;
        left_vidx--;
    }

    while (right->next != NULL) {
        __typeof__(win->tabs.head) next = right->next;
        if (next->item->tab.hidden) {
            right = next;
            continue;
        }
        const int w = tab_bar_tab_width(
            win, bar_width, next->item, right_vidx + 1);
        if (used + w > bar_width)
            break;
        used += w;
        right = next;
        right_vidx++;
    }

    while (left->prev != NULL) {
        __typeof__(win->tabs.head) prev = left->prev;
        if (prev->item->tab.hidden) {
            left = prev;
            continue;
        }
        const int w = tab_bar_tab_width(
            win, bar_width, prev->item, left_vidx - 1u);
        if (used + w > bar_width)
            break;
        used += w;
        left = prev;
        left_vidx--;
    }

    /* Translate the boundary iterators back to full-list indices */
    r.first = r.last = (size_t)-1;
    size_t idx = 0;
    tll_foreach(win->tabs, it) {
        if (it == left)
            r.first = idx;
        if (it == right)
            r.last = idx;
        idx++;
    }
    xassert(r.first != (size_t)-1 && r.last != (size_t)-1);
    return r;
}

void
tab_bar_layout_get(struct wl_window *win, int width,
                   struct tab_bar_range *r)
{
    *r = tab_bar_visible_range(win, width);
}

void
tab_bar_tab_extent(struct wl_window *win, int width, size_t idx,
                   int *x0, int *x1)
{
    const struct tab_bar_range r = tab_bar_visible_range(win, width);

    if (idx < r.first || idx > r.last) {
        *x0 = *x1 = 0;
        return;
    }

    /* Accumulate per-tab widths from the first visible tab. Hidden
     * tabs take no slot. */
    const int tabs_width = tab_bar_tabs_width(win, width);
    int x = 0;
    size_t vidx = 0;
    for (size_t i = r.first; i <= idx; i++) {
        const struct terminal *tab = tab_at_index(win, i);
        if (tab->tab.hidden)
            continue;
        const int w = tab_bar_tab_width(win, tabs_width, tab, vidx);
        if (i == idx) {
            *x0 = x;
            *x1 = x + w;
            return;
        }
        x += w;
        vidx++;
    }

    *x0 = *x1 = 0;
}

/* True if 'tab' is currently drawn in the tab bar */
bool
tab_bar_tab_visible(struct wl_window *win, const struct terminal *tab)
{
    size_t idx = 0;
    tll_foreach(win->tabs, it) {
        if (it->item == tab)
            break;
        idx++;
    }

    const struct tab_bar_range r =
        tab_bar_visible_range(win, win->term->width);
    return idx >= r.first && idx <= r.last;
}

int
tab_bar_tab_at(struct wl_window *win, int x)
{
    const size_t count = tll_length(win->tabs);
    if (count == 0 || x < 0)
        return -1;

    const int width = win->term->width;
    const struct tab_bar_range r = tab_bar_visible_range(win, width);
    const int tabs_width = tab_bar_tabs_width(win, width);

    /* Accumulate per-tab widths from the first visible tab */
    int px = 0;
    size_t vidx = 0;
    for (size_t i = r.first; i <= r.last; i++) {
        const struct terminal *tab = tab_at_index(win, i);
        if (tab == NULL || tab->tab.hidden)
            continue;

        const int w = tab_bar_tab_width(win, tabs_width, tab, vidx);
        if (x >= px && x < px + w)
            return (int)i;
        px += w;
        vidx++;
    }

    return -1;
}

int
tab_bar_y_offset(const struct wl_window *win)
{
    if (!win->term->conf->tabs.bar_bottom)
        return 0;

    /* Bar is anchored to the bottom of the window. wl_subsurface_set_position
     * takes logical (scale-independent) coordinates */
    const float scale = win->term->scale;
    const int strip = roundf(tab_bar_strip_height(win->term) / scale);
    const int win_h = roundf(win->term->height / scale);
    return max(0, win_h - strip);
}
