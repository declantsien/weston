/*
 * Copyright © 2020 Collabora Ltd
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

#ifndef WESTON_CLOCK_H
#define WESTON_CLOCK_H

#include <time.h>
#include <wayland-server.h>

struct weston_clock;

enum weston_clock_type {
	WESTON_CLOCK_TYPE_REAL,
	WESTON_CLOCK_TYPE_FAKE,
};

struct weston_clock *
weston_clock_create(enum weston_clock_type type, struct wl_event_loop *loop);

void
weston_clock_destroy(struct weston_clock *clock);

int
weston_clock_gettime(struct weston_clock *clock,
		     clockid_t clockid, struct timespec *ts);

int
weston_clock_advance_time(struct weston_clock *clock,
			  const struct timespec *ts);

struct wl_event_source *
weston_clock_add_timer(struct weston_clock *clock,
		       wl_event_loop_timer_func_t func,
		       void *data);

void
weston_clock_timer_update(struct weston_clock *clock,
			  struct wl_event_source *source,
		          int ms_delay);

void
weston_clock_timer_remove(struct weston_clock *clock,
			  struct wl_event_source *source);

#endif
