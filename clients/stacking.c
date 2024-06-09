/*
 * Copyright © 2013 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <linux/input.h>
#include <cairo.h>
#include <wayland-util.h>

#include "shared/xalloc.h"
#include "shared/helpers.h"
#include "window.h"

#include "xdg-shell-client-protocol.h"

struct stacking_session {
	struct stacking *stacking;
	struct session *session;
	char *session_id;
	struct wl_list window_session_list;  /* struct stacking_window_session::link */
	struct wl_list link;                 /* struct stacking::session_list */
};

struct stacking_window {
	struct stacking *stacking;
	struct window *window;
	struct widget *widget;
	struct stacking_session *session;
	struct stacking_window_session *window_session;
	struct wl_list link;          /* struct stacking::window_list */
};

struct stacking_window_session {
	struct stacking_window *window;
	struct stacking_session *session;
	char *toplevel_id;
	bool active;
	struct wl_list link;  /* struct stacking_session::window_session_list */
};

struct stacking {
	struct display *display;
	struct wl_list window_list;   /* struct stacking_window::link */
	struct wl_list session_list;  /* struct stacking_session::link */
};

static void
session_created_handler(struct session *session, const char *session_id, void *data)
{
	struct stacking_session *stacking_session = data;

	printf("session created %s\n", session_id);

	stacking_session->session_id = xstrdup(session_id);
}

static void
session_restored_handler(struct session *session, void *data)
{
	struct stacking_session *stacking_session = data;

	printf("session restored %s\n", session_get_session_id(session));

	assert(strcmp(stacking_session->session_id, session_get_session_id(session)) == 0);
}

static struct stacking_session *
create_session(struct stacking *stacking, const char *session_id)
{
	struct session *session;
	struct stacking_session *stacking_session = NULL;

	session = display_get_session(stacking->display, session_id);
	if (session == NULL)
		goto fail;

	stacking_session = xzalloc(sizeof *stacking_session);

	stacking_session->stacking = stacking;
	stacking_session->session = session;

	if (session_id)
		stacking_session->session_id = xstrdup(session_id);
	wl_list_init(&stacking_session->window_session_list);

	wl_list_insert(stacking->session_list.prev, &stacking_session->link);

	session_set_user_data(session, stacking_session);
	session_set_created_handler(session, session_created_handler);
	session_set_restored_handler(session, session_restored_handler);

	// Trigger session creation
	wl_display_roundtrip(display_get_display(stacking->display));

	return stacking_session;

fail:
	fprintf(stderr, "could not create session\n");
	return NULL;
}

/* Remove references to window session, toplevel_ids no longer valid */
static void
remove_window_session(struct stacking_window_session *stacking_window_session)
{
	struct stacking_window *stacking_window = stacking_window_session->window;
	printf("remove window session %p %s\n", stacking_window_session, stacking_window_session->toplevel_id);
	session_remove_window(stacking_window_session->session->session, stacking_window_session->toplevel_id);
	if (stacking_window)
	{
		stacking_window_session->window = NULL;
		window_session_destroy(stacking_window->window);
		stacking_window->window_session = NULL;
		stacking_window_session->active = false;
		window_schedule_redraw(stacking_window->window);
	}
	wl_list_remove(&stacking_window_session->link);
	stacking_window_session->session = NULL;
	free(stacking_window_session->toplevel_id);
	stacking_window_session->toplevel_id = NULL;
	free(stacking_window_session);
}

/* Detach window sessions from window so toplevel_ids can be used to restore windows */
static void
destroy_window_session(struct stacking_window_session *stacking_window_session)
{
	struct stacking_window *stacking_window = stacking_window_session->window;
	printf("destroy window session %p %s\n", stacking_window_session, stacking_window_session->toplevel_id);
	if (stacking_window)
	{
		stacking_window_session->window = NULL;
		window_session_destroy(stacking_window->window);
		stacking_window->window_session = NULL;
		stacking_window_session->active = false;
		window_schedule_redraw(stacking_window->window);
	}
}

/* Should not bother to remove/destroy child toplevel id's
 * Should free window session objects */
//static void
//remove_session(struct stacking_session *stacking_session)
//{
//	// TODO: Keep and restore session_id
//	//   (Switch the meaning of remove_session and destroy_session)
//	session_remove(stacking_session->session);
//}

/* Should not bother to remove/destroy child toplevel id's
 * Should we keep session object around in anticipation of restore? probably */
static void
destroy_session(struct stacking_session *stacking_session)
{
	struct stacking_window_session *stacking_window_session, *tmp;
	struct stacking_window *stacking_window;
	wl_list_for_each_safe(stacking_window_session, tmp, &stacking_session->window_session_list, link)
	{
		if (stacking_window_session->window)
			stacking_window_session->window->session = NULL;
		remove_window_session(stacking_window_session);
	}
	wl_list_for_each(stacking_window, &stacking_session->stacking->window_list, link)
	{
		if (stacking_window->session == stacking_session)
		{
			window_schedule_redraw(stacking_window->window);
			stacking_window->session = NULL;
		}
	}

	session_destroy(stacking_session->session);
	stacking_session->session = NULL;
	wl_list_remove(&stacking_session->link);
	if (stacking_session->session_id)
		free(stacking_session->session_id);
	stacking_session->session_id = NULL;
	free(stacking_session);
}

static void
close_handler(void *data);
static void
button_handler(struct widget *widget,
               struct input *input, uint32_t time,
               uint32_t button,
               enum wl_pointer_button_state state, void *data);
static void
key_handler(struct window *window,
            struct input *input, uint32_t time,
            uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
            void *data);
static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data);
static void
fullscreen_handler(struct window *window, void *data);
static void
redraw_handler(struct widget *widget, void *data);

static void
window_session_toplevel_id_handler(struct window *window, const char *toplevel_id, void *data);
static void
window_session_restored_handler(struct window *window, void *data);

/* Iff parent_window is set, the new window will be transient. */
static struct stacking_window *
create_window(struct stacking *stacking, struct window *parent_window, struct stacking_session *stacking_session)
{
	struct stacking_window *stacking_window = NULL;
	struct stacking_window_session *stacking_window_session = NULL, *sws;
	struct window *new_window;
	struct widget *new_widget;

	// Search session for unused toplevel_id
	if (stacking_session)
	{
		wl_list_for_each(sws, &stacking_session->window_session_list, link)
		{
			if (sws->window == NULL && sws->toplevel_id != NULL)
			{
				stacking_window_session = sws;
				printf("Reuse window_session \"%s\"\n",
				       stacking_window_session->toplevel_id);
			}
		}
	}

	stacking_window = xzalloc(sizeof *stacking_window);
	stacking_window->stacking = stacking;
	stacking_window->session = stacking_session;
	wl_list_insert(stacking->window_list.prev, &stacking_window->link);

	if (stacking_window_session != NULL)
	{
		new_window = window_create_restore(stacking->display,
						   stacking_window_session->session->session,
						   stacking_window_session->toplevel_id);
		stacking_window->window_session = stacking_window_session;
		stacking_window_session->window = stacking_window;
	}
	else
	{
		new_window = window_create(stacking->display);
	}

	stacking_window->window = new_window;
	window_set_parent(new_window, parent_window);

	new_widget = window_frame_create(new_window, new_window);
	stacking_window->widget = new_widget;

	window_set_title(new_window, "Stacking Test");
	window_set_appid(new_window, "org.freedesktop.weston.stacking-test");
	window_set_key_handler(new_window, key_handler);
	window_set_keyboard_focus_handler(new_window, keyboard_focus_handler);
	window_set_fullscreen_handler(new_window, fullscreen_handler);
	window_set_close_handler(new_window, close_handler);
	window_set_session_toplevel_id_handler(new_window, window_session_toplevel_id_handler);
	window_set_session_restored_handler(new_window, window_session_restored_handler);
	widget_set_button_handler(new_widget, button_handler);
	widget_set_redraw_handler(new_widget, redraw_handler);
	window_set_user_data(new_window, stacking_window);

	// TODO: Should we be resizing the window if we are restoring it?
	//if (!stacking_window->window_session)
		window_schedule_resize(new_window, 300, 300);

	wl_display_roundtrip(display_get_display(stacking->display));

	return stacking_window;
}



static void
destroy_window(struct stacking_window *stacking_window)
{
	struct stacking *stacking = stacking_window->stacking;

	printf("close window %p\n", stacking_window);

	widget_destroy(stacking_window->widget);
	window_destroy(stacking_window->window);

	if (stacking_window->window_session != NULL)
	{
		stacking_window->window_session->active = false;
		stacking_window->window_session->window = NULL;
	}

	wl_list_remove(&stacking_window->link);
	free(stacking_window);

	if (wl_list_empty(&stacking->window_list))
		display_exit(stacking->display);
}

static void
create_window_session(struct stacking_window *stacking_window, struct stacking_session *stacking_session)
{
	if (stacking_window->window_session != NULL)
		return;
	stacking_window->session = stacking_session;
	struct stacking_window_session *stacking_window_session = xzalloc(sizeof *stacking_window_session);
	stacking_window_session->window = stacking_window;
	stacking_window_session->session = stacking_session;
	wl_list_insert(stacking_session->window_session_list.prev, &stacking_window_session->link);
	stacking_window->window_session = stacking_window_session;
	session_add_window(stacking_session->session, stacking_window->window);
}

static void
show_popup_cb(void *data, struct input *input, int index)
{
	/* Ignore the selected menu item. */
}

static void
show_popup(struct stacking *stacking, struct input *input, uint32_t time,
           struct window *window)
{
	int32_t x, y;
	static const char *entries[] = {
		"Test Entry",
		"Another Test Entry",
	};

	input_get_position(input, &x, &y);
	window_show_menu(stacking->display, input, time, window, x, y,
	                 show_popup_cb, entries, ARRAY_LENGTH(entries));
}

static void
close_handler(void *data)
{
	struct stacking_window *stacking_window = data;

	destroy_window(stacking_window);
}

static void
button_handler(struct widget *widget,
               struct input *input, uint32_t time,
               uint32_t button,
               enum wl_pointer_button_state state, void *data)
{
	struct window *window = data;
	struct stacking_window *stacking_window = window_get_user_data(window);

	switch (button) {
	case BTN_RIGHT:
		if (state == WL_POINTER_BUTTON_STATE_PRESSED)
			show_popup(stacking_window->stacking, input, time,
			           widget_get_user_data(widget));
		break;

	case BTN_LEFT:
	default:
		break;
	}
}

static void
key_handler(struct window *window,
            struct input *input, uint32_t time,
            uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
            void *data)
{
	struct stacking_window *stacking_window = data;
	struct stacking *stacking = stacking_window->stacking;
	struct stacking_session *stacking_session;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (sym) {
	case XKB_KEY_f:
		fullscreen_handler(window, data);
		break;

	case XKB_KEY_m:
		window_set_maximized(window, !window_is_maximized(window));
		break;

	case XKB_KEY_n:
		/* New top-level window. */
		create_window(stacking, NULL, stacking_window->session);
		break;

	case XKB_KEY_p:
		show_popup(stacking, input, time, window);
		break;

	case XKB_KEY_c:
		destroy_window(stacking_window);
		break;

	case XKB_KEY_q:
		display_exit(stacking->display);
		break;

	case XKB_KEY_t:
		/* New transient window. */
		create_window(stacking, window, stacking_window->session);
		break;

	// Add current window to selected session
	case XKB_KEY_a:
		if (stacking_window->session != NULL)
		{
			create_window_session(stacking_window, stacking_window->session);
		}
		break;

	// Remove current window from selected session
	case XKB_KEY_r:
		if (stacking_window->window_session != NULL)
		{
			remove_window_session(stacking_window->window_session);
			window_schedule_redraw(window);
		}
		break;

	// TODO: Remove this when ready
	// Create two session, adding 5 windows with toplevel sessions to each
	case XKB_KEY_b:
		for (int i = 0 ; i < 2 ; i++)
		{
			stacking_session = create_session(stacking, NULL);
			for (int j = 0 ; j < 5 ; j++)
			{
				printf("i=%d, j=%d\n", i, j);
				stacking_window = create_window(stacking, NULL, NULL);
				window_schedule_redraw(stacking_window->window);
				create_window_session(stacking_window, stacking_session);
			}
		}
		break;

	// Destroy the selected session
	case XKB_KEY_S:
		if (stacking_window->session)
		{
			destroy_session(stacking_window->session);
			stacking_window->session = NULL;
			window_schedule_redraw(window);
		}
		break;

	// Create a new session, removing window from previous session
	case XKB_KEY_s:
		if (stacking_window->window_session)
		{
			destroy_window_session(stacking_window->window_session);
		}
		stacking_session = create_session(stacking, NULL);
		stacking_window->session = stacking_session;
		window_schedule_redraw(window);
		break;

	// Switch window session to next in session_list, removing it from the session
	case XKB_KEY_Left:
		if (wl_list_empty(&stacking->session_list))
			break;
		stacking_session = NULL;
		// Select the next session
		if (stacking_window->session)
			stacking_session = wl_container_of(stacking_window->session->link.next, stacking_session, link);
		// Or select the first session
		if (!stacking_session || &stacking_session->link == &stacking->session_list)
			stacking_session = wl_container_of(stacking->session_list.next, stacking_session, link);

		if (stacking_window->session != stacking_session) {
			if (stacking_window->window_session)
				destroy_window_session(stacking_window->window_session);
			stacking_window->session = stacking_session;
			window_schedule_redraw(stacking_window->window);
		}
		break;

	// Switch window session to previous in session_list, removing it from the session
	case XKB_KEY_Right:
		if (wl_list_empty(&stacking->session_list))
			break;
		stacking_session = NULL;
		// Select the previous session
		if (stacking_window->session)
			stacking_session = wl_container_of(stacking_window->session->link.prev, stacking_session, link);
		// Or select the last session
		if (!stacking_session || &stacking_session->link == &stacking->session_list)
			stacking_session = wl_container_of(stacking->session_list.prev, stacking_session, link);

		if (stacking_window->session != stacking_session) {
			if (stacking_window->window_session)
				destroy_window_session(stacking_window->window_session);
			stacking_window->session = stacking_session;
			window_schedule_redraw(stacking_window->window);
		}
		break;

	default:
		break;
	}
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	window_schedule_redraw(window);
}

static void
fullscreen_handler(struct window *window, void *data)
{
	window_set_fullscreen(window, !window_is_fullscreen(window));
}

static void
draw_string(cairo_t *cr,
            const char *fmt, ...) WL_PRINTF(2, 3);

static void
draw_string(cairo_t *cr,
            const char *fmt, ...)
{
	char buffer[4096];
	char *p, *end;
	va_list argp;
	cairo_text_extents_t text_extents;
	cairo_font_extents_t font_extents;

	cairo_save(cr);

	cairo_select_font_face(cr, "sans-serif",
	                       CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);

	cairo_font_extents(cr, &font_extents);

	va_start(argp, fmt);

	vsnprintf(buffer, sizeof(buffer), fmt, argp);

	p = buffer;
	while (*p) {
		end = strchr(p, '\n');
		if (end)
			*end = 0;

		cairo_show_text(cr, p);
		cairo_text_extents(cr, p, &text_extents);
		cairo_rel_move_to(cr, -text_extents.x_advance, font_extents.height);

		if (end)
			p = end + 1;
		else
			break;
	}

	va_end(argp);

	cairo_restore(cr);
}

static void
set_window_background_colour(cairo_t *cr, struct window *window)
{
	if (window_get_parent(window))
		cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.4);
	else if (window_is_maximized(window))
		cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.6);
	else if (window_is_fullscreen(window))
		cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.6);
	else
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct window *window;
	struct stacking_window *stacking_window;
	struct stacking_window_session *stacking_window_session;
	struct rectangle allocation;
	const char *session_id = NULL, *toplevel_id = NULL;
	int session_toplevel_count = 0;
	int session_active_toplevel_count = 0;
	cairo_t *cr;

	widget_get_allocation(widget, &allocation);

	window = widget_get_user_data(widget);
	stacking_window = window_get_user_data(window);

	if (stacking_window->session != NULL)
	{
		session_id = stacking_window->session->session_id;
		wl_list_for_each(stacking_window_session, &stacking_window->session->window_session_list, link)
		{
			session_toplevel_count += 1;
			if (stacking_window_session->active)
				session_active_toplevel_count += 1;
		}
	}

	if (stacking_window->window_session != NULL)
		toplevel_id = stacking_window->window_session->toplevel_id;

	cr = widget_cairo_create(widget);
	cairo_translate(cr, allocation.x, allocation.y);

	/* Draw background. */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_window_background_colour(cr, window);
	cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
	cairo_fill(cr);

	/* Print the instructions. */
	cairo_move_to(cr, 5, 15);
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

	draw_string(cr,
	            "Window: %p\n"
	            "Transient Parent %p\n"
	            "Session-ID %s\n"
	            " (%d toplevel ids, %d active)\n"
	            "Toplevel-ID %s\n"
	            "Fullscreen? %u\n"
	            "Maximized? %u\n"
	            "Transient? %u\n"
	            "Keys: (f)ullscreen, (m)aximize,\n"
	            "      (n)ew window, (p)opup,\n"
	            "      (c)lose, (q)uit,\n"
	            "      (t)ransient window\n"
	            "Session Man:\n"
	            "      create new (s)ession,\n"
	            "      (a)dd to session,\n"
	            "      (r)emove from session\n"
	            "      Left/Right switch session\n",
	            window, window_get_parent(window), session_id,
	            session_toplevel_count, session_active_toplevel_count,
	            toplevel_id, window_is_fullscreen(window),
	            window_is_maximized(window), window_get_parent(window) ? 1 : 0);

	cairo_destroy(cr);
}

static void
window_session_toplevel_id_handler(struct window *window, const char *toplevel_id, void *data)
{
	struct stacking_window *stacking_window = window_get_user_data(window);
	if (stacking_window->window_session)
	{
		stacking_window->window_session->toplevel_id = xstrdup(toplevel_id);
		stacking_window->window_session->active = true;
		printf("Window added to session with ID \"%s\"\n", toplevel_id);
	}
	window_schedule_redraw(window);
}

static void
window_session_restored_handler(struct window *window, void *data)
{
	struct stacking_window *stacking_window = window_get_user_data(window);
	if (stacking_window->window_session)
	{
		assert(strcmp(stacking_window->window_session->toplevel_id, window_session_get_toplevel_id(window)) == 0);
		stacking_window->window_session->active = true;
		printf("Window restored with ID \"%s\"\n", stacking_window->window_session->toplevel_id);
	}
	window_schedule_redraw(window);
}

int
main(int argc, char *argv[])
{
	struct stacking stacking;
	struct stacking_window *stacking_window, *tmp;
	struct stacking_session *stacking_session, *session_tmp;

	memset(&stacking, 0, sizeof stacking);

	wl_list_init(&stacking.window_list);
	wl_list_init(&stacking.session_list);

	stacking.display = display_create(&argc, argv);
	if (stacking.display == NULL) {
		fprintf(stderr, "Failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	display_set_user_data(stacking.display, &stacking);

	create_window(&stacking, NULL, NULL);

	display_run(stacking.display);

	wl_list_for_each_safe(stacking_session, session_tmp, &stacking.session_list, link)
		destroy_session(stacking_session);

	wl_list_for_each_safe(stacking_window, tmp, &stacking.window_list, link)
		destroy_window(stacking_window);

	display_destroy(stacking.display);

	return 0;
}

