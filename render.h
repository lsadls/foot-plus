#pragma once
#include <stdbool.h>

#include "terminal.h"
#include "fdm.h"
#include "wayland.h"
#include "misc.h"

struct renderer;
struct renderer *render_init(struct fdm *fdm, struct wayland *wayl);
void render_destroy(struct renderer *renderer);

enum resize_options {
    RESIZE_NORMAL = 0,
    RESIZE_FORCE = 1 << 0,
    RESIZE_BY_CELLS = 1 << 1,
    RESIZE_KEEP_GRID = 1 << 2,
};

bool render_resize(
    struct terminal *term, int width, int height, uint8_t resize_options);

void render_refresh(struct terminal *term);
void render_refresh_full(struct terminal *term);

/*
 * Frees all cached SHM render buffers of 'term' (including
 * 'render.last_buf'). Intended for background tabs: they don't
 * render, and re-activating them triggers a full repaint anyway.
 * Busy buffers are deferred until the compositor releases them.
 */
void term_release_render_buffers(struct terminal *term);

/* Scrollbar geometry, in physical pixels, relative to the scrollbar */
struct scrollbar_geometry {
    int x, y;            /* Position, relative to the window */
    int width, height;
    int track_y, track_height;  /* The part the thumb moves within */
    int thumb_y, thumb_height;
    int view_pos;        /* Viewport position in the scrollback */
    int max_view_pos;    /* View position when at the bottom */
};

/* Returns false if the scrollbar should not be shown */
bool render_scrollbar_geometry(
    const struct terminal *term, struct scrollbar_geometry *g);
void render_refresh_tab_bar(struct wl_window *win);

/* Width, in physical pixels, of 'text' rendered in 'font' */
int render_text_width(
    const struct terminal *term, struct fcft_font *font, const char32_t *text);

int render_text_draw_width(
    struct terminal *term, struct fcft_font *font, const char32_t *text);
void render_flush_pending_resize(struct terminal *term);
void render_refresh_app_id(struct terminal *term);
void render_refresh_icon(struct terminal *term);
void render_refresh_csd(struct terminal *term);
void render_refresh_search(struct terminal *term);
void render_refresh_title(struct terminal *term);
void render_refresh_urls(struct terminal *term);
bool render_xcursor_set(
    struct seat *seat, struct terminal *term, enum cursor_shape shape);
bool render_xcursor_is_valid(const struct seat *seat, const char *cursor);

void render_overlay(struct terminal *term);

struct render_worker_context {
    int my_id;
    struct terminal *term;
};
int render_worker_thread(void *_ctx);

struct csd_data {
    int x;
    int y;
    int width;
    int height;
};

struct csd_data get_csd_data(const struct terminal *term, enum csd_surface surf_idx);

void render_buffer_release_callback(struct buffer *buf, void *data);
void render_wait_for_preapply_damage(struct terminal *term);
