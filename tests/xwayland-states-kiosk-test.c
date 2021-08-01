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

#include "config.h"

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "weston-test-runner.h"
#include "weston-test-fixture-compositor.h"

#include "weston-test-client-helper.h"
#include "xcb-client-helper.h"

static struct atom_x11 atoms;
static struct atom_x11 *atoms_ptr;

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.width = 800;
	setup.height = 600;
	setup.renderer = RENDERER_PIXMAN;
	setup.xwayland = true;
	setup.shell = SHELL_KIOSK;

	memset(&atoms, 0, sizeof(struct atom_x11));
	atoms_ptr = NULL;

	setenv("XCURSOR_PATH", "/tmp", 1);

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

TEST(xwayland_client_kiosk_shot_test)
{
	struct window_x11 *window;
	struct client *client;
	pixman_color_t bg_color;

	client = create_client();
	client_roundtrip(client);

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


	window_x11_map(window);
	handle_events_x11(window);
	assert(window->state.win_state & MAPPED);

	client_enable_xwayland_output_repaint_events(client);
	client_wait_xwayland_output(client);

	/* kiosk-shell doesn't handle the cursor so we need to use a different
	 * fullscreen image than from desktop-shell */
	verify_screen_content(client, "xwayland-fullscreen-client", 1, NULL, 0);

	/* FIXME: we don't support for the time being setting the window
	 * fullscreen from the shell itself, so avoid the assert(match);
	 * Also, see !424 which should address this */

	window_x11_unmap(window);
	handle_events_x11(window);
	assert(window->state.win_state & UNMAPPED);

	client_destroy(client);
	destroy_x11_window(window);
}
