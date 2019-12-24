/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
 * Copyright © 2019 Advanced Driver Information Technology Joint Venture GmbH
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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <assert.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <libudev.h>

#include <libweston/libweston.h>
#include <libweston/weston-log.h>
#include "transmitter-internal.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "renderer-gl/gl-renderer.h"
#include "weston-egl-ext.h"
#include "pixman-renderer.h"
#include "pixel-formats.h"
#include "libinput-seat.h"
#include "launcher-util.h"

#include "presentation-time-server-protocol.h"
#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "linux-explicit-synchronization.h"
#include "renderer-waltham/waltham-renderer.h"

#define MAX_EPOLL_WATCHES 2
#define ESTABLISH_CONNECTION_PERIOD 2000
#define RETRY_CONNECTION_PERIOD 5000

static struct gl_renderer_interface *gl_renderer;
static const char default_seat[] = "seat0";
static const struct weston_transmitter_api transmitter_api_impl;

static struct drm_fb *
transmitter_output_render_gl(struct transmitter_output* output,
			     pixman_region32_t *damage)
{
	struct transmitter_backend *b = to_transmitter_backend(output->base.compositor);
	struct gbm_bo *bo;
	struct drm_fb *ret;

	output->base.compositor->renderer->repaint_output(&output->base,
							  damage);
	bo = gbm_surface_lock_front_buffer(output->gbm_surface);
	if (!bo) {
		weston_log("failed to lock front buffer: %s\n", strerror(errno));
		return NULL;
	}
	/* The renderer always produces an opaque image. */
	ret = drm_fb_get_from_bo(bo, b, true, BUFFER_GBM_SURFACE);
	if (!ret) {
		weston_log("failed to get drm_fb for bo\n");
		gbm_surface_release_buffer(output->gbm_surface, bo);
		return NULL;
	}
	ret->gbm_surface = output->gbm_surface;
	gbm_surface_release_buffer(output->gbm_surface, bo);
	return ret;
}

static void
transmitter_output_render(struct transmitter_output *output, pixman_region32_t *damage)
{
	struct transmitter_plane_state *scanout_state = output->scanout_plane->state_cur;
	struct weston_compositor *c = output->base.compositor;
	struct transmitter_plane *scanout_plane = output->scanout_plane;
	struct drm_fb *fb;

	if (!pixman_region32_not_empty(damage) && scanout_plane->state_cur->fb &&
	    (scanout_plane->state_cur->fb->type == BUFFER_GBM_SURFACE ||
	    scanout_plane->state_cur->fb->type == BUFFER_PIXMAN_DUMB) &&
	    scanout_plane->state_cur->fb->width == output->base.current_mode->width &&
	    scanout_plane->state_cur->fb->height == output->base.current_mode->height)
		fb = drm_fb_ref(scanout_plane->state_cur->fb);
	else
		fb = transmitter_output_render_gl(output, damage);

	if (!fb)
		weston_log("fb is NULL\n");

	scanout_state->fb = fb;
	scanout_state->output = output;
	scanout_state->src_x = 0;
	scanout_state->src_y = 0;
	scanout_state->src_w = output->base.current_mode->width << 16;
	scanout_state->src_h = output->base.current_mode->height << 16;
	scanout_state->dest_x = 0;
	scanout_state->dest_y = 0;
	scanout_state->dest_w = scanout_state->src_w >> 16;
	scanout_state->dest_h = scanout_state->src_h >> 16;
	pixman_region32_copy(&scanout_state->damage, damage);

	if (output->base.zoom.active) {
		weston_matrix_transform_region(&scanout_state->damage,
					       &output->base.matrix,
					       &scanout_state->damage);
	} else {
		pixman_region32_translate(&scanout_state->damage,
					  -output->base.x, -output->base.y);
		weston_transformed_region(output->base.width,
					  output->base.height,
					  output->base.transform,
					  output->base.current_scale,
					  &scanout_state->damage,
					  &scanout_state->damage);
	}

	pixman_region32_subtract(&c->primary_plane.damage,
				 &c->primary_plane.damage, damage);
}

int
get_frame_fd(struct weston_output *base,struct weston_view *ev, int *buf_stride,
        pixman_region32_t *damage)
{
	struct transmitter_output *output = to_transmitter_output(base);
	struct weston_compositor *c = output->base.compositor;
	struct transmitter_plane_state *scanout_state;
	struct transmitter_plane *scanout_plane = output->scanout_plane;
	struct transmitter_backend *b = to_transmitter_backend(c);
	scanout_state = scanout_plane->state_cur;
	int fd, ret;

	/* Drop frame if there isn't free buffers */
	if (!gbm_surface_has_free_buffers(output->gbm_surface)) {
		weston_log("%s: Drop frame!!\n", __func__);
		return -1;
	}
	transmitter_output_render(output,damage);

	if (!scanout_state || !scanout_state->fb) {
		weston_log("scanout_state is NULL \n");
		return -1;
	}
	*buf_stride = scanout_state->fb->strides[0];
	ret = drmPrimeHandleToFD(b->drm.fd, scanout_state->fb->handles[0],
				 DRM_CLOEXEC, &fd);
	if (ret<0) {
		weston_log("failed to create prime fd for front buffer\n");
		drm_fb_unref(scanout_state->fb);
		close(fd);
		return -1;
	}
	return fd;
}

static int
transmitter_check_remote_output(struct weston_transmitter_surface *txs,
				struct weston_compositor *compositor,
				struct weston_transmitter_remote *remote)
{
	struct weston_view *view;
	int transmitter_is_surface_removed = 1;

	wl_list_for_each_reverse(view, &compositor->view_list, link) {
		if (view->surface == txs->surface)
			transmitter_is_surface_removed = 0;
	}

	if (transmitter_is_surface_removed) {
		transmitter_api_impl.surface_destroy(txs);
		return 0;
	}
	return 1;
}

static int
transmitter_output_repaint(struct weston_output *output_base,
			   pixman_region32_t *damage, void *repaint_data)
{
	struct transmitter_output* output = to_transmitter_output(output_base);
	struct weston_transmitter_remote *remote = output->remote;
	struct weston_transmitter_surface *txs, *next;
	struct weston_compositor *compositor = output_base->compositor;
	struct weston_view *view;
	bool found_output = false;
	uint32_t ivi_id;

	if (wl_list_empty(&compositor->view_list))
		goto out;

	if (remote->status != WESTON_TRANSMITTER_CONNECTION_READY)
		goto out;

	wl_list_for_each_reverse(view, &compositor->view_list, link) {
		bool found_surface = false;
		if (view->output == &output->base && (view->surface->width >= 64
		    && view->surface->height >= 64)) {
			found_output = true;
			wl_list_for_each_safe(txs, next, &remote->surface_list, link) {
				if (!transmitter_check_remote_output(txs, compositor, remote)) {
					/* avoid calling push to remote for
					 * this repaint outside loop */
					found_surface = true;
					break;
				}
				if (txs->surface == view->surface) {
					found_surface = true;
					if (!remote->wthp_surf)
						transmitter_api_impl.surface_push_to_remote(
								view->surface,
								remote, NULL);
					output->renderer->dmafd = get_frame_fd(&output->base,
									view,
									&output->renderer->buf_stride,
									damage);
					if (output->renderer->dmafd < 0) {
						weston_log("Failed to get dmafd\n");
						goto out;
					}
					output->renderer->surface_width =
							view->surface->width;
					output->renderer->surface_height =
							view->surface->height;
					output->renderer->repaint_output(output);
					output->renderer->dmafd = 0;
					transmitter_api_impl.surface_gather_state(
							txs);
				}
			}
			if (!found_surface) {
				txs = transmitter_api_impl.surface_push_to_remote(
						view->surface, remote, NULL);
				if (remote->count == 0) {
					remote->wthp_surf =
						wthp_compositor_create_surface(remote->display->compositor);
					ivi_id = rand()%1000;
					remote->wthp_ivi_surface =
						wthp_ivi_application_surface_create(
						remote->display->application,
						ivi_id, remote->wthp_surf);
					wth_connection_flush(remote->display->connection);
					weston_log("surface ID %d\n", ivi_id);

					if (!(remote->wthp_surf) || !(remote->wthp_ivi_surface))
						weston_log("Failed to create txs->ivi_surf\n");
					else
						remote->count++;
				}
				output->renderer->dmafd = get_frame_fd(&output->base,
								view,
								&output->renderer->buf_stride,
								damage);
				if (output->renderer->dmafd < 0) {
					weston_log("Failed to get dmafd\n");
					goto out;
				}
				output->renderer->surface_width = view->surface->width;
				output->renderer->surface_height = view->surface->height;
				output->renderer->repaint_output(output);
				output->renderer->dmafd = 0;
				transmitter_api_impl.surface_gather_state(txs);
			}
		}
	}
	if (!found_output) {
		wl_list_for_each_safe(txs, next, &remote->surface_list, link) {
			transmitter_api_impl.surface_destroy(txs);
		}
		goto out;
	}

	wl_event_source_timer_update(output->finish_frame_timer, 1);
	return 0;

out:
	wl_event_source_timer_update(output->finish_frame_timer,1);
	return 0;
}

static int
transmitter_output_start_repaint_loop(struct weston_output *output_base)
{
	struct transmitter_output *output = to_transmitter_output(output_base);
	weston_output_finish_frame(&output->base,NULL,
				   WP_PRESENTATION_FEEDBACK_INVALID);
	return 0;
}

static struct gbm_device *
create_gbm_device(int fd)
{
	struct gbm_device *gbm;

	gl_renderer = weston_load_module("gl-renderer.so",
					 "gl_renderer_interface");
	if (!gl_renderer)
		return NULL;

	/* GBM will load a dri driver, but even though they need symbols from
	 * libglapi, in some version of Mesa they are not linked to it. Since
	 * only the gl-renderer module links to it, the call above won't make
	 * these symbols globally available, and loading the DRI driver fails.
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL.
	 */
	dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);
	gbm = gbm_create_device(fd);
	return gbm;
}

/* When initializing EGL, if the preferred buffer format isn't available
 * we may be able to substitute an ARGB format for an XRGB one.
 *
 * This returns 0 if substitution isn't possible, but 0 might be a
 * legitimate format for other EGL platforms, so the caller is
 * responsible for checking for 0 before calling gl_renderer->create().
 */
static int
fallback_format_for(uint32_t format)
{
	switch (format) {
	case GBM_FORMAT_XRGB8888:
		return GBM_FORMAT_ARGB8888;
	case GBM_FORMAT_XRGB2101010:
		return GBM_FORMAT_ARGB2101010;
	default:
		return 0;
	}
}

static int
transmitter_backend_create_gl_renderer(struct transmitter_backend *b)
{
	EGLint format[3] = {
		b->gbm_format,
		fallback_format_for(b->gbm_format),
		0,
	};
	int n_formats = 2;

	if (format[1])
		n_formats = 3;
	if (gl_renderer->display_create(b->compositor,
					EGL_PLATFORM_GBM_KHR,
					(void *)b->gbm,
					NULL,
					gl_renderer->opaque_attribs,
					format,
					n_formats) < 0) {
		return -1;
	}
	return 0;
}

static int
init_egl(struct transmitter_backend *b)
{
	b->gbm = create_gbm_device(b->drm.fd);

	if (!b->gbm)
		return -1;

	if (transmitter_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		return -1;
	}

	return 0;
}

static void
setup_output_seat_constraint(struct transmitter_backend *b,
			     struct weston_output *output,
			     const char *s)
{
	if (strcmp(s, "") != 0) {
		struct weston_pointer *pointer;
		struct udev_seat *seat;

		seat = udev_seat_get_named(&b->input, s);
		if (!seat)
			return;

		seat->base.output = output;

		pointer = weston_seat_get_pointer(&seat->base);
		if (pointer)
			weston_pointer_clamp(pointer,
					     &pointer->x,
					     &pointer->y);
	}
}

static int
transmitter_output_attach_head(struct weston_output *output_base,
			       struct weston_head *head_base)
{
	weston_log("%s is called \n",__func__);
	return 0;
}

static void
transmitter_output_detach_head(struct weston_output *output_base,
			       struct weston_head *head_base)
{
	weston_log("%s is called \n",__func__);
}

static int
parse_gbm_format(const char *s, uint32_t default_value, uint32_t *gbm_format)
{
	const struct pixel_format_info *pinfo;

	if (s == NULL) {
		*gbm_format = default_value;
		return 0;
	}

	pinfo = pixel_format_get_info_by_drm_name(s);
	if (!pinfo) {
		weston_log("fatal: unrecognized pixel format: %s\n", s);
		return -1;
	}

	/* GBM formats and DRM formats are identical. */
	*gbm_format = pinfo->format;

	return 0;
}

static void
transmitter_output_set_seat(struct weston_output *base,
			    const char *seat)
{
	struct transmitter_output *output = to_transmitter_output(base);
	struct transmitter_backend *b = to_transmitter_backend(base->compositor);

	setup_output_seat_constraint(b, &output->base, seat ? seat : "");
}

static struct weston_mode *
get_current_mode(struct wl_list *mode_list)
{
	struct weston_mode *mode;

	wl_list_for_each(mode, mode_list, link)
		if (mode->flags & WL_OUTPUT_MODE_CURRENT)
			return mode;

	assert(0);
	return NULL;
}

static int
make_mode_list(struct wl_list *list,
	       const struct weston_transmitter_output_info info)
{
	struct weston_mode *mode;

	mode = zalloc(sizeof *mode);
	if (!mode)
		return -1;

	*mode = info.mode;
	wl_list_insert(list->prev, &mode->link);

	return 0;
}

static int
transmitter_output_set_size(struct weston_output *base,
			    int width, int height)
{
	struct transmitter_output *output = to_transmitter_output(base);
	/* We can only be called once. */
	assert(!output->base.current_mode);

	output->info.subpixel = WL_OUTPUT_SUBPIXEL_NONE;
	output->info.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	output->info.scale = 1;
	output->info.x = 0;
	output->info.y = 0;
	output->info.width_mm = 1024;
	output->info.height_mm = 768;
	output->info.mode.flags = WL_OUTPUT_MODE_CURRENT
			| WL_OUTPUT_MODE_PREFERRED;
	output->info.mode.width = 1024;
	output->info.mode.height = 768;
	output->info.mode.refresh = 60;
	output->info.mode.link.next = output->info.mode.link.prev = NULL;

	if (width != 0) {
		if (height != 0) {
			output->info.mode.width = width;
			output->info.mode.height = height;
			output->info.mode.refresh = 60;
		}
	}
	weston_log("width=%d height=%d\n",output->info.mode.width,
		   output->info.mode.height);
	wl_list_init(&output->base.mode_list);
	if (make_mode_list(&output->base.mode_list, output->info) < 0) {
		weston_log("NOT ABLE TO INSERT MODE in wl_list\n");
		return -1;
	}

	output->base.current_mode = get_current_mode(&output->base.mode_list);
	output->base.height = output->base.current_mode->height;
	output->base.width = output->base.current_mode->width;
	output->base.native_mode = output->base.current_mode;
	output->base.native_scale = output->base.current_scale;
	output->base.scale = 1;
	output->base.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	return 0;
}

static int
transmitter_output_finish_frame_handler(void *data)
{
	struct transmitter_output *output = data;
	struct timespec now;
	weston_compositor_read_presentation_clock(output->base.compositor, &now);
	weston_output_finish_frame(&output->base, &now, 0);
	return 0;
}

void transmitter_assign_planes(struct weston_output *output_base,
			       void *repaint_data)
{
	/* This function prevents compositor releasing buffer early. */
	struct transmitter_output *output = to_transmitter_output(output_base);
	struct weston_compositor *compositor = output_base->compositor;
	struct weston_view *view;
	struct weston_plane *primary = &output_base->compositor->primary_plane;

	wl_list_for_each_reverse(view,&compositor->view_list,link) {
		if (view->output == &output->base &&
		    (view->surface->width >= 64 && view->surface->height >= 64)) {
			weston_view_move_to_plane(view, primary);
			view->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
		}
	}
}

/**
 * Allocate a new, empty, plane state.
 */
struct transmitter_plane_state *
transmitter_plane_state_alloc(struct transmitter_output_state *state_output,
		      struct transmitter_plane *plane)
{
	struct transmitter_plane_state *state = zalloc(sizeof(*state));

	assert(state);
	state->output_state = state_output;
	state->plane = plane;
	state->in_fence_fd = -1;
	pixman_region32_init(&state->damage);

	/* Here we only add the plane state to the desired link, and not
	 * set the member. Having an output pointer set means that the
	 * plane will be displayed on the output; this won't be the case
	 * when we go to disable a plane. In this case, it must be part of
	 * the commit (and thus the output state), but the member must be
	 * NULL, as it will not be on any output when the state takes
	 * effect.
	 */
	if (state_output)
		wl_list_insert(&state_output->plane_list, &state->link);
	else
		wl_list_init(&state->link);

	return state;
}

static struct transmitter_plane *
transmitter_plane_create(struct transmitter_backend *b, struct transmitter_output *output)
{
	struct transmitter_plane *plane;

	/* num of formats is one */
	plane = zalloc(sizeof(*plane) + sizeof(plane->formats[0]));
	if (!plane) {
		weston_log("%s: out of memory\n", __func__);
		return NULL;
	}

	plane->type = WDRM_PLANE_TYPE_PRIMARY;
	plane->backend = b;
	plane->state_cur = transmitter_plane_state_alloc(NULL, plane);
	plane->state_cur->complete = true;
	plane->formats[0].format = output->gbm_format;
	plane->count_formats = 1;
	if ((output->gbm_bo_flags & GBM_BO_USE_LINEAR) && b->fb_modifiers) {
		uint64_t *modifiers = zalloc(sizeof *modifiers);
		if (modifiers) {
			*modifiers = DRM_FORMAT_MOD_LINEAR;
			plane->formats[0].modifiers = modifiers;
			plane->formats[0].count_modifiers = 1;
		}
	}

	weston_plane_init(&plane->base, b->compositor, 0, 0);
	wl_list_insert(&b->plane_list, &plane->link);

	return plane;
}

/* Init output state that depends on gl or gbm */
static int
transmitter_output_init_egl(struct transmitter_output *output, struct transmitter_backend *b)
{
	EGLint format[2] = {
		output->gbm_format,
		fallback_format_for(output->gbm_format),
	};
	int n_formats = 1;
	struct weston_mode *mode = output->base.current_mode;
	struct transmitter_plane *plane = output->scanout_plane;
	unsigned int i;

	assert(output->gbm_surface == NULL);

	for (i = 0; i < plane->count_formats; i++) {
		if (plane->formats[i].format == output->gbm_format)
			break;
	}

	if (i == plane->count_formats) {
		weston_log("format 0x%x not supported by output %s\n",
			   output->gbm_format, output->base.name);
		return -1;
	}

#ifdef HAVE_GBM_MODIFIERS
	if (plane->formats[i].count_modifiers > 0) {
	weston_log("Created gbm_surface with modifiers\n");
		output->gbm_surface =
			gbm_surface_create_with_modifiers(b->gbm,
							  mode->width,
							  mode->height,
							  output->gbm_format,
							  plane->formats[i].modifiers,
							  plane->formats[i].count_modifiers);
	}

	/* If allocating with modifiers fails, try again without. This can
	 * happen when the KMS display device supports modifiers but the
	 * GBM driver does not, e.g. the old i915 Mesa driver. */
	if (!output->gbm_surface)
#endif
	{
	weston_log("Created gbm_surface without modifiers\n");
		output->gbm_surface =
		    gbm_surface_create(b->gbm, mode->width, mode->height,
				       output->gbm_format,
				       output->gbm_bo_flags);
	}

	if (!output->gbm_surface) {
		weston_log("failed to create gbm surface\n");
		return -1;
	}

	if (format[1])
		n_formats = 2;
	if (gl_renderer->output_window_create(&output->base,
					      (EGLNativeWindowType)output->gbm_surface,
					      output->gbm_surface,
					      gl_renderer->opaque_attribs,
					      format,
					      n_formats) < 0) {
		weston_log("failed to create gl renderer output state\n");
		gbm_surface_destroy(output->gbm_surface);
		output->gbm_surface = NULL;
		return -1;
	}
	return 0;
}

static int
transmitter_output_enable(struct weston_output *base)
{
	struct transmitter_output *output = to_transmitter_output(base);
	struct transmitter_backend *b = to_transmitter_backend(base->compositor);
	struct wl_event_loop *loop;
	output->base.start_repaint_loop = transmitter_output_start_repaint_loop;
	output->base.repaint = transmitter_output_repaint;
	output->base.assign_planes = transmitter_assign_planes;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;
	output->base.set_gamma = NULL;
	output->base.set_backlight = NULL;
	output->base.gamma_size = 0;
	output->scanout_plane = transmitter_plane_create(b, output);
	if (!output->scanout_plane) {
		weston_log("Failed to find primary plane for output %s\n",
			   output->base.name);
		return -1;
	}
	if (transmitter_output_init_egl(output, b) < 0) {
		weston_log("Failed to init output gl state\n");
		return -1;
	}
	loop = wl_display_get_event_loop(base->compositor->wl_display);
	output->finish_frame_timer = wl_event_loop_add_timer(loop,
			transmitter_output_finish_frame_handler, output);
	weston_compositor_stack_plane(b->compositor,
				      &output->scanout_plane->base,
				      &b->compositor->primary_plane);
	return 0;
}

static void
free_mode_list(struct wl_list *mode_list)
{
	struct weston_mode *mode;

	while (!wl_list_empty(mode_list)) {
		mode = wl_container_of(mode_list->next, mode, link);
		wl_list_remove(&mode->link);
		free(mode);
	}
}

static void
transmitter_output_destroy(struct weston_output *base)
{
	struct transmitter_output *output = to_transmitter_output(base);

	if (output->remote->wthp_surf && output->remote->wthp_ivi_surface) {
		weston_log("Destroying wthp_surface and wthp_ivi_surface\n");
		wthp_surface_destroy(output->remote->wthp_surf);
		wthp_ivi_surface_destroy(output->remote->wthp_ivi_surface);
	}
	wl_list_remove(&output->link);
	free_mode_list(&output->base.mode_list);
	wl_event_source_remove(output->finish_frame_timer);
	weston_output_release(&output->base);
	free(output);
}

static void
transmitter_head_destroy(struct transmitter_head *head)
{
	weston_head_release(&head->base);
	free(head);
}

/**
 * Create a Weston output structure
 *
 * Create an "empty" transmitter_output. This is the implementation of
 * weston_backend::create_output.
 *
 * Creating an output is usually followed by transmitter_output_attach_head()
 * and transmitter_output_enable() to make use of it.
 *
 * @param compositor The compositor instance.
 * @param name Name for the new output.
 * @returns The output, or NULL on failure.
 */
static struct weston_output *
transmitter_output_create(struct weston_compositor *compositor,
			  const char *name)
{
	struct transmitter_backend *b = to_transmitter_backend(compositor);
	struct weston_transmitter *txr = b->txr;
	struct transmitter_output *output;
	struct weston_transmitter_remote *remote, *r = NULL;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;
	wl_list_for_each_reverse(remote, &txr->remote_list, link) {
		if (strstr(name,remote->port))
			 r = remote;
	}
	output->backend = b;
	output->parent.draw_initial_frame = true;
	weston_output_init(&output->base, compositor, name);
	output->base.enable = transmitter_output_enable;
	output->base.destroy = transmitter_output_destroy;
	output->base.disable = NULL;
	output->base.attach_head = transmitter_output_attach_head;
	output->base.detach_head = transmitter_output_detach_head;
	output->remote = r;
	output->gbm_bo_flags = GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING;
	output->gbm_format = b->gbm_format;
	wl_list_init(&output->link);
	wl_list_insert(&r->output_list, &output->link);
	if (txr->waltham_renderer->display_create(output) < 0) {
		weston_log("Failed to create waltham renderer display \n");
		return NULL;
	}
	return &output->base;
}

static int transmitter_head_create(struct weston_compositor *compositor,
				   const char *name)
{
	assert(name);
	struct transmitter_head *head;
	const char *make = strdup(WESTON_TRANSMITTER_OUTPUT_MAKE);
	const char *model = name;
	const char *serial_number = strdup("0");
	head = zalloc(sizeof *head);
	if (!head) {
		weston_log("allocation failed for head\n");
		return -1;
	}
	weston_head_init(&head->base, name);
	weston_head_set_subpixel(&head->base, WL_OUTPUT_SUBPIXEL_NONE);
	weston_head_set_monitor_strings(&head->base, make, model,
					serial_number);
	head->base.connected = true;
	weston_compositor_add_head(compositor, &head->base);
	head->base.compositor = compositor;
	return 0;
}

static void
transmitter_destroy(struct weston_compositor *ec)
{
	struct transmitter_backend *b = to_transmitter_backend(ec);
	struct weston_head *base, *next;

	udev_input_destroy(&b->input);
	weston_compositor_shutdown(ec);

	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link)
		transmitter_head_destroy(to_transmitter_head(base));

	if (b->gbm)
		gbm_device_destroy(b->gbm);

	udev_monitor_unref(b->udev_monitor);
	udev_unref(b->udev);

	weston_launcher_destroy(ec->launcher);

	close(b->drm.fd);
	free(b->drm.filename);
	free(b);
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct weston_compositor *compositor = data;
	struct transmitter_backend *b = to_transmitter_backend(compositor);

	if (compositor->session_active) {
		weston_log("activating session\n");
		weston_compositor_wake(compositor);
		weston_compositor_damage_all(compositor);
		b->state_invalid = true;
		udev_input_enable(&b->input);
	} else {
		weston_log("deactivating session\n");
		udev_input_disable(&b->input);
		weston_compositor_offscreen(compositor);
	}
}

/**
 * Determines whether or not a device is capable of modesetting. If successful,
 * sets b->drm.fd and b->drm.filename to the opened device.
 */
static bool drm_device_is_kms(struct transmitter_backend *b,
			      struct udev_device *device)
{
	const char *filename = udev_device_get_devnode(device);
	const char *sysnum = udev_device_get_sysnum(device);
	dev_t devnum = udev_device_get_devnum(device);
	drmModeRes *res;
	int id = -1, fd;

	if (!filename)
		return false;

	fd = weston_launcher_open(b->compositor->launcher, filename, O_RDWR);
	if (fd < 0)
		return false;

	res = drmModeGetResources(fd);
	if (!res)
		goto out_fd;

	if (res->count_crtcs <= 0 || res->count_connectors <= 0 ||
	    res->count_encoders <= 0)
		goto out_res;

	if (sysnum)
		id = atoi(sysnum);
	if (!sysnum || id < 0) {
		weston_log("couldn't get sysnum for device %s\n", filename);
		goto out_res;
	}

	/* We can be called successfully on multiple devices; if we have,
	 * clean up old entries. */
	if (b->drm.fd >= 0)
		weston_launcher_close(b->compositor->launcher, b->drm.fd);
	free(b->drm.filename);

	b->drm.fd = fd;
	b->drm.id = id;
	b->drm.filename = strdup(filename);
	b->drm.devnum = devnum;

	drmModeFreeResources(res);

	return true;

out_res:
	drmModeFreeResources(res);
out_fd:
	weston_launcher_close(b->compositor->launcher, fd);
	return false;
}

/* Find primary GPU
 * Some systems may have multiple DRM devices attached to a single seat. This
 * function loops over all devices and tries to find a PCI device with the
 * boot_vga sysfs attribute set to 1.
 * If no such device is found, the first DRM device reported by udev is used.
 * Devices are also vetted to make sure they are are capable of modesetting,
 * rather than pure render nodes (GPU with no display), or pure
 * memory-allocation devices (VGEM).
 */
static struct udev_device*
find_primary_gpu(struct transmitter_backend *b, const char *seat)
{
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	const char *path, *device_seat, *id;
	struct udev_device *device, *drm_device, *pci;

	e = udev_enumerate_new(b->udev);
	udev_enumerate_add_match_subsystem(e, "drm");
	udev_enumerate_add_match_sysname(e, "card[0-9]*");

	udev_enumerate_scan_devices(e);
	drm_device = NULL;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		bool is_boot_vga = false;

		path = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(b->udev, path);
		if (!device)
			continue;
		device_seat = udev_device_get_property_value(device, "ID_SEAT");
		if (!device_seat)
			device_seat = default_seat;
		if (strcmp(device_seat, seat)) {
			udev_device_unref(device);
			continue;
		}

		pci = udev_device_get_parent_with_subsystem_devtype(device,
								    "pci", NULL);
		if (pci) {
			id = udev_device_get_sysattr_value(pci, "boot_vga");
			if (id && !strcmp(id, "1"))
				is_boot_vga = true;
		}

		/* If we already have a modesetting-capable device, and this
		 * device isn't our boot-VGA device, we aren't going to use
		 * it. */
		if (!is_boot_vga && drm_device) {
			udev_device_unref(device);
			continue;
		}

		/* Make sure this device is actually capable of modesetting;
		 * if this call succeeds, b->drm.{fd,filename} will be set,
		 * and any old values freed. */
		if (!drm_device_is_kms(b, device)) {
			udev_device_unref(device);
			continue;
		}

		/* There can only be one boot_vga device, and we try to use it
		 * at all costs. */
		if (is_boot_vga) {
			if (drm_device)
				udev_device_unref(drm_device);
			drm_device = device;
			break;
		}

		/* Per the (!is_boot_vga && drm_device) test above, we only
		 * trump existing saved devices with boot-VGA devices, so if
		 * we end up here, this must be the first device we've seen.
		 */
		assert(!drm_device);
		drm_device = device;
	}

	/* If we're returning a device to use, we must have an open FD for it.*/
	assert(!!drm_device == (b->drm.fd >= 0));

	udev_enumerate_unref(e);
	return drm_device;
}

static struct udev_device *
open_specific_drm_device(struct transmitter_backend *b, const char *name)
{
	struct udev_device *device;

	device = udev_device_new_from_subsystem_sysname(b->udev, "drm", name);
	if (!device) {
		weston_log("ERROR: could not open DRM device '%s'\n", name);
		return NULL;
	}

	if (!drm_device_is_kms(b, device)) {
		udev_device_unref(device);
		weston_log("ERROR: DRM device '%s' is not a KMS device.\n",
			   name);
		return NULL;
	}

	/* If we're returning a device to use, we must have an open FD for it. */
	assert(b->drm.fd >= 0);

	return device;
}

static void
conn_ready_notify(struct wl_listener *l, void *data)
{
	struct weston_transmitter_remote *remote =
	  wl_container_of(l, remote, establish_listener);
	transmitter_remote_create_seat(remote);
}

static int
transmitter_create_remote(char *model, char *addr, char *port, int *width,
			  int *height, struct weston_compositor *c)
{
	struct transmitter_backend *b = to_transmitter_backend(c);
	struct weston_transmitter *txr = b->txr;
	struct weston_transmitter_remote *remote;

	remote = zalloc(sizeof (*remote));
	if (!remote)
		return -1;
	remote->transmitter = txr;
	wl_list_insert(&txr->remote_list, &remote->link);
	remote->model = strdup(model);
	remote->addr = strdup(addr);
	remote->port = strdup(port);
	remote->width = width;
	remote->height = height;
	remote->status = WESTON_TRANSMITTER_CONNECTION_INITIALIZING;
	wl_signal_init(&remote->connection_status_signal);
	wl_list_init(&remote->output_list);
	wl_list_init(&remote->surface_list);
	wl_list_init(&remote->seat_list);
	wl_signal_init(&remote->conn_establish_signal);
	remote->establish_listener.notify = conn_ready_notify;
	wl_signal_add(&remote->conn_establish_signal, &remote->establish_listener);
	return 0;
}

static const struct weston_transmitter_output_api api = {
	transmitter_output_set_seat,
	transmitter_output_set_size,
	transmitter_head_create,
	transmitter_create_remote,
};

/* Send configure event through ivi-shell.
 *
 * \param txs The Transmitter surface.
 * \param width Suggestion for surface width.
 * \param height Suggestion for surface height.
 *
 * When the networking code receives a ivi_surface.configure event, it calls
 * this function to relay it to the application.
 *
 * \c txs cannot be a zombie, because transmitter_surface_zombify() must
 * tear down the network link, so a zombie cannot receive events.
 */
void
transmitter_surface_ivi_resize(struct weston_transmitter_surface *txs,
			       int32_t width, int32_t height)
{
	assert(txs->resize_handler);
	if (!txs->resize_handler)
		return;

	assert(txs->surface);
	if (!txs->surface)
		return;

	txs->resize_handler(txs->resize_handler_data, width, height);
}

static void
transmitter_surface_configure(struct weston_transmitter_surface *txs,
			      int32_t dx, int32_t dy)
{
	assert(txs->surface);
	if (!txs->surface)
		return;

	txs->attach_dx += dx;
	txs->attach_dy += dy;
}

static void
buffer_send_complete(struct wthp_buffer *b, uint32_t serial)
{
	if (b)
		wthp_buffer_destroy(b);
}

static const struct wthp_buffer_listener buffer_listener = {
	buffer_send_complete
};

static void
transmitter_surface_gather_state(struct weston_transmitter_surface *txs)
{
	struct weston_transmitter_remote *remote = txs->remote;
	struct waltham_display *dpy = remote->display;

	if (!dpy->running) {
		if (remote->status != WESTON_TRANSMITTER_CONNECTION_DISCONNECTED) {
			remote->status =
				WESTON_TRANSMITTER_CONNECTION_DISCONNECTED;
			wth_connection_destroy(remote->display->connection);
			wl_event_source_remove(remote->source);
			wl_event_source_timer_update(remote->retry_timer, 1);
		}
	} else {
		/* TODO: transmit surface state to remote */
		/* The buffer must be transmitted to remote side */

		/* waltham */
		struct weston_surface *surf = txs->surface;
		struct weston_compositor *comp = surf->compositor;
		int32_t stride, data_sz, width, height;
		void *data;

		width = 1;
		height = 1;
		stride = width * (PIXMAN_FORMAT_BPP(comp->read_format) / 8);

		data = malloc(stride * height);
		data_sz = stride * height;

		/* fake sending buffer */
		txs->wthp_buf = wthp_blob_factory_create_buffer(remote->display->blob_factory,
								data_sz,
								data,
								surf->width,
								surf->height,
								stride,
								PIXMAN_FORMAT_BPP(
								comp->read_format));

		wthp_buffer_set_listener(txs->wthp_buf, &buffer_listener, txs);

		wthp_surface_attach(remote->wthp_surf, txs->wthp_buf,
				    txs->attach_dx, txs->attach_dy);
		wthp_surface_damage(remote->wthp_surf, txs->attach_dx,
				    txs->attach_dy, surf->width,
				    surf->height);
		wthp_surface_commit(remote->wthp_surf);

		wth_connection_flush(remote->display->connection);
		free(data);
		data = NULL;
		txs->attach_dx = 0;
		txs->attach_dy = 0;
	}
}

/** 
 * Mark the weston_transmitter_surface dead.
 * Stop all remoting actions on this surface.
 * Still keeps the pointer stored by a shell valid, so it can be freed later.
 */
static void
transmitter_surface_zombify(struct weston_transmitter_surface *txs)
{
	struct weston_transmitter_remote *remote;
	/* may be called multiple times */
	if (!txs->surface)
		return;

	wl_signal_emit(&txs->destroy_signal, txs);

	wl_list_remove(&txs->surface_destroy_listener.link);
	txs->surface = NULL;

	remote = txs->remote;
	if (!remote->display->compositor)
		weston_log("remote->compositor is NULL\n");
	/* In case called from destroy_transmitter() */
	txs->remote = NULL;
}

static void
transmitter_surface_destroy(struct weston_transmitter_surface *txs)
{
	transmitter_surface_zombify(txs);
	wl_list_remove(&txs->link);
	free(txs);
}

/** weston_surface destroy signal handler */
static void
transmitter_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct weston_transmitter_surface *txs =
		wl_container_of(listener, txs, surface_destroy_listener);

	assert(data == txs->surface);

	transmitter_surface_zombify(txs);
	wl_list_remove(&txs->link);
	free(txs);
}

static struct weston_transmitter_surface *
transmitter_surface_push_to_remote(struct weston_surface *ws,
				   struct weston_transmitter_remote *remote,
				   struct wl_listener *stream_status)
{
	struct weston_transmitter *txr = remote->transmitter;
	struct weston_transmitter_surface *txs;
	bool found = false;

	if (remote->status != WESTON_TRANSMITTER_CONNECTION_READY)
		return NULL;

	wl_list_for_each(txs, &remote->surface_list, link) {
		if (txs->surface == ws) {
			found = true;
			break;
		}
	}

	if (!found) {
		txs = NULL;
		txs = zalloc(sizeof(*txs));
		if (!txs)
			return NULL;

		txs->remote = remote;
		wl_signal_init(&txs->destroy_signal);
		wl_list_insert(&remote->surface_list, &txs->link);

		txs->status = WESTON_TRANSMITTER_STREAM_INITIALIZING;
		wl_signal_init(&txs->stream_status_signal);
		if (stream_status)
			wl_signal_add(&txs->stream_status_signal, stream_status);

		txs->surface = ws;
		txs->surface_destroy_listener.notify =
				transmitter_surface_destroyed;
		wl_signal_add(&ws->destroy_signal,
			      &txs->surface_destroy_listener);

		wl_list_init(&txs->frame_callback_list);
		wl_list_init(&txs->feedback_list);

		txs->lyt = weston_plugin_api_get(txr->compositor,
						 IVI_LAYOUT_API_NAME,
						 sizeof(txs->lyt));
	}

	/* TODO: create the content stream connection... */
	if (!remote->display->compositor)
		weston_log("remote->compositor is NULL\n");
	return txs;
}

static enum weston_transmitter_stream_status
transmitter_surface_get_stream_status(struct weston_transmitter_surface *txs)
{
	return txs->status;
}

/**
 * waltham's server advertises a global interface.
 * We can store the ad for later and/or bind to it immediately if we want to.
 * We also need to keep track of the globals we bind to, so that
 * global_remove can be handled properly (not implemented).
 */
static void
registry_handle_global(struct wthp_registry *registry,
		       uint32_t name,
		       const char *interface,
		       uint32_t version)
{
	struct waltham_display *dpy = wth_object_get_user_data(
				      (struct wth_object *) registry);

	if (strcmp(interface, "wthp_compositor") == 0) {
		assert(!dpy->compositor);
		dpy->compositor = (struct wthp_compositor *) wthp_registry_bind(
				  registry, name, interface, 1);
		/* has no events to handle */
	} else if (strcmp(interface, "wthp_blob_factory") == 0) {
		assert(!dpy->blob_factory);
		dpy->blob_factory =
				(struct wthp_blob_factory *) wthp_registry_bind(
				registry, name, interface, 1);
		/* has no events to handle */
	} else if (strcmp(interface, "wthp_seat") == 0) {
		assert(!dpy->seat);
		dpy->seat = (struct wthp_seat *) wthp_registry_bind(registry,
			    name, interface, 1);
		wthp_seat_set_listener(dpy->seat, &seat_listener, dpy);
	}else if (strcmp(interface, "wthp_ivi_application") == 0) {
	        assert(!dpy->application);
		dpy->application = (struct wthp_ivi_application *)
				   wthp_registry_bind(registry, name,
						      interface, 1);
	}
}

/**
 * waltham's server removed a global.
 * We should destroy everything we created through that global,
 * and destroy the objects we created by binding to it.
 * The identification happens by global's name, so we need to keep
 * track what names we bound.
 * (not implemented)
 */
static void
registry_handle_global_remove(struct wthp_registry *wthp_registry,
			      uint32_t name)
{
	if (wthp_registry)
		wthp_registry_free(wthp_registry);
}

static const struct wthp_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
connection_handle_data(struct watch *w, uint32_t events)
{
	struct waltham_display *dpy = wl_container_of(w, dpy, conn_watch);
	struct weston_transmitter_remote *remote = dpy->remote;
	int ret;

	if (!dpy->running) {
		weston_log("This server is not running yet. %s:%s\n",
			   remote->addr, remote->port);
		return;
	}

	if (events & EPOLLERR) {
		weston_log("Connection errored out.\n");
		dpy->running = false;
		remote->status = WESTON_TRANSMITTER_CONNECTION_INITIALIZING;
		return;
	}

	if (events & EPOLLOUT) {
		/* Flush out again. If the flush completes, stop
		 * polling for writable as everything has been written.
		 */
		ret = wth_connection_flush(dpy->connection);
	}

	if (events & EPOLLIN) {
		/* Do not ignore EPROTO */
		ret = wth_connection_read(dpy->connection);

		if (ret < 0) {
			weston_log("Connection read error %s:%s\n",
				   remote->addr, remote->port);
			perror("Connection read error\n");
			dpy->running = false;
			remote->status =
				WESTON_TRANSMITTER_CONNECTION_INITIALIZING;
			perror("EPOLL_CTL_DEL\n");

			return;
		}
	}

	if (events & EPOLLHUP) {
		weston_log("Connection hung up.\n");
		dpy->running = false;
		remote->status = WESTON_TRANSMITTER_CONNECTION_INITIALIZING;

		return;
	}
}

static int
waltham_mainloop(int fd, uint32_t mask, void *data)
{
	struct weston_transmitter_remote *remote = data;
	struct watch *w;
	int ret;
	int running_display;
	running_display = 0;

	struct waltham_display *dpy = remote->display;
	w = &dpy->conn_watch;
	if (!dpy)
		goto not_running;

	if (!dpy->connection)
		dpy->running = false;

	if (!dpy->running)
		goto not_running;

	running_display++;
	/* Dispatch queued events. */
	ret = wth_connection_dispatch(dpy->connection);
	if (ret < 0) {
		dpy->running = false;
		remote->status = WESTON_TRANSMITTER_CONNECTION_INITIALIZING;
	}
	if (!dpy->running)
		goto not_running;

	/* Run any application idle tasks at this point. */
	/* (nothing to run so far) */

	/* Flush out buffered requests. If the Waltham socket is
	 * full, poll it for writable too, and continue flushing then.
	 */
	ret = wth_connection_flush(dpy->connection);

	if (0 < running_display) {
		/* Waltham events only read in the callback, not dispatched,
		 * if the Waltham socket signalled readable. If it signalled
		 * writable, flush more. See connection_handle_data().
		 */
		w->cb(w, mask);
	}
	return 0;
not_running:
	return -1;
}

static int
waltham_client_init(struct waltham_display *dpy)
{
	if (!dpy)
		return -1;
	/* addr and port are set in weston.ini */
	dpy->connection = wth_connect_to_server(dpy->remote->addr,
						dpy->remote->port);
	if (!dpy->connection) {
		return -2;
	}
	else {
		dpy->remote->status = WESTON_TRANSMITTER_CONNECTION_READY;
		wl_signal_emit(&dpy->remote->connection_status_signal,
			       dpy->remote);
	}

	dpy->conn_watch.display = dpy;
	dpy->conn_watch.cb = connection_handle_data;
	dpy->conn_watch.fd = wth_connection_get_fd(dpy->connection);
	dpy->remote->source = wl_event_loop_add_fd(dpy->remote->transmitter->loop,
						   dpy->conn_watch.fd,
						   WL_EVENT_READABLE,
						   waltham_mainloop,
						   dpy->remote);

	dpy->display = wth_connection_get_display(dpy->connection);
	/* wth_display_set_listener() is already done by waltham, as
	 * all the events are just control messaging.
	 */

	/* Create a registry so that we will get advertisements of the
	 * interfaces implemented by the server.
	 */
	dpy->registry = wth_display_get_registry(dpy->display);
	wthp_registry_set_listener(dpy->registry, &registry_listener, dpy);

	/* Roundtrip ensures all globals' ads have been received. */
	if (wth_connection_roundtrip(dpy->connection) < 0) {
		weston_log("Roundtrip failed.\n");
		return -1;
	}

	if (!dpy->compositor) {
		weston_log("Did not find wthp_compositor, quitting.\n");
		return -1;
	}

	dpy->running = true;

	return 0;
}

static int
establish_timer_handler(void *data)
{
	struct weston_transmitter_remote *remote = data;
	int ret;

	ret = waltham_client_init(remote->display);
	if (ret == -2) {
		wl_event_source_timer_update(remote->establish_timer,
					     ESTABLISH_CONNECTION_PERIOD);
		return 0;
	}
	remote->status = WESTON_TRANSMITTER_CONNECTION_READY;
	wl_signal_emit(&remote->connection_status_signal, remote);
	return 0;
}

static void
init_globals(struct waltham_display *dpy)
{
	dpy->compositor = NULL;
	dpy->blob_factory = NULL;
	dpy->seat = NULL;
	dpy->application = NULL;
	dpy->pointer = NULL;
	dpy->keyboard = NULL;
	dpy->touch = NULL;
}

static void
disconnect_surface(struct weston_transmitter_remote *remote)
{
	struct weston_transmitter_surface *txs;
	wl_list_for_each(txs, &remote->surface_list, link) {
		free(remote->wthp_surf);
		remote->wthp_surf = NULL;
		free(remote->wthp_ivi_surface);
		remote->wthp_ivi_surface = NULL;
	}
}

static int
retry_timer_handler(void *data)
{
	struct weston_transmitter_remote *remote = data;
	struct waltham_display *dpy = remote->display;

	if (!dpy->running)
	{
		registry_handle_global_remove(dpy->registry, 1);
		init_globals(dpy);
		disconnect_surface(remote);
		wl_event_source_timer_update(remote->establish_timer,
					     ESTABLISH_CONNECTION_PERIOD);

		return 0;
	}
	else
		wl_event_source_timer_update(remote->retry_timer,
					     RETRY_CONNECTION_PERIOD);
	return 0;
}

static struct weston_transmitter_remote *
transmitter_connect_to_remote(struct weston_transmitter *txr)
{
	struct weston_transmitter_remote *remote;
	struct wl_event_loop *loop_est, *loop_retry;

	wl_list_for_each_reverse(remote, &txr->remote_list, link) {
		/* actually start connecting */
		/* waltham */
		remote->display = zalloc(sizeof (*(remote->display)));
		if (!remote->display) {
			weston_log("Fatal: Transmitter waltham connecting failed.\n");
			return NULL;
		}
		remote->display->remote = remote;
		/* set connection establish timer */
		loop_est = wl_display_get_event_loop(
				txr->compositor->wl_display);
		remote->establish_timer =
			wl_event_loop_add_timer(loop_est,
						establish_timer_handler,
						remote);
		wl_event_source_timer_update(remote->establish_timer, 1);
		/* set connection retry timer */
		loop_retry = wl_display_get_event_loop(
				txr->compositor->wl_display);
		remote->retry_timer =
			wl_event_loop_add_timer(loop_retry,
						retry_timer_handler,
						remote);
		wl_signal_emit(&remote->conn_establish_signal, NULL);
	}
	return remote;
}

static enum weston_transmitter_connection_status
transmitter_remote_get_status(struct weston_transmitter_remote *remote)
{
	return remote->status;
}

static void
transmitter_remote_destroy(struct weston_transmitter_remote *remote)
{
	struct weston_transmitter_surface *txs;
	struct transmitter_output *output, *otmp;
	struct weston_transmitter_seat *seat, *stmp;

	/* Do not emit connection_status_signal. */

	/**
	 *  Must not touch remote->transmitter as it may be stale:
	 * the desctruction order between the shell and Transmitter is
	 * undefined.
	 */

	if (!wl_list_empty(&remote->surface_list))
		weston_log("Transmitter warning: surfaces remain in %s.\n",
			   __func__);
	wl_list_for_each(txs, &remote->surface_list, link)
		txs->remote = NULL;
	wl_list_remove(&remote->surface_list);

	wl_list_for_each_safe(seat, stmp, &remote->seat_list, link)
		transmitter_seat_destroy(seat);

	wl_list_for_each_safe(output, otmp, &remote->output_list, link)
		transmitter_output_destroy(&output->base);

	free(remote->addr);
	wl_list_remove(&remote->link);
	if (remote->source)
		wl_event_source_remove(remote->source);

	free(remote);
}

/* Transmitter is destroyed on compositor shutdown. */
static void
transmitter_compositor_destroyed(struct wl_listener *listener, void *data)
{
	struct weston_transmitter_remote *remote, *temp;
	struct weston_transmitter_surface *txs;
	struct weston_transmitter *txr =
		wl_container_of(listener, txr, compositor_destroy_listener);

	assert(data == txr->compositor);

	/* may be called before or after shell cleans up */
	wl_list_for_each_safe(remote, temp,  &txr->remote_list, link) {
		wl_list_for_each(txs, &remote->surface_list, link) {
			transmitter_api_impl.surface_destroy(txs);
		}
		transmitter_api_impl.remote_destroy(remote);
	}

	/*
	 * Remove the head in case the list is not empty, to avoid
	 * transmitter_remote_destroy() accessing freed memory if the shell
	 * cleans up after Transmitter.
	 */
	wl_list_remove(&txr->remote_list);

	free(txr);
}

static struct weston_transmitter *
transmitter_get(struct weston_compositor *compositor)
{
	struct transmitter_backend *b = to_transmitter_backend(compositor);

	return b->txr;
}

static void
transmitter_register_connection_status(struct weston_transmitter *txr,
				       struct wl_listener *connected_listener)
{
	wl_signal_add(&txr->connected_signal, connected_listener);
}

static struct weston_surface *
transmitter_get_weston_surface(struct weston_transmitter_surface *txs)
{
	return txs->surface;
}

static const struct weston_transmitter_api transmitter_api_impl = {
	transmitter_get,
	transmitter_connect_to_remote,
	transmitter_remote_get_status,
	transmitter_remote_destroy,
	transmitter_surface_push_to_remote,
	transmitter_surface_get_stream_status,
	transmitter_surface_destroy,
	transmitter_surface_configure,
	transmitter_surface_gather_state,
	transmitter_register_connection_status,
	transmitter_get_weston_surface,
};

void
surface_set_ivi_id(struct weston_transmitter_surface *txs,
		    uint32_t ivi_id)
{
	//dummy function
}

static void
transmitter_surface_set_resize_callback(
	struct weston_transmitter_surface *txs,
	weston_transmitter_ivi_resize_handler_t cb,
	void *data)
{
	txs->resize_handler = cb;
	txs->resize_handler_data = data;
}

static const struct weston_transmitter_ivi_api transmitter_ivi_api_impl = {
	surface_set_ivi_id,
	transmitter_surface_set_resize_callback,
};

int init_trans_api(struct weston_compositor *compositor,
		   struct wl_event_loop *loop,
		   struct transmitter_backend *b)
{
	struct weston_transmitter *txr;
	int ret;
	txr = zalloc(sizeof *txr);
	if (!txr) {
		weston_log("Transmitter disabled\n");
		return -1;
	}
	wl_list_init(&txr->remote_list);
	txr->compositor = compositor;
	txr->compositor_destroy_listener.notify =
					transmitter_compositor_destroyed;
	wl_signal_add(&compositor->destroy_signal,
		      &txr->compositor_destroy_listener);
	ret = weston_plugin_api_register(compositor,
					 WESTON_TRANSMITTER_API_NAME,
					 &transmitter_api_impl,
					 sizeof(transmitter_api_impl));
		if (ret < 0) {
			weston_log("Fatal: Transmitter API registration failed.\n");
			goto fail;
		}

		ret = weston_plugin_api_register(compositor,
						 WESTON_TRANSMITTER_IVI_API_NAME,
						 &transmitter_ivi_api_impl,
						 sizeof(transmitter_ivi_api_impl));
		if (ret < 0) {
			weston_log("Fatal: Transmitter IVI API registration failed.\n");
			goto fail;
		}
	txr->loop = loop;
	/* Loading a waltham renderer library */
	txr->waltham_renderer = weston_load_module("waltham-renderer.so",
						   "waltham_renderer_interface");
	if (txr->waltham_renderer == NULL) {
		weston_log("Failed to load waltham-renderer \n");
		goto fail;
	}
	b->txr = txr;
	return 0;
fail:
	wl_list_remove(&txr->compositor_destroy_listener.link);
	free(txr);
	return -1;
}

int
init_kms_caps(struct transmitter_backend *b)
{
	uint64_t cap;
	int ret;
	clockid_t clk_id;

	weston_log("using %s\n", b->drm.filename);

	ret = drmGetCap(b->drm.fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap);
	if (ret == 0 && cap == 1)
		clk_id = CLOCK_MONOTONIC;
	else
		clk_id = CLOCK_REALTIME;

	if (weston_compositor_set_presentation_clock(b->compositor, clk_id) < 0)
	{
		weston_log("Error: failed to set presentation clock %d.\n",
			   clk_id);
		return -1;
	}

#ifdef HAVE_DRM_ADDFB2_MODIFIERS
        ret = drmGetCap(b->drm.fd, DRM_CAP_ADDFB2_MODIFIERS, &cap);
        if (ret == 0)
                b->fb_modifiers = cap;
        else
#endif
	b->fb_modifiers = 0;

	return 0;
}

static struct transmitter_backend *
transmitter_backend_create(struct weston_compositor *compositor,
			   struct weston_transmitter_backend_config *config)
{
	struct transmitter_backend *b;
	struct udev_device *drm_device;
	struct wl_event_loop *loop;
	const char *seat_id = default_seat;
	const char *session_seat;
	int ret;
	drmModeRes *resources;

	session_seat = getenv("XDG_SEAT");
	if (session_seat)
		seat_id = session_seat;

	if (config->seat_id)
		seat_id = config->seat_id;

	weston_log("initializing transmitter backend\n");

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->state_invalid = true;
	b->drm.fd = -1;
	b->compositor = compositor;

	compositor->backend = &b->base;
	if (parse_gbm_format(config->gbm_format, DRM_FORMAT_XRGB8888,
			     &b->gbm_format) < 0)
		goto err_compositor;

	/* Check if we run transmitter-backend using weston-launch */
	compositor->launcher = weston_launcher_connect(compositor, config->tty,
						       seat_id, true);
	if (compositor->launcher == NULL) {
		weston_log("fatal: transmitter backend should be run using "
			   "weston-launch binary, or your system should "
			   "provide the logind D-Bus API.\n");
		goto err_compositor;
	}

	b->udev = udev_new();
	if (b->udev == NULL) {
		weston_log("failed to initialize udev context\n");
		goto err_launcher;
	}

	b->session_listener.notify = session_notify;
	wl_signal_add(&compositor->session_signal, &b->session_listener);

	if (config->specific_device)
		drm_device = open_specific_drm_device(b,
						      config->specific_device);
	else
		drm_device = find_primary_gpu(b, seat_id);
	if (drm_device == NULL) {
		weston_log("no drm device found\n");
		goto err_udev;
	}
	if (init_kms_caps(b) < 0) {
		weston_log("failed to initialize kms\n");
		goto err_udev_dev;
	}
		if (init_egl(b) < 0) {
			weston_log("failed to initialize egl\n");
			goto err_udev_dev;
		}

	b->base.destroy = transmitter_destroy;
	b->base.repaint_begin = NULL;
	b->base.repaint_flush = NULL;
	b->base.repaint_cancel = NULL;
	b->base.create_output = transmitter_output_create;
	b->base.device_changed = NULL;
	wl_list_init(&b->plane_list);
	weston_setup_vt_switch_bindings(compositor);
	if (udev_input_init(&b->input,
			    compositor, b->udev, seat_id,
			    config->configure_device) < 0) {
		weston_log("failed to create input devices\n");
		goto err_sprite;
	}

	loop = wl_display_get_event_loop(compositor->wl_display);

	b->udev_monitor = udev_monitor_new_from_netlink(b->udev, "udev");
	if (b->udev_monitor == NULL) {
		weston_log("failed to initialize udev monitor\n");
		goto err_drm_source;
	}
	udev_monitor_filter_add_match_subsystem_devtype(b->udev_monitor,
							"drm", NULL);

	if (udev_monitor_enable_receiving(b->udev_monitor) < 0) {
		weston_log("failed to enable udev-monitor receiving\n");
		goto err_udev_monitor;
	}

	udev_device_unref(drm_device);
	if (compositor->renderer->import_dmabuf) {
		if (linux_dmabuf_setup(compositor) < 0)
			weston_log("Error: initializing dmabuf "
				   "support failed.\n");
	}

	if (compositor->capabilities & WESTON_CAP_EXPLICIT_SYNC) {
		if (linux_explicit_synchronization_setup(compositor) < 0)
			weston_log("Error: initializing explicit "
				   " synchronization support failed.\n");
	}

	ret = weston_plugin_api_register(compositor,
					 WESTON_TRANSMITTER_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		weston_log("Failed to register output API.\n");
		goto err_udev_monitor;
	}
	ret = init_trans_api(compositor, loop, b);
	if (ret < 0) {
		weston_log("transmitter registration fail");
	}

	resources = drmModeGetResources(b->drm.fd);
	if (!resources) {
		weston_log("drmModeGetResources failed\n");
		return NULL;
	}
	b->min_width = resources->min_width;	//0
	b->max_width = resources->max_width;	//4096
	b->min_height = resources->min_height;	//0
	b->max_height = resources->max_height;	//2160

	return b;

err_udev_monitor:
	wl_event_source_remove(b->udev_drm_source);
	udev_monitor_unref(b->udev_monitor);
err_drm_source:
	wl_event_source_remove(b->drm_source);
err_sprite:
	if (b->gbm)
		gbm_device_destroy(b->gbm);
err_udev_dev:
	udev_device_unref(drm_device);
err_launcher:
	weston_launcher_destroy(compositor->launcher);
err_udev:
	udev_unref(b->udev);
err_compositor:
	weston_compositor_shutdown(compositor);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_transmitter_backend_config *config)
{
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct transmitter_backend *b;
	struct weston_transmitter_backend_config config = { { 0, } };

	if (config_base == NULL ||
	    config_base->struct_version !=
	    WESTON_TRANSMITTER_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size >
	    sizeof(struct weston_transmitter_backend_config)) {
		weston_log("transmitter backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = transmitter_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	return 0;
}
