#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <xkbcommon/xkbcommon.h>

struct seat;
struct terminal;
struct wl_window;

/*
 * Tabs: a window holds one or more terminals (tabs). Only the active
 * tab (win->term) renders, and receives input. Background tabs keep
 * reading from their PTYs, and are resized and repainted when they
 * are activated.
 */

/* Matches the tab-goto-N key bindings */

/* Which tabs to close, relative to the active tab */
enum tab_close_scope {
    TAB_CLOSE_NONE,
    TAB_CLOSE_ALL,     /* The window */
    TAB_CLOSE_LEFT,
    TAB_CLOSE_RIGHT,
    TAB_CLOSE_OTHERS,
};

bool tab_new(struct terminal *term);
void tab_activate(struct terminal *term);
void tab_activate_index(struct wl_window *win, size_t idx);
void tab_activate_named(struct wl_window *win, unsigned char name);
void tab_activate_last(struct wl_window *win);
void tab_cycle(struct wl_window *win, int direction);

/* Switches to the previously active tab */
void tab_activate_last_used(struct wl_window *win);

/*
 * Marks a background tab in the tab bar, until it is activated.
 * Called on bell, and on desktop notifications.
 */
void tab_mark_attention(struct terminal *term);

void tab_move(struct wl_window *win, int direction);
void tab_move_to(struct wl_window *win, size_t idx);
void tab_move_last(struct wl_window *win);
void tab_drag(struct wl_window *win, int x);
void tab_close(struct wl_window *win, enum tab_close_scope scope);

/*
 * Closes the tabs in 'scope'. If that is more than one tab, the user
 * is first asked to confirm (unless disabled in the configuration).
 */
void tab_request_close(struct wl_window *win, enum tab_close_scope scope);

/*
 * Closes the window, i.e. all its tabs, asking for confirmation like
 * tab_request_close(). Requesting a close while already asking,
 * closes the window.
 */
void tab_request_close_window(struct wl_window *win);

/* Number of tabs (not already closing) that 'scope' would close */
size_t tab_close_count(const struct wl_window *win, enum tab_close_scope scope);
void tab_confirm_close_input(struct terminal *term, xkb_keysym_t sym);
struct terminal *tab_at_index(const struct wl_window *win, size_t idx);

/* The tab's label: user assigned, or the window title */
const char *tab_title(const struct terminal *term);

/* Refresh the cached foreground process name shown in the tab label */
void tab_update_comm(struct terminal *term);

/*
 * Interactive renaming. While active, all keyboard input goes to the
 * tab's label. An empty name reverts to the window title.
 */
void tab_rename_start(struct terminal *term);
void tab_rename_commit(struct terminal *term);
void tab_rename_cancel(struct terminal *term);
void tab_rename_input(
    struct seat *seat, struct terminal *term, uint32_t key, xkb_keysym_t sym);

/*
 * Removes the terminal from its window's tab list. Returns true if
 * it was the last tab, in which case the caller should destroy the
 * window.
 */
bool tab_detach(struct terminal *term);

/*
 * The tab bar is visible (and takes up space) depending on the
 * show-bar option; by default, when there's more than one tab. It is
 * also *shown* while renaming a tab when it isn't visible, but then
 * on top of the grid, to avoid resizing it.
 */
bool tab_bar_visible(const struct wl_window *win);
bool tab_bar_shown(const struct wl_window *win);
int tab_bar_height(const struct terminal *term);
int tab_bar_strip_height(const struct terminal *term);
struct tab_bar_range {
    size_t first;
    size_t last;
};

/* Compute the visible-tab range in one pass. Each tab's width is its
 * own label width (clamped to the configured maximum and the tab
 * region); when the tabs don't all fit, a window around the active
 * tab is shown */
void tab_bar_layout_get(struct wl_window *win, int width,
                        struct tab_bar_range *r);

void tab_bar_tab_extent(
    struct wl_window *win, int width, size_t idx, int *x0, int *x1);

/* Slot width of the tab with visible index 'vidx', clamped to the
 * configured maximum and the tab region width */
int tab_bar_tab_width(struct wl_window *win, int tabs_width,
                      const struct terminal *tab, size_t vidx);

int tab_bar_tab_at(struct wl_window *win, int x);
int tab_bar_tabs_width(struct wl_window *win, int width);
int tab_bar_y_offset(const struct wl_window *win);

/* True if 'tab' is currently drawn in the tab bar (it may be scrolled
 * out of view when there are more tabs than fit) */
bool tab_bar_tab_visible(struct wl_window *win, const struct terminal *tab);
