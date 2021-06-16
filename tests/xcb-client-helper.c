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
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <poll.h>

#include <time.h>

#include <wayland-client.h>
#include "test-config.h"
#include "shared/os-compatibility.h"
#include "shared/helpers.h"
#include "shared/xalloc.h"
#include "shared/xcb-xwayland.h"
#include <libweston/zalloc.h>
#include "xcb-client-helper.h"
#include "weston-test-client-helper.h"

#define DEBUG

#ifdef DEBUG
#define printfd(fmt, args...) do {	\
	fprintf(stderr, fmt, ##args);	\
} while (0)
#else
#define printfd(fmt, args...)	{}
#endif

const char *to_event_name(uint8_t event);

static xcb_drawable_t
handle_event_to_wid(xcb_generic_event_t *ev)
{
	xcb_drawable_t wid;

	switch (EVENT_TYPE(ev)) {
	case XCB_CREATE_NOTIFY: {
		xcb_create_notify_event_t *ce = (xcb_create_notify_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_DESTROY_NOTIFY: {
		xcb_destroy_notify_event_t *ce = (xcb_destroy_notify_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_MAP_NOTIFY: {
		xcb_map_notify_event_t *ce = (xcb_map_notify_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_UNMAP_NOTIFY: {
		xcb_unmap_notify_event_t *ce = (xcb_unmap_notify_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *ce = (xcb_property_notify_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *ce = (xcb_client_message_event_t *) ev;
		wid = ce->window;
		break;
	}
	case XCB_KEY_PRESS: {
		xcb_key_press_event_t *ce = (xcb_key_press_event_t *) ev;
		wid = ce->root;
		break;
	}
	case XCB_EXPOSE: {
		xcb_expose_event_t *ce = (xcb_expose_event_t *) ev;
		wid = ce->window;
		break;
	}
	default:
		printfd("%s: unhandled event %d\n", __func__, EVENT_TYPE(ev));
		wid = 0;
	}

	return wid;
}

static bool
handle_event_check_pending(struct window_x11 *window, xcb_generic_event_t *ev,
			   bool ev_handled)
{
	xcb_drawable_t wid;
	uint8_t event;
	bool found = false;

	struct window_state *wstate, *wstate_next;
	struct wl_list *pending_events =
		&window->tentative_state.pending_events_list;
	event = EVENT_TYPE(ev);

	if (!ev_handled) {
		printfd("%s: event was not handled. "
			"Skipping removal from pending_list\n", __func__);
		return false;
	}

	wid = handle_event_to_wid(ev);

	printfd("%s: pending events to wait %d, received event %d - %s, wid %d\n",
		__func__, wl_list_length(pending_events), event,
		to_event_name(event), wid);

	wl_list_for_each_safe(wstate, wstate_next, pending_events, link) {
		if (wstate->event == event && wstate->wid == wid)  {
			wl_list_remove(&wstate->link);
			free(wstate);
			found = true;
			printfd("%s: removed event %d - %s\n", __func__,
				event, to_event_name(event));
			break;
		}

		/* for frame id, in case we might want to track notification
		 * events from it */
		if (wstate->event == event && event == XCB_CREATE_NOTIFY &&
		    wstate->wid == 0) {
			wl_list_remove(&wstate->link);
			free(wstate);
			found = true;
			printfd("%s: removed event %d - %s (not ours)\n", __func__,
				event, to_event_name(event));
			break;
		}
	}

	if (!found)
		return false;

	/* still need to get events? -> wait one more round */
	if (!wl_list_empty(pending_events))
		return false;

	return true;
}

/** Helper to explictly track notify events. In case you need to wait for a
 * notify event us this function to do so and call handle_events_x11() to wait
 * for them.
 *
 * \param window the window_x11 in question
 * \param event the event to wait for
 * \param wid the window id, could be different than that of the window itself
 */
void
handle_event_set_pending(struct window_x11 *window, uint8_t event,
			 xcb_drawable_t wid)
{
	struct window_state *wstate = zalloc(sizeof(*wstate));

	wstate->event = event;
	wstate->wid = wid;
	wl_list_insert(&window->tentative_state.pending_events_list,
		       &wstate->link);

	printfd("%s: Added pending event %d - %s, wid %d\n",
		__func__, event, to_event_name(event), wid);
}

/**
 * Sets the window's state. The window should *not* be mapped, or in
 * that state previously. Only MAXIMIZED or FULLSCREEN are allowed.
 *
 * After setting the window's state one should call handle_events_x11(), before
 * attempting on doing any other operations.
 *
 * \param window the window in question
 * \param state either MAXIMIZED or FULLSCREEN
 */
void
window_x11_set_state(struct window_x11 *window, enum w_state state)
{
	uint32_t property[3];
	uint32_t i = 0;

	assert(!(window->state.win_state & MAPPED));
	assert(state == MAXIMIZED || state == FULLSCREEN);

	assert(!(window->tentative_state.win_state & MAXIMIZED) ||
	       !(window->tentative_state.win_state & FULLSCREEN));

	if (state == MAXIMIZED) {
		property[i++] = window->atoms->net_wm_state_maximized_vert;
		property[i++] = window->atoms->net_wm_state_maximized_horz;

		window->tentative_state.win_state |= MAXIMIZED;
		window->tentative_state.start_maximized = true;
	} else if (state == FULLSCREEN) {
		property[i++] = window->atoms->net_wm_state_fullscreen;

		window->tentative_state.win_state |= FULLSCREEN;
		window->tentative_state.start_fullscreen = true;
	}

	handle_event_set_pending(window, XCB_PROPERTY_NOTIFY, window->win);

	xcb_change_property(window->connection, XCB_PROP_MODE_REPLACE,
			    window->win, window->atoms->net_wm_state,
			    XCB_ATOM_ATOM, 32, i, property);
	xcb_flush(window->connection);
}

/**
 * Move the window to the specified location
 *
 * After changing window's location one should call handle_events_x11(), before
 * attempting on doing any other operations.
 */
void
window_x11_move(struct window_x11 *window, int x, int y)
{
	uint32_t mask;
	uint32_t values[2];

	values[0] = x;
	values[1] = y;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;

	window->tentative_state.win_state |= CONFIGURED_MOVE;
	handle_event_set_pending(window, XCB_CONFIGURE_NOTIFY, window->win);

	xcb_configure_window(window->connection, window->win, mask, values);
	xcb_flush(window->connection);
}

/**
 * Resize the window to the specified size
 *
 * After changing window's size one should call handle_events_x11(), before
 * attempting on doing any other operations.
 */
void
window_x11_resize(struct window_x11 *window, int width, int height)
{
	uint32_t mask;
	uint32_t values[2];

	values[0] = width;
	values[1] = height;

	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

	window->tentative_state.win_state |= CONFIGURED_RESIZE;
	handle_event_set_pending(window, XCB_CONFIGURE_NOTIFY, window->win);

	xcb_configure_window(window->connection, window->win, mask, values);
	xcb_flush(window->connection);
}

/**
 * Move and resize the window
 *
 * After changing window's size/location one should call handle_events_x11(),
 * before attempting on doing any other operations.
 */
void
window_x11_move_resize(struct window_x11 *window, int x, int y, int width, int height)
{
	uint32_t mask;
	uint32_t values[4];

	values[0] = x;
	values[1] = y;
	values[2] = width;
	values[3] = height;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
	       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

	window->tentative_state.win_state |= CONFIGURED_MOVE_RESIZE;
	handle_event_set_pending(window, XCB_CONFIGURE_NOTIFY, window->win);

	xcb_configure_window(window->connection, window->win, mask, values);
	xcb_flush(window->connection);
}

/**
 * Toggle the window's state. This function will change the window's state
 * depending on a previous state, so a calling it twice would effectively
 * change the window state the original state.
 *
 * After a toggle state one should call handle_events_x11() in order to handle
 * all the pending events.
 *
 * \param window the window in question
 * \param state specify which state to apply, MAXIMIZED or FULLSCREEN. Any
 * other state passed will be rejected as it doesn't make sense using them.
 */
void
window_x11_toggle_state(struct window_x11 *window, enum w_state state)
{
#ifndef _NET_WM_STATE_REMOVE
#define _NET_WM_STATE_REMOVE        0    // remove/unset property
#endif

#ifndef _NET_WM_STATE_ADD
#define _NET_WM_STATE_ADD           1    // add/set property
#endif

#ifndef _NET_WM_STATE_TOGGLE
#define _NET_WM_STATE_TOGGLE        2    // toggle property
#endif

	xcb_client_message_event_t ev = {};
	uint32_t mask = (XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
	                XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY);

	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.type = window->atoms->net_wm_state;

	/* we only permit these states */
	assert(state == MAXIMIZED || state == FULLSCREEN);

	ev.format = 32;
	ev.window = window->win;

	/* ev.data.data32[0] = fullscreen ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE; */
	ev.data.data32[0] = _NET_WM_STATE_TOGGLE;

	if (state == FULLSCREEN) {
		ev.data.data32[1] = window->atoms->net_wm_state_fullscreen;
		ev.data.data32[2] = XCB_ATOM_NONE;
		ev.data.data32[3] = 0;
		ev.data.data32[4] = 0;

		if (window->state.win_state & FULLSCREEN)
			window->tentative_state.win_state |= NORMAL;
		else
			window->tentative_state.win_state |= FULLSCREEN;

	} else if (state == MAXIMIZED) {
		ev.data.data32[1] = window->atoms->net_wm_state_maximized_vert;
		ev.data.data32[2] = window->atoms->net_wm_state_maximized_horz;
		ev.data.data32[3] = XCB_ATOM_NONE;
		ev.data.data32[4] = 0;

		if (window->state.win_state & MAXIMIZED)
			window->tentative_state.win_state |= NORMAL;
		else
			window->tentative_state.win_state |= MAXIMIZED;
	}

	handle_event_set_pending(window, XCB_CLIENT_MESSAGE, window->win);
	handle_event_set_pending(window, XCB_PROPERTY_NOTIFY, window->win);

	/* a client message event, a property event and configure notify event
	 * are required for doing toggle on/off operations.
	 *
	 * in case the window was set as maximized or fullscreen (using either
	 * window_x11_set_maximized() or window_x11_set_fullscreen()), followed
	 * by a toggle operation, there wouldn't be a  need to wait for a
	 * configure notify event.
	 *
	 * In order to account for that, this verifies if the current state is
	 * either maximized or fullscreen which happened due to a previous
	 * configure notify event (as a result of previously calling the
	 * window_x11_set_*() functions) */
	if (!window->state.start_maximized && !window->state.start_fullscreen) {
		if (window->state.win_state & CONFIGURED_MAX)
			window->tentative_state.win_state |= CONFIGURED_NORMAL;
		else
			window->tentative_state.win_state |= CONFIGURED_FULL;

		if (window->state.win_state & CONFIGURED_FULL)
			window->tentative_state.win_state |= CONFIGURED_NORMAL;
		else
			window->tentative_state.win_state |= CONFIGURED_FULL;

		handle_event_set_pending(window, XCB_CONFIGURE_NOTIFY, window->win);
	}

	xcb_send_event(window->connection, 1, window->root_win, mask, (char *) &ev);
}

/**
 * Resets the internal window's state
 *
 * Maybe necessary as handle_events_x11() and its events modifies the states.
 */
void
window_x11_reset_internal_state(struct window_x11 *window)
{
	window->tentative_state.win_state = 0;
	window->state.win_state = 0;
}

static bool
handle_expose(xcb_generic_event_t *e, struct window_x11 *window)
{
	return true;
}

static bool
handle_client_message(xcb_generic_event_t *e, struct window_x11 *window)
{
	return true;
}

static bool
handle_keypress(xcb_generic_event_t *e, struct window_x11 *window)
{
	return true;
}

static bool
handle_map_notify(xcb_generic_event_t *e, struct window_x11 *window)
{
	xcb_map_notify_event_t *ce = (xcb_map_notify_event_t *) e;

	if (window->win == ce->window) {
		window->tentative_state.win_state &= ~MAPPED;
		window->state.win_state |= MAPPED;
	}

	return true;
}

static bool
handle_unmap_notify(xcb_generic_event_t *e, struct window_x11 *window)
{
	xcb_unmap_notify_event_t *ce = (xcb_unmap_notify_event_t*) e;

	if (window->win == ce->window) {
		assert(window->state.win_state & MAPPED);
		window->tentative_state.win_state &= ~UNMAPPED;
		window->state.win_state &= ~MAPPED;
		window->state.win_state |= UNMAPPED;
	}

	return true;
}

static bool
handle_destroy_notify(xcb_generic_event_t *e, struct window_x11 *window)
{
	xcb_destroy_notify_event_t *ce = (xcb_destroy_notify_event_t*) e;

	if (window->win == ce->window) {
		assert(window->state.win_state & CREATED);
		window->state.win_state &= ~CREATED;
		window->state.win_state |= DESTROYED;
		window->tentative_state.win_state &= ~DESTROYED;
	}

	return true;
}

static bool
handle_configure_notify(xcb_generic_event_t *e, struct window_x11 *window)
{
	xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t*) e;
	bool ev_handled = false;

	/* we're not interested into others windows */
	if (ce->window != window->win)
		return true;

	if (window->configured_state.width != ce->width &&
	    window->configured_state.height != ce->height) {
		window->configured_state.width = ce->width;
		window->configured_state.height = ce->height;
		window->configured_state.updated = true;
		printfd("%s: update configured_state, width %d, height %d\n",
			__func__, window->configured_state.width,
			window->configured_state.height);
	}

	if (window->configured_state.updated) {
		bool size_diff = window->configured_state.width != window->width &&
				 window->configured_state.height != window->height;

		/* for coming back to normal, from maximized or fullscreen, so we
		 * are at the original size/geometry */
		bool size_match = !size_diff;

		window->configured_state.updated = false;

		printfd("%s: configured_state updated, configured_state differs %s\n",
			__func__, size_diff == true ? "yes" : "no");

		if ((window->tentative_state.win_state & CONFIGURED_MAX) &&
		    size_diff) {
			window->state.win_state |= CONFIGURED_MAX;
			window->tentative_state.win_state &= ~CONFIGURED_MAX;
			ev_handled = true;
			printfd("%s: (CONFIGURE_NOTIFY) state set to MAXIMIZED\n", __func__);
		}

		if ((window->tentative_state.win_state & CONFIGURED_FULL) &&
		     size_diff) {
			window->state.win_state |= CONFIGURED_FULL;
			window->tentative_state.win_state &= ~CONFIGURED_FULL;
			ev_handled = true;
			printfd("%s: (CONFIGURE_NOTIFY) state set to FULLSCREEN\n", __func__);
		}

		/* resize back to normal */
		if ((window->tentative_state.win_state & CONFIGURED_NORMAL) &&
		     size_match) {
			window->state.win_state |= CONFIGURED_NORMAL;
			window->tentative_state.win_state &= ~CONFIGURED_NORMAL;
			ev_handled = true;
			printfd("%s: (CONFIGURE_NOTIFY) state set to normal\n", __func__);
		}

		if ((window->tentative_state.win_state & CONFIGURED_MOVE) &&
		    size_diff) {
			window->state.win_state |= CONFIGURED_MOVE;
			window->tentative_state.win_state &= ~CONFIGURED_MOVE;
			ev_handled = true;

			window->pos_x = ce->x;
			window->pos_y = ce->y;

			printfd("%s: (CONFIGURE_NOTIFY) state set to MOVE\n", __func__);
		}

		if ((window->tentative_state.win_state & CONFIGURED_RESIZE) &&
		     size_diff) {
			window->state.win_state |= CONFIGURED_RESIZE;
			window->tentative_state.win_state &= ~CONFIGURED_RESIZE;
			ev_handled = true;

			window->width = ce->width;
			window->height = ce->height;

			printfd("%s: (CONFIGURE_NOTIFY) state set to RESIZE\n", __func__);
		}

		if ((window->tentative_state.win_state & CONFIGURED_MOVE_RESIZE) &&
		    size_diff) {
			window->state.win_state |= CONFIGURED_MOVE_RESIZE;

			window->pos_x = ce->x;
			window->pos_y = ce->y;
			window->width = ce->width;
			window->height = ce->height;

			window->tentative_state.win_state &= ~CONFIGURED_MOVE_RESIZE;
			ev_handled = true;
			printfd("%s: (CONFIGURE_NOTIFY) state set to MOVE_RESIZE\n", __func__);
		}
	}

	return ev_handled;
}

static bool
handle_property_notify(xcb_generic_event_t *e, struct window_x11 *window)
{
	xcb_property_notify_event_t *ce = (xcb_property_notify_event_t *) e;

	if (ce->window != window->win)
		return true;

	if (ce->atom == window->atoms->net_wm_state) {
		if (window->tentative_state.win_state & FULLSCREEN) {
			if (window->state.win_state & MAXIMIZED)
				window->state.win_state &= ~MAXIMIZED;

			window->state.win_state |= FULLSCREEN;

			window->tentative_state.win_state &= ~FULLSCREEN;
			printfd("%s: state set to FULLSCREEN\n", __func__);
		} else if (window->tentative_state.win_state & MAXIMIZED) {
			if (window->state.win_state & FULLSCREEN)
				window->state.win_state &= ~FULLSCREEN;

			window->state.win_state |= MAXIMIZED;
			window->tentative_state.win_state &= ~MAXIMIZED;
			printfd("%s: state set to MAXIMIZED\n", __func__);
		} else if (window->tentative_state.win_state & NORMAL) {
			if (window->state.win_state & MAXIMIZED)
				window->state.win_state &= ~MAXIMIZED;
			if (window->state.win_state & FULLSCREEN)
				window->state.win_state &= ~FULLSCREEN;

			window->state.win_state |= NORMAL;
			window->tentative_state.win_state &= ~NORMAL;
			printfd("%s: state set to NORMAL\n", __func__);
		}

		if (window->tentative_state.start_maximized) {
			window->tentative_state.start_maximized	= false;
			window->state.start_maximized = true;
		}

		if (window->tentative_state.start_fullscreen) {
			window->tentative_state.start_fullscreen = false;
			window->state.start_fullscreen = true;
		}

	} else if (ce->atom == window->atoms->net_wm_name) {
		window->state.win_state |= PROPERTY_NAME;
		printfd("%s: net_wm_name set\n", __func__);
	}

	return true;
}

static bool
handle_create_notify(xcb_generic_event_t *e, struct window_x11 *window)
{
	xcb_create_notify_event_t *ce = (xcb_create_notify_event_t*) e;

	/* confirm that we are positioned, and set with the correct size */
	if (ce->window == window->win) {
		if (window->pos_x == ce->x && window->pos_y == ce->y &&
		    window->width == ce->width && window->height == ce->height) {
			window->tentative_state.win_state &= ~CREATED;
			window->state.win_state |= CREATED;
		}
	} else if (ce->window != window->win && window->frame_id == 0) {
		/* in case we need the frame's window id */
		window->frame_id = ce->window;
	}

	return true;
}

/* the event handlers should return a boolean that denotes that fact
 * they've been handled. One can customize that behaviour such that
 * it forces handle_events_x11() to wait for additional events, in case
 * that's needed. */
static const struct event_response events[] = {
	{ XCB_CLIENT_MESSAGE, handle_client_message, "CLIENT_MESSAGE" },
	{ XCB_KEY_PRESS, handle_keypress, "KEY_PRESS" },
	{ XCB_EXPOSE, handle_expose, "EXPOSE" },

	{ XCB_MAP_NOTIFY, handle_map_notify, "MAP_NOTIFY" },
	{ XCB_UNMAP_NOTIFY, handle_unmap_notify, "UNMAP_NOTIFY" },
	{ XCB_PROPERTY_NOTIFY, handle_property_notify, "PROPERTY_NOTIFY" },
	{ XCB_CONFIGURE_NOTIFY, handle_configure_notify, "CONFIGURE_NOTIFY" },

	{ XCB_CREATE_NOTIFY, handle_create_notify, "CREATE_NOTIFY" },
	{ XCB_DESTROY_NOTIFY, handle_destroy_notify, "DESTROY_NOTIFY" },
};

const char *
to_event_name(uint8_t event)
{
	size_t i;
	for (i = 0; i < ARRAY_LENGTH(events); i++)
		if (events[i].response_type == event)
			return events[i].name;

	return "(unknown event)";
}

/**
 * Tells the X server to display the window. 
 * Would add the map notify event to the list of pending events to wait for.
 *
 */
void
window_x11_map(struct window_x11 *window)
{
	handle_event_set_pending(window, XCB_MAP_NOTIFY, window->win);

	xcb_map_window(window->connection, window->win);
	xcb_flush(window->connection);
}

/**
 * \sa window_x11_map. Tells the X server to unmap the window.
 * Would add the unmap notify event to the list of pending events to wait for.
 *
 */
void
window_x11_unmap(struct window_x11 *window)
{
	handle_event_set_pending(window, XCB_UNMAP_NOTIFY, window->win);

	xcb_unmap_window(window->connection, window->win);
	xcb_flush(window->connection);
}

static xcb_generic_event_t *
wait_for_event(xcb_connection_t *conn)
{
	int fd = xcb_get_file_descriptor(conn);
	struct pollfd pollfds = {};
	int rpol;

	pollfds.fd = fd;
	pollfds.events = POLLIN;

	rpol = ppoll(&pollfds, 1, NULL, NULL);
	if (rpol > 0 && (pollfds.revents & POLLIN))
		return xcb_wait_for_event(conn);

	return NULL;
}

static void
window_x11_set_cursor(struct window_x11 *window)
{
	assert(window);

	if (xcb_cursor_context_new(window->connection, window->screen, &window->ctx) < 0) {
		fprintf(stderr, "Error creating context!\n");
		return;
	}

	window->cursor = xcb_cursor_load_cursor(window->ctx, "left_ptr");

	xcb_change_window_attributes(window->connection,
				     window->root_win, XCB_CW_CURSOR,
				     &window->cursor);
	xcb_flush(window->connection);
}

static int
handle_events(struct window_x11 *window)
{
	bool running = true;
	xcb_generic_event_t *ev;
	int ret = 0;

	assert(window->handle_in_progress == false);

	do {
		uint8_t event;
		bool events_handled = false;
		bool ev_handled = false;
		unsigned int i;

		xcb_flush(window->connection);

		if (xcb_connection_has_error(window->connection)) {
			fprintf(stderr, "X11 connection got interrupted\n");
			ret = -1;
			break;
		}

		window->handle_in_progress = true;
		ev = wait_for_event(window->connection);
		if (!ev) {
			fprintf(stderr, "Error, no event received, although we requested for one!\n");
			break;
		}

		event = EVENT_TYPE(ev);
		for (i = 0; i < ARRAY_LENGTH(events); i++) {
			if (event == events[i].response_type) {
				ev_handled = events[i].eventcb(ev, window);
				events_handled =
					handle_event_check_pending(window, ev,
								   ev_handled);
			}
		}

		/* signals that we've done processing all the pending events */
		if (events_handled) {
			running = false;
		}

		free(ev);
	} while (running);

	window->handle_in_progress = false;
	return ret;
}

/**
 * Each operation on 'window_x11' requires calling handle_events_x11() to call
 * the appropriate event handler, to flush out the connection, and to poll for
 * xcb_generic_event_t (not particularly in this order).
 *
 * This function should never block to allow a programmatic way of applying
 * different operations/states to the window. If that happens, running it
 * under meson test will cause a test fail (with a timeout).
 *
 * Before calling this function, one *shall* use handle_event_set_pending() to
 * explicitly set which events to wait for. Not doing so will effectively
 * deadlock the test, as it will wait for pending events never being set.
 *
 * Note that all state change functions, including map and unmap, would
 * implicitly, if not otherwise stated, set the pending events to wait for.
 *
 * \param window the X11 window in question
 */
int
handle_events_x11(struct window_x11 *window)
{
	return handle_events(window);
}

/* Required in order to receive create notify for our own window. */
static void
create_notify_event_for_root_window(struct window_x11 *window)
{
	int mask_values =
		XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

	xcb_change_window_attributes(window->connection,
				     window->root_win,
				     XCB_CW_EVENT_MASK, &mask_values);
	xcb_flush(window->connection);
}

void
window_x11_set_win_name(struct window_x11 *window, const char *name)
{
	handle_event_set_pending(window, XCB_PROPERTY_NOTIFY, window->win);

	xcb_change_property(window->connection, XCB_PROP_MODE_REPLACE,
			    window->win, window->atoms->net_wm_name,
			    window->atoms->string, 8,
			    strlen(name), name);
	xcb_flush(window->connection);
}

/**
 * creates a X window, based on the initial values. All operations work on this
 * window_x11. The creation and destruction of the window_x11 is handled
 * implictly so there's no need wait for (additional) events, like
 * it is required for change state operations.
 *
 * The window is not mapped/shown so that needs to happen explictly, by calling
 * window_x11_map() and then waiting for events using handle_events_x11().
 *
 * \param width initial size, width value
 * \param height initial size, height value
 * \param pos_x initial position, x value
 * \param pos_y initial position, y value
 * \param bg_color a background color
 * \param parent the window_x11 parent
 * \return a pointer to window_x11, which gets destroyed with destroy_x11_window()
 */
struct window_x11 *
create_x11_window(int width, int height, int pos_x, int pos_y,
		  pixman_color_t bg_color, struct window_x11 *parent)
{
	uint32_t colorpixel = 0x0;
	uint32_t values[2];
	uint32_t mask = 0;

	xcb_colormap_t colormap;
	struct window_x11 *window = zalloc(sizeof(*window));
	xcb_window_t parent_win;
	xcb_alloc_color_cookie_t cookie;
	xcb_alloc_color_reply_t *reply;

	if (access(XSERVER_PATH, X_OK) != 0)
		return NULL;

	window = zalloc(sizeof(*window));
	if (!window)
		return NULL;

	window->connection = xcb_connect(NULL, NULL);
	if (!window->connection)
		return NULL;

	window->screen =
		xcb_setup_roots_iterator(xcb_get_setup(window->connection)).data;

	wl_list_init(&window->window_list);
	wl_list_init(&window->tentative_state.pending_events_list);

	window->root_win = window->screen->root;
	window->parent = parent;
	if (window->parent) {
		parent_win = window->parent->win;
		wl_list_insert(&parent->window_list, &window->window_link);
	} else {
		parent_win = window->root_win;
	}

	create_notify_event_for_root_window(window);
	window_x11_reset_internal_state(window);

	colormap = window->screen->default_colormap;
	cookie = xcb_alloc_color(window->connection, colormap,
				 bg_color.red, bg_color.blue, bg_color.green);
	reply = xcb_alloc_color_reply(window->connection, cookie, NULL);
	assert(reply);

	colorpixel = reply->pixel;
	free(reply);

	window->background = xcb_generate_id(window->connection);
	mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
	values[0]  = colorpixel;
	values[1] = 0;
	window->bg_color = bg_color;

	xcb_create_gc(window->connection, window->background,
		      window->root_win, mask, values);

	/* create the window */
	window->win = xcb_generate_id(window->connection);
	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	values[0] = colorpixel;
	values[1] = XCB_EVENT_MASK_EXPOSURE |
		    XCB_EVENT_MASK_KEY_PRESS |
		    XCB_EVENT_MASK_PROPERTY_CHANGE |
		    XCB_EVENT_MASK_VISIBILITY_CHANGE |
		    XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

	window->pos_x = pos_x;
	window->pos_y = pos_y;

	window->width = width;
	window->height = height;

	xcb_create_window(window->connection,
			  XCB_COPY_FROM_PARENT,
			  window->win, parent_win,
			  window->pos_x, window->pos_y,
			  window->width, window->height,
			  10,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  window->screen->root_visual, mask, values);

	handle_event_set_pending(window, XCB_CREATE_NOTIFY, window->win);
	handle_events(window);
	assert(window->state.win_state & CREATED);

	window_x11_set_cursor(window);

	return window;
}

static void
kill_window(struct window_x11 *window)
{
	handle_event_set_pending(window, XCB_DESTROY_NOTIFY, window->win);

	xcb_destroy_window(window->connection, window->win);
	xcb_flush(window->connection);
}

/**
 * \sa create_x11_window(). The creation and destruction of the window_x11 is
 * handled implicitly so there's no wait for (additional) events.
 *
 * This function would wait for destroy notify event and will disconnect from
 * the x11. No further operation can happen on the window_x11. Call this function
 * when you're done with the test.
 */
void
destroy_x11_window(struct window_x11 *window)
{
	struct window_state *wstate, *wstate_next;

	xcb_free_cursor(window->connection, window->cursor);
	xcb_cursor_context_free(window->ctx);
	xcb_flush(window->connection);

	kill_window(window);
	handle_events(window);

	xcb_disconnect(window->connection);

	/* in case we're called before any events have been handled */
	wl_list_for_each_safe(wstate, wstate_next,
			      &window->tentative_state.pending_events_list, link) {
		wl_list_remove(&wstate->link);
		free(wstate);
	}
	free(window);
}

/**
 * Return the reply_t for an atom
 *
 * \param window the window in question
 * \param win the handle for the window; could be different from the window itself!
 * \param atom the atom in question
 *
 */
xcb_get_property_reply_t *
window_x11_dump_prop(struct window_x11 *window, xcb_drawable_t win, xcb_atom_t atom)
{
	xcb_get_property_cookie_t prop_cookie;
	xcb_get_property_reply_t *prop_reply;

	prop_cookie = xcb_get_property(window->connection, 0, win, atom,
				      XCB_GET_PROPERTY_TYPE_ANY, 0, 2048);

	prop_reply = xcb_get_property_reply(window->connection, prop_cookie, NULL);

	/* callers needs to free it */
	return prop_reply;
}

void
window_x11_set_atoms(struct window_x11 *window, struct atom_x11 *atoms)
{
	window->atoms = atoms;
}
