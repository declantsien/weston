/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
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
#include <dlfcn.h>

#include "drm-internal.h"
#include "pixman-renderer.h"
#include "pixel-formats.h"
#include "renderer-gl/gl-renderer.h"
#include "shared/weston-egl-ext.h"
#include "linux-dmabuf.h"
#include "linux-explicit-synchronization.h"

struct gl_renderer_interface *gl_renderer;

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
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL. */
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
 *
 * This works around https://bugs.freedesktop.org/show_bug.cgi?id=89689
 * but it's entirely possible we'll see this again on other implementations.
 */
static uint32_t
fallback_format_for(uint32_t format)
{
	const struct pixel_format_info *pf;

	pf = pixel_format_get_info_by_opaque_substitute(format);
	if (!pf)
		return 0;

	return pf->format;
}

static int
drm_backend_create_gl_renderer(struct drm_backend *b)
{
	uint32_t format[3] = {
		b->gbm_format,
		fallback_format_for(b->gbm_format),
		0,
	};
	unsigned n_formats = 2;

	if (format[1])
		n_formats = 3;
	if (gl_renderer->display_create(b->compositor,
					EGL_PLATFORM_GBM_KHR,
					(void *)b->gbm,
					EGL_WINDOW_BIT,
					format,
					n_formats) < 0) {
		return -1;
	}

	return 0;
}

int
init_egl(struct drm_backend *b)
{
	b->gbm = create_gbm_device(b->drm.fd);

	if (!b->gbm)
		return -1;

	if (drm_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		return -1;
	}

	return 0;
}

static void drm_output_fini_cursor_egl(struct drm_output *output)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(output->gbm_cursor_fb); i++) {
		drm_fb_unref(output->gbm_cursor_fb[i]);
		output->gbm_cursor_fb[i] = NULL;
	}
}

static int
drm_output_init_cursor_egl(struct drm_output *output, struct drm_backend *b)
{
	unsigned int i;

	/* No point creating cursors if we don't have a plane for them. */
	if (!output->cursor_plane)
		return 0;

	for (i = 0; i < ARRAY_LENGTH(output->gbm_cursor_fb); i++) {
		struct gbm_bo *bo;

		bo = gbm_bo_create(b->gbm, b->cursor_width, b->cursor_height,
				   GBM_FORMAT_ARGB8888,
				   GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
		if (!bo)
			goto err;

		output->gbm_cursor_fb[i] =
			drm_fb_get_from_bo(bo, b, false, BUFFER_CURSOR);
		if (!output->gbm_cursor_fb[i]) {
			gbm_bo_destroy(bo);
			goto err;
		}
		output->gbm_cursor_handle[i] = gbm_bo_get_handle(bo).s32;
	}

	return 0;

err:
	weston_log("cursor buffers unavailable, using gl cursors\n");
	b->cursors_are_broken = true;
	drm_output_fini_cursor_egl(output);
	return -1;
}

/* Init output state that depends on gl or gbm */
int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b)
{
	uint32_t format[2] = {
		output->gbm_format,
		fallback_format_for(output->gbm_format),
	};
	unsigned n_formats = 1;
	struct weston_mode *mode = output->base.current_mode;
	struct drm_plane *plane = output->scanout_plane;
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
					      format,
					      n_formats) < 0) {
		weston_log("failed to create gl renderer output state\n");
		gbm_surface_destroy(output->gbm_surface);
		output->gbm_surface = NULL;
		return -1;
	}

	drm_output_init_cursor_egl(output, b);

	return 0;
}

void
drm_output_fini_egl(struct drm_output *output)
{
	struct drm_backend *b = to_drm_backend(output->base.compositor);

	/* Destroying the GBM surface will destroy all our GBM buffers,
	 * regardless of refcount. Ensure we destroy them here. */
	if (!b->shutting_down &&
	    output->scanout_plane->state_cur->fb &&
	    output->scanout_plane->state_cur->fb->type == BUFFER_GBM_SURFACE) {
		drm_plane_state_free(output->scanout_plane->state_cur, true);
		output->scanout_plane->state_cur =
			drm_plane_state_alloc(NULL, output->scanout_plane);
		output->scanout_plane->state_cur->complete = true;
	}

	gl_renderer->output_destroy(&output->base);
	gbm_surface_destroy(output->gbm_surface);
	output->gbm_surface = NULL;
	drm_output_fini_cursor_egl(output);
}

struct drm_fb *
drm_output_render_gl(struct drm_output_state *state, pixman_region32_t *damage)
{
	struct drm_output *output = state->output;
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct gbm_bo *bo;
	struct drm_fb *ret;

	output->base.compositor->renderer->repaint_output(&output->base,
							  damage);

	bo = gbm_surface_lock_front_buffer(output->gbm_surface);
	if (!bo) {
		weston_log("failed to lock front buffer: %s\n",
			   strerror(errno));
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

	return ret;
}

static void
switch_to_gl_renderer(struct drm_backend *b)
{
	struct drm_output *output;
	bool dmabuf_support_inited;
	bool linux_explicit_sync_inited;

	if (!b->use_pixman)
		return;

	dmabuf_support_inited = !!b->compositor->renderer->import_dmabuf;
	linux_explicit_sync_inited =
		b->compositor->capabilities & WESTON_CAP_EXPLICIT_SYNC;

	weston_log("Switching to GL renderer\n");

	b->gbm = create_gbm_device(b->drm.fd);
	if (!b->gbm) {
		weston_log("Failed to create gbm device. "
			   "Aborting renderer switch\n");
		return;
	}

	wl_list_for_each(output, &b->compositor->output_list, base.link)
		pixman_renderer_output_destroy(&output->base);

	b->compositor->renderer->destroy(b->compositor);

	if (drm_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		weston_log("Failed to create GL renderer. Quitting.\n");
		/* FIXME: we need a function to shutdown cleanly */
		assert(0);
	}

	wl_list_for_each(output, &b->compositor->output_list, base.link)
		drm_output_init_egl(output, b);

	b->use_pixman = 0;

	if (!dmabuf_support_inited && b->compositor->renderer->import_dmabuf) {
		if (linux_dmabuf_setup(b->compositor) < 0)
			weston_log("Error: initializing dmabuf "
				   "support failed.\n");
	}

	if (!linux_explicit_sync_inited &&
	    (b->compositor->capabilities & WESTON_CAP_EXPLICIT_SYNC)) {
		if (linux_explicit_synchronization_setup(b->compositor) < 0)
			weston_log("Error: initializing explicit "
				   " synchronization support failed.\n");
	}
}

void
renderer_switch_binding(struct weston_keyboard *keyboard,
			const struct timespec *time, uint32_t key, void *data)
{
	struct drm_backend *b =
		to_drm_backend(keyboard->seat->compositor);

	switch_to_gl_renderer(b);
}

int
drm_gbm_output_copy_content_dmafd(struct weston_output *output_base,
				  int dma_fd, int width,
				  int height, int stride,
				  uint format)
{
	struct drm_backend *backend = to_drm_backend(output_base->compositor);
	struct drm_output *output = to_drm_output(output_base);
	struct gbm_bo *bo;
	uint display_format;
	int display_width, display_height, display_stride;
	int display_buffer_fd = -1;
	int num_planes;
	int ret;

	if (!output->gbm_surface) {
		weston_log("%s : gbm surface is NULL\n", __func__);
		return -1;
	}

	bo = gbm_surface_lock_front_buffer(output->gbm_surface);
	if (!bo) {
		weston_log("%s : failed to lock front buffer\n", __func__);
		ret = -1;
		goto error;
	}

	display_width = gbm_bo_get_width(bo);
	display_height = gbm_bo_get_height(bo);
	num_planes = gbm_bo_get_plane_count(bo);
	display_stride = gbm_bo_get_stride(bo);

	display_format = output->gbm_format;

	if (num_planes != 1) {
		weston_log("%s: multi-planer display buffers are not supported\n",
			   __func__);
		goto error;
	}

	ret = drmPrimeHandleToFD(backend->drm.fd, gbm_bo_get_handle(bo).u32,
				 DRM_CLOEXEC, &display_buffer_fd);
	if(ret) {
		weston_log("%s : generating prime handle from gbm bo failed\n",
			   __func__);
		ret = -1;
		goto error;
	}

	ret = gl_renderer->output_copy_content_dmafd(output_base, display_buffer_fd,
						     display_width, display_height,
						     display_stride, display_format,
						     dma_fd, width, height, stride,
						     format);
error:
	if (display_buffer_fd != -1)
		close(display_buffer_fd);

	gbm_surface_release_buffer(output->gbm_surface, bo);

	return ret;
}
