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

#ifndef TRANSMITTER_INTERNAL_H_
#define TRANSMITTER_INTERNAL_H_

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
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>
#include <libudev.h>

#include <libweston/libweston.h>
#include <libweston/weston-log.h>
#include "shared/helpers.h"
#include "libinput-seat.h"
#include "backend.h"
#include "libweston-internal.h"
#include <waltham-object.h>
#include <waltham-client.h>
#include <waltham-connection.h>
#include <libweston/backend-transmitter.h>

#ifndef GBM_BO_USE_LINEAR
#define GBM_BO_USE_LINEAR (1 << 4)
#endif

struct transmitter_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;
	struct udev *udev;
	struct wl_event_source *drm_source;
	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_drm_source;
	struct {
		int id;
		int fd;
		char *filename;
		dev_t devnum;
	} drm;
	struct gbm_device *gbm;
	struct wl_listener session_listener;
	uint32_t gbm_format;
	/* we need these parameters in order to not fail drmModeAddFB2()
	 * due to out of bounds dimensions.
	 */
	int min_width, max_width;
	int min_height, max_height;
	void *repaint_data;
	bool state_invalid;
	struct udev_input input;
	bool fb_modifiers;
	struct weston_transmitter_remote *remote;
};

enum drm_fb_type {
	BUFFER_INVALID = 0, /**< never used */
	BUFFER_CLIENT, /**< directly sourced from client */
	BUFFER_DMABUF, /**< imported from linux_dmabuf client */
	BUFFER_PIXMAN_DUMB, /**< internal Pixman rendering */
	BUFFER_GBM_SURFACE, /**< internal EGL rendering */
	BUFFER_CURSOR, /**< internal cursor buffer */
};

struct drm_fb {
	enum drm_fb_type type;
	int refcnt;
	uint32_t fb_id, size;
	uint32_t handles[4];
	uint32_t strides[4];
	uint32_t offsets[4];
	int num_planes;
	const struct pixel_format_info *format;
	uint64_t modifier;
	int width, height;
	int fd;
	struct weston_buffer_reference buffer_ref;
	struct weston_buffer_release_reference buffer_release_ref;

	/* Used by gbm fbs */
	struct gbm_bo *bo;
	struct gbm_surface *gbm_surface;

	void *map; /* Used by dumb fbs */
};

struct transmitter_head {
	struct weston_head base;
	struct transmitter_backend *backend;
};

struct transmitter_output {
	struct weston_output base;
	struct transmitter_backend *backend;
	struct weston_transmitter_output_info info;
	struct {
		bool draw_initial_frame;
		struct wl_surface *surface;
		struct wl_output *output;
		struct wl_display *display;
		int configure_width, configure_height;
		bool wait_for_configure;
	} parent;
	struct weston_transmitter_remote *remote;
	struct wl_list link; /* weston_transmitter_remote::output_list */
	struct frame *frame;
	struct wl_event_source *finish_frame_timer;
	struct wl_callback *frame_cb;
	struct renderer *renderer;
};

struct renderer {
	void (*repaint_output)(struct transmitter_output *base);
	struct GstAppContext *ctx;
	int32_t dmafd;
	int buf_stride;
	int surface_width;
	int surface_height;
	bool recorder_enabled;
};

static inline struct transmitter_head *
to_transmitter_head(struct weston_head *base)
{
	return container_of(base, struct transmitter_head, base);
}

static inline struct transmitter_output *
to_transmitter_output(struct weston_output *base)
{
	return container_of(base, struct transmitter_output, base);
}

static inline struct transmitter_backend *
to_transmitter_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct transmitter_backend, base);
}

struct drm_fb *
drm_fb_ref(struct drm_fb *fb);

struct drm_fb *
drm_fb_get_from_bo(struct gbm_bo *bo, struct transmitter_backend *backend,
		   bool is_opaque, enum drm_fb_type type);

int
drm_get_dma_fd_from_view(struct weston_output *base,
			 struct weston_view *ev, int *buf_stride);
#endif
