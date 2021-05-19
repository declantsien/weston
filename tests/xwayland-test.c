/*
 * Copyright © 2015 Samsung Electronics Co., Ltd
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
 *
 * xwayland-test: Confirm that we can map a window and we're running
 *		  under Xwayland, not just X.
 *
 *		  This is done in steps:
 *		  1) Confirm that the WL_SURFACE_ID atom exists
 *		  2) Confirm that our window name is "Xwayland Test Window"
 *		  3) Confirm that there's conforming Window Manager
 *		  4) Confirm that the window manager's name is "Weston WM"
 *		  5) Make sure we can map a window
 */

#include "config.h"

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "weston-test-runner.h"
#include "weston-test-fixture-compositor.h"
#include "shared/string-helpers.h"
#include "weston-test-client-helper.h"
#include "xcb-client-helper.h"

static struct atom_x11 atoms;
static struct atom_x11 *atoms_ptr;

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.xwayland = true;
	setup.logging_scopes = "xwm-wm-x11";

	memset(&atoms, 0, sizeof(struct atom_x11));
	atoms_ptr = NULL;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

static char *
get_x11_window_name(struct window_x11 *window, xcb_drawable_t win)
{
	xcb_get_property_reply_t *reply;
	int reply_len;
	char *name;

	reply = window_x11_dump_prop(window, win, window->atoms->net_wm_name);
	assert(reply);

	assert(reply->type == window->atoms->string ||
	       reply->type == window->atoms->utf8_string);
	reply_len = xcb_get_property_value_length(reply);
	assert(reply_len > 0);

	str_printf(&name, "%.*s", reply_len,
		   (char *) xcb_get_property_value(reply));
	free(reply);
	return name;
}

static char *
get_wm_name(struct window_x11 *window)
{
	xcb_get_property_reply_t *reply;
	xcb_generic_error_t *error;
	xcb_get_property_cookie_t prop_cookie;
	char *wm_name = NULL;

	prop_cookie = xcb_get_property(window->connection, 0, window->root_win,
				       window->atoms->net_supporting_wm_check,
				       XCB_ATOM_WINDOW, 0, 1024);
	reply = xcb_get_property_reply(window->connection, prop_cookie, &error);
	assert(reply);

	if (reply && reply->format == 32 && reply->type == XCB_ATOM_WINDOW) {
		xcb_window_t wm_id = *(xcb_window_t *) xcb_get_property_value(reply);
		wm_name = get_x11_window_name(window, wm_id);
	}

	free(error);
	return wm_name;
}

TEST(xwayland_client_test)
{
	struct window_x11 *window;
	xcb_get_property_reply_t *reply;
	char *win_name;
	char *wm_name;
	pixman_color_t bg_color;

	color_rgb888(&bg_color, 255, 0, 0);
	window = create_x11_window(100, 100, 100, 100, bg_color, NULL);
	assert(window);

	if (atoms_ptr == NULL) {
		get_atoms(window->connection, &atoms);
		atoms_ptr = &atoms;
	}

	window_x11_set_atoms(window, &atoms);
	assert(window->atoms);

	window_x11_set_win_name(window, "Xwayland Test Window");
	handle_events_x11(window);
	assert(window->state.win_state & PROPERTY_NAME);

	assert(window->atoms->net_supporting_wm_check != XCB_ATOM_NONE);
	assert(window->atoms->wl_surface_id != XCB_ATOM_NONE);
	assert(window->atoms->net_wm_name != XCB_ATOM_NONE);
	assert(window->atoms->utf8_string != XCB_ATOM_NONE);

	window_x11_map(window);
	handle_events_x11(window);
	assert(window->state.win_state & MAPPED);

	win_name = get_x11_window_name(window, window->win);
	assert(strcmp(win_name, "Xwayland Test Window") == 0);
	free(win_name);

	/* The Window Manager MUST set _NET_SUPPORTING_WM_CHECK on the root
	 * window to be the ID of a child window created by himself, to
	 * indicate that a compliant window manager is active.
	 *
	 * The child window MUST also have the _NET_SUPPORTING_WM_CHECK
	 * property set to the ID of the child window. The child window MUST
	 * also have the _NET_WM_NAME property set to the name of the Window
	 * Manager.
	 *
	 * See Extended Window Manager Hints,
	 * https://specifications.freedesktop.org/wm-spec/latest/ar01s03.html,
	 * _NET_SUPPORTING_WM_CHECK
	 * */
	reply = window_x11_dump_prop(window, window->root_win,
				     window->atoms->net_supporting_wm_check);
	assert(reply);
	assert(reply->type == XCB_ATOM_WINDOW);
	free(reply);

	wm_name = get_wm_name(window);
	assert(wm_name);
	assert(strcmp(wm_name, "Weston WM") == 0);
	free(wm_name);

	window_x11_unmap(window);
	handle_events_x11(window);
	assert(window->state.win_state & UNMAPPED);

	destroy_x11_window(window);
}
