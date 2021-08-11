/*
 * Copyright 2021 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _WESTON_TEST_CLIENT_XWAYLAND_HELPER_H_
#define _WESTON_TEST_CLIENT_XWAYLAND_HELPER_H_

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "shared/xcb-xwayland.h"
#include <pixman.h>
#include <wayland-client.h>

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>

enum w_state {
	CREATED			= 1 << 0,
	NORMAL			= 1 << 1,	/* for property */
	MAXIMIZED 		= 1 << 2,	/* for property */
	FULLSCREEN		= 1 << 3,	/* for property */
	MAPPED			= 1 << 4,
	UNMAPPED		= 1 << 5,
	CONFIGURED_MAX		= 1 << 6,	/* for configure */
	CONFIGURED_FULL		= 1 << 7,	/* for configure */
	CONFIGURED_NORMAL	= 1 << 8,	/* for configure */
	CONFIGURED_MOVE 	= 1 << 9,
	CONFIGURED_RESIZE 	= 1 << 10,
	CONFIGURED_MOVE_RESIZE 	= 1 << 11,
	EXPOSE			= 1 << 12,
	PROPERTY_NAME		= 1 << 13,
	DESTROYED		= 1 << 14,
};

struct window_configure {
	int x, y;
	int width, height;
	bool updated;
};

struct window_state {
	uint8_t event;
	xcb_drawable_t wid;
	struct wl_list link;
};

struct window_x11 {
	struct window_x11 *parent;
	struct xcb_connection_t *connection;
	struct xcb_screen_t *screen;
	bool handle_in_progress;

	xcb_drawable_t root_win;	/* screen root */
	xcb_drawable_t win;		/* this window */
	xcb_drawable_t parent_win;	/* the parent, if set */

	xcb_gcontext_t background;

	xcb_cursor_context_t *ctx;
	xcb_cursor_t cursor;

	int width;
	int height;

	int pos_x;
	int pos_y;

	struct atom_x11 *atoms;
	pixman_color_t bg_color;

	/* these track what the X11 client does */
	struct {
		/* not applied, only queued */
		uint32_t win_state;

		bool start_fullscreen;
		bool start_maximized;

		/* pending queue events */
		struct wl_list pending_events_list;
	} tentative_state;

	/* these track what we got back from the server */
	struct {
		bool start_fullscreen;
		bool start_maximized;
		/* applied, received event */
		uint32_t win_state;
	} state;

	/* updated for each configure notify */
	struct window_configure configured_state;

	struct wl_list window_list;
	struct wl_list window_link;

	xcb_window_t frame_id;
};

struct event_response {
    uint8_t response_type;
    bool (*eventcb)(xcb_generic_event_t *e, struct window_x11 *win);
    const char *name;
};

void
window_x11_reset_internal_state(struct window_x11 *window);

void
window_x11_map(struct window_x11 *window);

void
window_x11_unmap(struct window_x11 *window);

void
window_x11_move(struct window_x11 *window, int x, int y);

void
window_x11_resize(struct window_x11 *window, int width, int height);

void
window_x11_move_resize(struct window_x11 *window, int x, int y, int width, int height);

int
handle_events_x11(struct window_x11 *window);

void
window_x11_set_fullscreen(struct window_x11 *window);

void
window_x11_set_maximized(struct window_x11 *window);

void
window_x11_set_state(struct window_x11 *window, enum w_state state);

void
window_x11_toggle_state(struct window_x11 *window, enum w_state state);

struct window_x11 *
create_x11_window(int width, int height, int pos_x, int pos_y,
		  pixman_color_t bg_color, struct window_x11 *parent);
void
destroy_x11_window(struct window_x11 *window);

void
window_x11_set_win_name(struct window_x11 *window, const char *name);

void
window_x11_set_atoms(struct window_x11 *window, struct atom_x11 *atoms);

xcb_get_property_reply_t *
window_x11_dump_prop(struct window_x11 *window, xcb_drawable_t win, xcb_atom_t atom);

void
handle_event_set_pending(struct window_x11 *window, uint8_t event, xcb_drawable_t wid);

#endif
