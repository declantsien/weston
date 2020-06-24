/*
 * Copyright © 2020 Collabora, Ltd.
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

/* weston-clock is a component that abstracts time related functionality and
 * allows us to replace real time with fake time that can be controlled by test
 * clients.
 *
 * When fake time is enabled, all timers are stored in a list sorted by their
 * trigger time. When the fake time is advanced, these timers are scheduled in
 * order using idle sources. Each timer callback is invoked with a fake time
 * corresponding to the callback's trigger time, so that all code that runs in
 * the context of the callback uses this time rather than the final advanced
 * value.  A final idle source is scheduled after all timer sources to set the
 * fake time to its final advanced value.
 *
 * The timer idle sources and the final idle source are often rescheduled to
 * maintain correct callback order, in cases where, e.g., a timer is updated
 * from within a timer or idle callback.
 */

#include "config.h"

#include "weston-clock.h"

#include <assert.h>
#include <string.h>
#include <wayland-util.h>

#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "libweston/zalloc.h"
#include "libweston/libweston.h"
#include "weston-clock-server-protocol.h"

static clockid_t valid_clocks[] = {
	CLOCK_REALTIME,
	CLOCK_MONOTONIC,
	CLOCK_MONOTONIC_RAW,
	CLOCK_REALTIME_COARSE,
	CLOCK_MONOTONIC_COARSE,
#ifdef CLOCK_BOOTTIME
	CLOCK_BOOTTIME,
#endif
};

/* Holds information about added timers when using fake time. */
struct timer_info {
	struct wl_list link;
	struct weston_clock *clock;
	wl_event_loop_timer_func_t func;
	void *data;
	struct wl_event_source *source;
	struct timespec trigger_time;
	bool armed;
	struct wl_event_source *idle_source;
};

/* Holds information about the idle source that sets the final time after
 * advancing the fake time. */
struct final_time {
	struct wl_event_source *source;
	struct timespec target;
};

struct weston_clock {
	bool using_real_time;
	struct wl_event_loop *loop;
	struct timespec fake_clock_time;
	struct wl_list timer_info_list;
	struct final_time final_time;
};

static bool
is_clockid_valid(clockid_t clockid)
{
       size_t i;

       for (i = 0; i < ARRAY_LENGTH(valid_clocks); i++) {
               if (valid_clocks[i] == clockid)
                       return true;
       }

       return false;
}

static struct timer_info *
get_timer_info(struct weston_clock *clock, struct wl_event_source *source)
{
	struct timer_info *tinfo;

	wl_list_for_each(tinfo, &clock->timer_info_list, link) {
		if (tinfo->source == source)
			return tinfo;
	}

	return NULL;
}

/* Ensures that the provided timer is at the correct position in the
 * timer_info_list, sorted by its trigger time. */
static void
sort_timer_info(struct weston_clock *clock, struct timer_info *tinfo)
{
	struct timer_info *cur;

	wl_list_remove(&tinfo->link);

	wl_list_for_each_reverse(cur, &clock->timer_info_list, link) {
		if (timespec_sub_to_nsec(&tinfo->trigger_time, &cur->trigger_time) >= 0) {
			wl_list_insert(&cur->link, &tinfo->link);
			return;
		}
	}

	wl_list_insert(&clock->timer_info_list, &tinfo->link);
}

static void
schedule_timer_sources(struct weston_clock *clock, struct timespec *now);

static void
apply_final_time(void *data)
{
	struct weston_clock *clock = data;
	clock->fake_clock_time = clock->final_time.target;
	clock->final_time.source = NULL;
	schedule_timer_sources(clock, &clock->fake_clock_time);
}

/* Adds an idle source that sets the final time to the provided value.  If we
 * already have an active final time idle source, it is rescheduled.  It's
 * safe, and often necessary to call this even if the final time hasn't
 * changed, to ensure that the final time idle source is called in the correct
 * order relative to other idle sources (either fake timer idle sources, or
 * other idle sources scheduled by weston from within timer callbacks). */
static void
schedule_apply_final_time(struct weston_clock *clock, struct timespec *ts)
{
	clock->final_time.target = *ts;

	if (clock->final_time.source)
		wl_event_source_remove(clock->final_time.source);

	if (clock->loop) {
		clock->final_time.source =
			wl_event_loop_add_idle(clock->loop,
					       apply_final_time,
					       clock);
	} else {
		clock->fake_clock_time = *ts;
	}
}

/* Invokes the timer callback function for a timer.
 * The timer callback is invoked with a fake time set at the trigger time of the
 * timer. */
static void
invoke_timer_func(void *data)
{
	struct timer_info *tinfo = data;
	struct weston_clock *clock = tinfo->clock;

	/* Invoke the timer callback function with the fake time set at the
	 * timer target time. */
	clock->fake_clock_time = tinfo->trigger_time;

	timespec_from_nsec(&tinfo->trigger_time, 0);
	tinfo->armed = false;
	tinfo->idle_source = NULL;

	tinfo->func(tinfo->data);

	/* Reschedule all timer sources and final time to ensure the correct
	 * order of these in relation to any other idle sources added from within
	 * the callback. */
	schedule_timer_sources(clock, &clock->fake_clock_time);
	schedule_apply_final_time(clock, &clock->final_time.target);
}

/* Schedule all armed timer sources that occur at or before the specified time.
 * Any scheduled timer sources that have not yet been triggered are rescheduled
 * in the correct order. */
static void
schedule_timer_sources(struct weston_clock *clock, struct timespec *now)
{
	struct timer_info *cur;

	wl_list_for_each(cur, &clock->timer_info_list, link) {
		if (!cur->armed)
			continue;
		if (timespec_sub_to_nsec(&cur->trigger_time, now) > 0)
			break;

		if (cur->idle_source)
			wl_event_source_remove(cur->idle_source);
		cur->idle_source =
			wl_event_loop_add_idle(clock->loop, invoke_timer_func, cur);
	}

}

WL_EXPORT struct weston_clock *
weston_clock_create(enum weston_clock_type type, struct wl_event_loop *loop)
{
	struct weston_clock *clock;

	clock = zalloc(sizeof *clock);
	if (clock == NULL)
		return NULL;

	clock->using_real_time = (type == WESTON_CLOCK_TYPE_REAL);
	clock->loop = loop;
	wl_list_init(&clock->timer_info_list);

	return clock;
}

WL_EXPORT void
weston_clock_destroy(struct weston_clock *clock)
{
	/* In case of fake time all sources need to have been destroyed,
	 * otherwise they may refer to this destroyed clock when triggered.
	 */
	assert(wl_list_empty(&clock->timer_info_list));

	if (clock->final_time.source)
		wl_event_source_remove(clock->final_time.source);

	free(clock);
}

WL_EXPORT int
weston_clock_gettime(struct weston_clock *clock, clockid_t clockid, struct timespec *ts)
{
	if (clock->using_real_time)
		return clock_gettime(clockid, ts);

	if (!is_clockid_valid(clockid))
		return -1;

	*ts = clock->fake_clock_time;

	return 0;
}

WL_EXPORT int
weston_clock_advance_time(struct weston_clock *clock, const struct timespec *ts)
{
	struct timespec now;

	if (clock->using_real_time)
		return -1;

	timespec_add(&now, &clock->fake_clock_time, ts);
	schedule_timer_sources(clock, &now);
	schedule_apply_final_time(clock, &now);

	return 0;
}

WL_EXPORT struct wl_event_source *
weston_clock_add_timer(struct weston_clock *clock,
		       wl_event_loop_timer_func_t func,
		       void *data)
{
	struct wl_event_source *timer;
	struct timer_info *tinfo;

	/* We create a timer source even when using fake time, in order to
	 * have a stable handle to identify this source. */
	timer = wl_event_loop_add_timer(clock->loop, func, data);
	if (clock->using_real_time)
		return timer;

	tinfo = zalloc(sizeof *tinfo);
	if (tinfo == NULL)
		return NULL;

	tinfo->clock = clock;
	tinfo->func = func;
	tinfo->data = data;
	tinfo->source = timer;

	/* We add this timer at the start of the list. Since the timer is not
	 * armed its order in the list doesn't matter. */
	wl_list_insert(&clock->timer_info_list, &tinfo->link);

	return tinfo->source;
}

WL_EXPORT void
weston_clock_timer_update(struct weston_clock *clock,
			  struct wl_event_source *source,
		          int ms_delay)
{
	struct timer_info *tinfo;

	if (clock->using_real_time) {
		wl_event_source_timer_update(source, ms_delay);
		return;
	}

	tinfo = get_timer_info(clock, source);
	assert(tinfo);

	if (ms_delay > 0) {
		timespec_add_msec(&tinfo->trigger_time, &clock->fake_clock_time, ms_delay);
		tinfo->armed = true;
	} else {
		timespec_from_nsec(&tinfo->trigger_time, 0);
		tinfo->armed = false;
	}

	/* Update the position of this timer in the timer list and reschedule
	 * all timer sources and final time to ensure correct relative order. */
	sort_timer_info(clock, tinfo);
	schedule_timer_sources(clock, &clock->fake_clock_time);
	schedule_apply_final_time(clock, &clock->final_time.target);
}

WL_EXPORT void
weston_clock_timer_remove(struct weston_clock *clock,
			  struct wl_event_source *source)
{
	struct timer_info *tinfo;

	wl_event_source_remove(source);
	if (clock->using_real_time)
		return;

	tinfo = get_timer_info(clock, source);
	assert(tinfo);

	if (tinfo->idle_source)
		wl_event_source_remove(tinfo->idle_source);
	wl_list_remove(&tinfo->link);
	free(tinfo);
}

static void
weston_clock_advance_time_request(struct wl_client *client,
				  struct wl_resource *resource,
				  uint32_t tv_sec_hi,
				  uint32_t tv_sec_lo,
				  uint32_t tv_nsec)
{
	struct timespec ts;
	struct weston_compositor *compositor = wl_resource_get_user_data(resource);

	timespec_from_proto(&ts, tv_sec_hi, tv_sec_lo, tv_nsec);

	weston_clock_advance_time(compositor->clock, &ts);
}

static const struct weston_clock_interface weston_clock_implementation = {
	weston_clock_advance_time_request,
};

static void
bind_weston_clock(struct wl_client *client,
		  void *data, uint32_t version,
		  uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &weston_clock_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource,
				       &weston_clock_implementation,
				       compositor, NULL);
}

int
weston_clock_protocol_setup(struct weston_compositor *compositor)
{
	if (!wl_global_create(compositor->wl_display,
			      &weston_clock_interface,
			      1, compositor,
			      bind_weston_clock))
		return -1;

	return 0;
}
