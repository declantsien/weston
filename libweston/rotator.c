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

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>

#include <libweston/libweston.h>
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "backend.h"
#include "libweston-internal.h"

static void
weston_rotator_change_aspect_ration(struct weston_output *output)
{
	/* it only applies to 90 degree transformations */
	if (1 & (output->prev_transform ^ output->transform))
		wl_signal_emit(&output->compositor->output_resized_signal,
			       output);
}

static void
notify_output_redraw(void *data)
{
       struct weston_output *output = data;
       struct output_accel *oac = output->oac;

       oac->output_redraw = NULL;
       weston_log("Doing a redraw on output %s\n", output->name);
       weston_output_damage(output);

       if (oac->done)
	       oac->done(oac->data);
}

static void
weston_schedule_output_repaint(struct weston_output *output)
{
       struct weston_compositor *wc = output->compositor;
       struct output_accel *oac = output->oac;
       struct wl_event_loop *loop;

       if (!oac || oac->output_redraw) {
	       weston_log("weston_schedule_output_repaint(): not OAC for output %s set\n",
			       output->name);
               return;
       }

       loop = wl_display_get_event_loop(wc->wl_display);
       oac->output_redraw = wl_event_loop_add_idle(loop,
                                                   notify_output_redraw,
                                                   output);
}


WL_EXPORT void
weston_rotator_rotate(struct weston_output *output, uint32_t transform,
		      weston_rotator_cb done, void *data)
{

	if (!output->oac) {
		/* we destroy any oacs at the end */
		output->oac = zalloc(sizeof(*output->oac));
	}

	if (done)
		output->oac->done = done;
	if (data)
		output->oac->data = data;

	output->prev_transform = output->transform;
	weston_output_set_transform(output, transform);

	/* Only swap width and height when the aspect ratio changed. */
	weston_rotator_change_aspect_ration(output);

	/* calls 'done' cb in case that was passed */
	weston_schedule_output_repaint(output);
}
