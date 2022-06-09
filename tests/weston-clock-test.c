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

#include <assert.h>
#include "weston-clock.h"

#include "shared/timespec-util.h"

#include "zunitc/zunitc.h"

ZUC_TEST(weston_clock_test, initial_fake_time_is_zero)
{
	struct timespec ts;
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, NULL);

	weston_clock_gettime(clock, CLOCK_MONOTONIC, &ts);
	ZUC_ASSERT_TRUE(timespec_is_zero(&ts));

	weston_clock_destroy(clock);
}

ZUC_TEST(weston_clock_test, fake_time_is_set_for_all_clocks)
{
	struct timespec expected = {123, 456};
	struct timespec ts = {0, 0};
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, NULL);

	weston_clock_advance_time(clock, &expected);

	weston_clock_gettime(clock, CLOCK_MONOTONIC, &ts);
	weston_clock_gettime(clock, CLOCK_MONOTONIC_RAW, &ts);
	weston_clock_gettime(clock, CLOCK_REALTIME, &ts);

	ZUC_ASSERT_TRUE(timespec_eq(&ts, &expected));

	weston_clock_destroy(clock);
}

static int
increase(void *data)
{
	*((int*)data) += 1;
	return 0;
}

ZUC_TEST(weston_clock_test, disarmed_timer_is_not_triggered_with_fake_time)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered = 0;
	struct wl_event_source *timer;

	timer = weston_clock_add_timer(clock, increase, &timer_triggered);

	timespec_from_msec(&ts, 1);
	weston_clock_advance_time(clock, &ts);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 0);

	weston_clock_timer_remove(clock, timer);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

ZUC_TEST(weston_clock_test, fake_time_triggers_timer_at_correct_time)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered = 0;
	struct wl_event_source *timer;

	timer = weston_clock_add_timer(clock, increase, &timer_triggered);
	weston_clock_timer_update(clock, timer, 3);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 0);

	timespec_from_msec(&ts, 2);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 0);

	timespec_from_msec(&ts, 1);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 1);

	weston_clock_timer_remove(clock, timer);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

ZUC_TEST(weston_clock_test, timer_is_disarmed_after_trigger)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered = 0;
	struct wl_event_source *timer;

	timer = weston_clock_add_timer(clock, increase, &timer_triggered);
	weston_clock_timer_update(clock, timer, 3);

	timespec_from_msec(&ts, 3);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 1);

	timespec_from_msec(&ts, 1);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 1);

	weston_clock_timer_remove(clock, timer);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

ZUC_TEST(weston_clock_test, timer_can_be_triggered_only_once_by_fake_time_update)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered = 0;
	struct wl_event_source *timer;

	timer = weston_clock_add_timer(clock, increase, &timer_triggered);
	weston_clock_timer_update(clock, timer, 3);

	timespec_from_msec(&ts, 3);
	weston_clock_advance_time(clock, &ts);
	timespec_from_msec(&ts, 1);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);

	ZUC_ASSERT_EQ(timer_triggered, 1);

	weston_clock_timer_remove(clock, timer);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

struct timer_data {
	int id;
	struct wl_array *triggers;
};

static int
append_trigger(void *data)
{
	struct timer_data *tdata = data;
	int *trigger_id = wl_array_add(tdata->triggers, sizeof(int));
	*trigger_id = tdata->id;
	return 0;
}

ZUC_TEST(weston_clock_test, fake_time_triggers_timers_in_timeout_order)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	struct wl_array triggers;
	struct timer_data timer_data1 = { .id = 1, .triggers = &triggers };
	struct timer_data timer_data2 = { .id = 2, .triggers = &triggers };
	struct timer_data timer_data3 = { .id = 3, .triggers = &triggers };
	struct wl_event_source *timer1;
	struct wl_event_source *timer2;
	struct wl_event_source *timer3;

	wl_array_init(&triggers);

	timer1 = weston_clock_add_timer(clock, append_trigger, &timer_data1);
	timer2 = weston_clock_add_timer(clock, append_trigger, &timer_data2);
	timer3 = weston_clock_add_timer(clock, append_trigger, &timer_data3);

	weston_clock_timer_update(clock, timer2, 2);
	weston_clock_timer_update(clock, timer1, 1);
	weston_clock_timer_update(clock, timer3, 3);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(triggers.size, 0);

	timespec_from_msec(&ts, 3);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);

	ZUC_ASSERT_EQ(triggers.size, 3 * sizeof(int));
	ZUC_ASSERT_EQ(((int*)triggers.data)[0], 1);
	ZUC_ASSERT_EQ(((int*)triggers.data)[1], 2);
	ZUC_ASSERT_EQ(((int*)triggers.data)[2], 3);

	weston_clock_timer_remove(clock, timer1);
	weston_clock_timer_remove(clock, timer2);
	weston_clock_timer_remove(clock, timer3);
	wl_array_release(&triggers);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

ZUC_TEST(weston_clock_test, removed_timer_does_not_trigger_with_fake_time)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered[3] = { 0, 0, 0 };
	struct wl_event_source *timer[3];

	timer[0] = weston_clock_add_timer(clock, increase, &timer_triggered[0]);
	weston_clock_timer_update(clock, timer[0], 1);
	timer[1] = weston_clock_add_timer(clock, increase, &timer_triggered[1]);
	weston_clock_timer_update(clock, timer[1], 2);
	timer[2] = weston_clock_add_timer(clock, increase, &timer_triggered[2]);
	weston_clock_timer_update(clock, timer[2], 2);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered[0], 0);
	ZUC_ASSERT_EQ(timer_triggered[1], 0);
	ZUC_ASSERT_EQ(timer_triggered[2], 0);

	weston_clock_timer_remove(clock, timer[0]);
	weston_clock_timer_remove(clock, timer[2]);

	timespec_from_msec(&ts, 2);
	weston_clock_advance_time(clock, &ts);
	wl_event_loop_dispatch(loop, 0);

	ZUC_ASSERT_EQ(timer_triggered[0], 0);
	ZUC_ASSERT_EQ(timer_triggered[1], 1);
	ZUC_ASSERT_EQ(timer_triggered[2], 0);

	weston_clock_timer_remove(clock, timer[1]);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

ZUC_TEST(weston_clock_test, timer_activated_by_fake_time_can_be_removed_before_dispatch)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered = 0;
	struct wl_event_source *timer;

	timer = weston_clock_add_timer(clock, increase, &timer_triggered);
	weston_clock_timer_update(clock, timer, 3);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 0);

	timespec_from_msec(&ts, 2);
	weston_clock_advance_time(clock, &ts);

	/* Although the timer has been activated by advancing the fake time,
	 * we can still safely remove it before dispatch. */
	weston_clock_timer_remove(clock, timer);
	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 0);

	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

struct schedule_timer_ctx {
	struct weston_clock *clock;
	struct wl_event_source *timer;
	int time_ms;
};

static int
schedule_timer(void *data)
{
	struct schedule_timer_ctx *ctx = data;
	weston_clock_timer_update(ctx->clock, ctx->timer, ctx->time_ms);
	return 0;
}

ZUC_TEST(weston_clock_test, timer_scheduled_by_other_timer_triggers_at_correct_time)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer1_triggered = 0;
	struct wl_event_source *timer1;
	struct wl_event_source *timer2;
	struct schedule_timer_ctx ctx2 = { .clock = clock, .time_ms = 3 };

	timer1 = weston_clock_add_timer(clock, increase, &timer1_triggered);
	ctx2.timer = timer1;
	timer2 = weston_clock_add_timer(clock, schedule_timer, &ctx2);
	weston_clock_timer_update(clock, timer2, 3);

	timespec_from_msec(&ts, 6);
	weston_clock_advance_time(clock, &ts);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer1_triggered, 1);

	weston_clock_timer_remove(clock, timer1);
	weston_clock_timer_remove(clock, timer2);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

static void
idle_schedule_timer(void *data)
{
	struct schedule_timer_ctx *ctx = data;
	weston_clock_timer_update(ctx->clock, ctx->timer, ctx->time_ms);
}

ZUC_TEST(weston_clock_test, timer_scheduled_by_idle_triggers_at_correct_time)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer_triggered = 0;
	struct wl_event_source *timer;
	struct schedule_timer_ctx ctx = { .clock = clock, .time_ms = 3 };

	timer = weston_clock_add_timer(clock, increase, &timer_triggered);
	ctx.timer = timer;
	wl_event_loop_add_idle(loop, idle_schedule_timer, &ctx);

	timespec_from_msec(&ts, 3);
	weston_clock_advance_time(clock, &ts);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer_triggered, 1);

	weston_clock_timer_remove(clock, timer);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

struct idle_store_time_ctx {
	struct wl_event_loop *loop;
	struct weston_clock *clock;
	struct timespec time;
};

static void
idle_store_time(void *data)
{
	struct idle_store_time_ctx *ctx = data;
	weston_clock_gettime(ctx->clock, CLOCK_MONOTONIC, &ctx->time);
}

static int
schedule_idle_store_time(void *data)
{
	struct idle_store_time_ctx *ctx = data;
	wl_event_loop_add_idle(ctx->loop, idle_store_time, ctx);
	return 0;
}

ZUC_TEST(weston_clock_test, idle_scheduled_by_timer_triggers_at_correct_time)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	struct wl_event_source *timer;
	struct idle_store_time_ctx ctx = { .loop = loop, .clock = clock, .time = {0, 0} };

	timer = weston_clock_add_timer(clock, schedule_idle_store_time, &ctx);
	weston_clock_timer_update(clock, timer, 3);

	timespec_from_msec(&ts, 6);
	weston_clock_advance_time(clock, &ts);

	wl_event_loop_dispatch(loop, 0);
	/* The idle callback should have been triggered in the context of the timer
	 * which was scheduled for +3 ms. */
	timespec_from_msec(&ts, 3);
	ZUC_ASSERT_TRUE(timespec_eq(&ctx.time, &ts));

	weston_clock_timer_remove(clock, timer);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}

struct through_idle_ctx {
	struct wl_event_loop *loop;
	void *data;
};

static int
schedule_timer_through_idle(void *data)
{
	struct through_idle_ctx *ctx = data;
	wl_event_loop_add_idle(ctx->loop,
			       idle_schedule_timer, ctx->data);
	return 0;
}

static void
idle_increase(void *data)
{
	*((int*)data) += 1;
}

static int
increase_through_idle(void *data)
{
	struct through_idle_ctx *ctx = data;
	wl_event_loop_add_idle(ctx->loop, idle_increase, ctx->data);
	return 0;
}

ZUC_TEST(weston_clock_test, multiple_chained_events_are_triggered_by_single_advance)
{
	struct timespec ts = {0, 0};
	struct wl_event_loop *loop = wl_event_loop_create();
	struct weston_clock *clock = weston_clock_create(WESTON_CLOCK_TYPE_FAKE, loop);
	int timer1_triggered = 0;
	struct wl_event_source *timer1;
	struct wl_event_source *timer2;
	struct wl_event_source *timer3;
	struct wl_event_source *timer4;
	struct through_idle_ctx ctx1 = { .loop = loop, .data = &timer1_triggered };
	struct schedule_timer_ctx ctx2_sched = { .clock = clock, .time_ms = 3 };
	struct through_idle_ctx ctx2 = { .loop = loop, .data = &ctx2_sched };
	struct schedule_timer_ctx ctx3 = { .clock = clock, .time_ms = 3 };
	struct schedule_timer_ctx ctx4 = { .clock = clock, .time_ms = 3 };
	struct schedule_timer_ctx ctx5 = { .clock = clock, .time_ms = 3 };

	timer1 = weston_clock_add_timer(clock, increase_through_idle, &ctx1);
	ctx2_sched.timer = timer1;
	timer2 = weston_clock_add_timer(clock, schedule_timer_through_idle, &ctx2);
	ctx3.timer = timer2;
	timer3 = weston_clock_add_timer(clock, schedule_timer, &ctx3);
	ctx4.timer = timer3;
	timer4 = weston_clock_add_timer(clock, schedule_timer, &ctx4);
	ctx5.timer = timer4;
	wl_event_loop_add_idle(loop, idle_schedule_timer, &ctx5);

	timespec_from_msec(&ts, 12);
	weston_clock_advance_time(clock, &ts);

	wl_event_loop_dispatch(loop, 0);
	ZUC_ASSERT_EQ(timer1_triggered, 1);

	weston_clock_timer_remove(clock, timer1);
	weston_clock_timer_remove(clock, timer2);
	weston_clock_timer_remove(clock, timer3);
	weston_clock_timer_remove(clock, timer4);
	weston_clock_destroy(clock);
	wl_event_loop_destroy(loop);
}
