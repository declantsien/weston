/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright 2022 Collabora, Ltd.
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

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <pixman.h>
#include <cairo.h>
#include <assert.h>
#include <getopt.h>
#include <xf86drm.h>
#include <gbm.h>

#include <wayland-client.h>
#include "weston-output-capture-client-protocol.h"
#include "shared/os-compatibility.h"
#include "shared/xalloc.h"
#include "shared/file-util.h"
#include "pixel-formats.h"
#include "shared/weston-drm-fourcc.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#define DRM_RENDER_NODE "/dev/dri/card0"
#define MAX_BUFFER_PLANES 4

#define SHM_BUFFER 0
#define DMA_BUFFER 1

#define FRAMEBUFFER_SOURCE 0
#define WRITEBACK_SOURCE 1
 
struct screenshooter_app {
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct weston_capture_v1 *capture_factory;
	int buffer_type;
	int source_type;

	struct wl_list output_list; /* struct screenshooter_output::link */

	bool retry;
	bool failed;
	int waitcount;
	struct {
		int drm_fd;
		struct gbm_device *device;
	} gbm;
};

struct screenshooter_buffer {
	size_t len;
	void *data;
	struct wl_buffer *wl_buffer;
	pixman_image_t *image;
	int buffer_type; /* Indicate SHM_BUFFER or DMA_BUFFER */
	struct {
		struct gbm_bo *bo;
		uint64_t modifier;
		int plane_count;
		int plain_fds[MAX_BUFFER_PLANES];
		uint32_t strides[MAX_BUFFER_PLANES];
		uint32_t offsets[MAX_BUFFER_PLANES];
		int release_fence_fd;
	} dma_buffer;
};

struct screenshooter_output {
	struct screenshooter_app *app;
	struct wl_list link; /* struct screenshooter_app::output_list */

	struct wl_output *wl_output;
	int offset_x, offset_y;

	struct weston_capture_source_v1 *source;

	int buffer_width;
	int buffer_height;
	const struct pixel_format_info *fmt;
	struct screenshooter_buffer *buffer;
};

struct buffer_size {
	int width, height;

	int min_x, min_y;
	int max_x, max_y;
};

static void
create_succeeded(void *data,
                 struct zwp_linux_buffer_params_v1 *params,
                 struct wl_buffer *new_buffer)
{
	/* not used for zwp_linux_buffer_params_v1_create_immed() */
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	fprintf(stderr, "%s: error: zwp_linux_buffer_params.create failed.\n",
		__func__);
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	create_succeeded,
	create_failed
};

static struct screenshooter_buffer *
screenshot_create_shm_buffer(struct screenshooter_app *app,
			     size_t width, size_t height,
			     const struct pixel_format_info *fmt)
{
	struct screenshooter_buffer *buffer;
	struct wl_shm_pool *pool;
	int fd;
	size_t bytes_pp;
	size_t stride;

	assert(width > 0);
	assert(height > 0);
	assert(fmt && fmt->bpp > 0);
	assert(fmt->pixman_format);

	buffer = xzalloc(sizeof *buffer);

	bytes_pp = fmt->bpp / 8;
	stride = width * bytes_pp;
	buffer->len = stride * height;
	buffer->buffer_type = SHM_BUFFER;
	assert(width == stride / bytes_pp);
	assert(height == buffer->len / stride);

	fd = os_create_anonymous_file(buffer->len);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %zd B failed: %s\n",
			buffer->len, strerror(errno));
		free(buffer);
		return NULL;
	}

	buffer->data = mmap(NULL, buffer->len, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	if (buffer->data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		free(buffer);
		return NULL;
	}

	pool = wl_shm_create_pool(app->shm, fd, buffer->len);
	close(fd);
	buffer->wl_buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					  pixel_format_get_shm_format(fmt));
	wl_shm_pool_destroy(pool);

	buffer->image = pixman_image_create_bits(fmt->pixman_format,
						 width, height,
						 buffer->data, stride);
	abort_oom_if_null(buffer->image);

	return buffer;
}


static struct screenshooter_buffer *
screenshot_create_dma_buffer(struct screenshooter_app *app,
			     int width, int height,
			     const struct pixel_format_info *fmt)
{
	struct zwp_linux_buffer_params_v1 *params;
	struct screenshooter_buffer* buffer = NULL;
	uint32_t format = fmt->format;
	int ret;
	buffer = xzalloc(sizeof *buffer);
	if (buffer == NULL) {
		fprintf(stderr, "%s: error: failed allocate screenshooter_buffer\n", __func__);
		return NULL;
	}

	buffer->buffer_type = DMA_BUFFER;

	for (int i = 0 ; i < MAX_BUFFER_PLANES ; i++)
		buffer->dma_buffer.plain_fds[i] = -1;
	
	buffer->dma_buffer.bo = gbm_bo_create(app->gbm.device, width,
					      height, format, GBM_BO_USE_RENDERING);

	if (!buffer->dma_buffer.bo) {
		fprintf(stderr, "%s: error: unable to create gbm bo, error %s\n",
			__func__, strerror(errno));
		free(buffer);
		buffer = NULL;
		return buffer;
	}

	buffer->dma_buffer.modifier = gbm_bo_get_modifier(buffer->dma_buffer.bo);
	
	params = zwp_linux_dmabuf_v1_create_params(app->dmabuf);
	buffer->dma_buffer.plane_count = gbm_bo_get_plane_count(buffer->dma_buffer.bo);

	for (int i = 0 ; i < buffer->dma_buffer.plane_count ; i++) {
		ret = drmPrimeHandleToFD(app->gbm.drm_fd,
			gbm_bo_get_handle_for_plane(buffer->dma_buffer.bo, i).u32,
			0, &buffer->dma_buffer.plain_fds[i]);
		if (ret < 0 || buffer->dma_buffer.plain_fds[i] < 0) {
			fprintf(stderr, "%s: error: failed to get dma buffer fd\n",
				__func__);
			goto error;
		}

		buffer->dma_buffer.offsets[i] = gbm_bo_get_offset(buffer->dma_buffer.bo, i);
		buffer->dma_buffer.strides[i] = gbm_bo_get_stride_for_plane(buffer->dma_buffer.bo, i);

		zwp_linux_buffer_params_v1_add(params,
					       buffer->dma_buffer.plain_fds[i], i,
					       buffer->dma_buffer.offsets[i],
					       buffer->dma_buffer.strides[i],
					       buffer->dma_buffer.modifier >> 32,
					       buffer->dma_buffer.modifier & 0xffffffff);
	}

	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, app);

	buffer->data = gbm_bo_map(buffer->dma_buffer.bo, 0, 0, width, height, 0,
				      buffer->dma_buffer.strides, &buffer->data);
	if (!buffer->data) {
		fprintf(stderr, "%s: error: failed to map DMA buffer\n", __func__);
		goto error;
	}

	buffer->wl_buffer = zwp_linux_buffer_params_v1_create_immed(
			params, width, height, format, 0);
	
	buffer->image = pixman_image_create_bits(fmt->pixman_format,
						 width, height,
						 buffer->data, 
						 buffer->dma_buffer.strides[0]);
error:
	zwp_linux_buffer_params_v1_destroy(params);
	return buffer;
}

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	/* XXX: unimplemented */
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	/* XXX: unimplemented */
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
        dmabuf_format,
        dmabuf_modifiers
};

static void
screenshooter_buffer_destroy(struct screenshooter_buffer *buffer)
{
	if (!buffer)
		return;

	pixman_image_unref(buffer->image);
	if (buffer->buffer_type == SHM_BUFFER) {
		if (buffer->data)
			munmap(buffer->data, buffer->len);
	} else {
		if (buffer->buffer_type == DMA_BUFFER) {
			if (buffer->data)
				gbm_bo_unmap(buffer->dma_buffer.bo, buffer->data);
			for (int i = 0; i < MAX_BUFFER_PLANES; i++)
				if (buffer->dma_buffer.plain_fds[i] >= 0)
					close(buffer->dma_buffer.plain_fds[i]);
			if (buffer->dma_buffer.bo)
				gbm_bo_destroy(buffer->dma_buffer.bo);
		}
	}
	wl_buffer_destroy(buffer->wl_buffer);
	free(buffer);
}

static void
capture_source_handle_format(void *data,
			     struct weston_capture_source_v1 *proxy,
			     uint32_t drm_format)
{
	struct screenshooter_output *output = data;

	assert(output->source == proxy);

	output->fmt = pixel_format_get_info(drm_format);
}

static void
capture_source_handle_size(void *data,
			   struct weston_capture_source_v1 *proxy,
			   int32_t width, int32_t height)
{
	struct screenshooter_output *output = data;

	assert(width > 0);
	assert(height > 0);

	output->buffer_width = width;
	output->buffer_height = height;
}

static void
capture_source_handle_complete(void *data,
			       struct weston_capture_source_v1 *proxy)
{
	struct screenshooter_output *output = data;

	output->app->waitcount--;
}

static void
capture_source_handle_retry(void *data,
			    struct weston_capture_source_v1 *proxy)
{
	struct screenshooter_output *output = data;

	output->app->waitcount--;
	output->app->retry = true;
}

static void
capture_source_handle_failed(void *data,
			     struct weston_capture_source_v1 *proxy,
			     const char *msg)
{
	struct screenshooter_output *output = data;

	output->app->waitcount--;
	output->app->failed = true;

	if (msg)
		fprintf(stderr, "Output capture error: %s\n", msg);
}

static const struct weston_capture_source_v1_listener capture_source_handlers = {
	.format = capture_source_handle_format,
	.size = capture_source_handle_size,
	.complete = capture_source_handle_complete,
	.retry = capture_source_handle_retry,
	.failed = capture_source_handle_failed,
};

static void
create_output(struct screenshooter_app *app, uint32_t output_name, uint32_t version)
{
	struct screenshooter_output *output;

	version = MIN(version, 4);
	output = xzalloc(sizeof *output);
	output->app = app;
	output->wl_output = wl_registry_bind(app->registry, output_name,
					     &wl_output_interface, version);
	abort_oom_if_null(output->wl_output);

	output->source = weston_capture_v1_create(app->capture_factory,
						  output->wl_output,
						  ((!app->source_type) ?
						  WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER :
						  WESTON_CAPTURE_V1_SOURCE_WRITEBACK));
	abort_oom_if_null(output->source);
	weston_capture_source_v1_add_listener(output->source,
					      &capture_source_handlers, output);

	wl_list_insert(&app->output_list, &output->link);
}

static void
destroy_output(struct screenshooter_output *output)
{
	weston_capture_source_v1_destroy(output->source);

	if (wl_output_get_version(output->wl_output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
		wl_output_release(output->wl_output);
	else
		wl_output_destroy(output->wl_output);

	screenshooter_buffer_destroy(output->buffer);
	wl_list_remove(&output->link);
	free(output);
}

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	struct screenshooter_app *app = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		create_output(app, name, version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		/*
		 * Not listening for format advertisements,
		 * weston_capture_source_v1.format event tells us what to use.
		 */
	} else if (strcmp(interface, weston_capture_v1_interface.name) == 0) {
		app->capture_factory = wl_registry_bind(registry, name,
							&weston_capture_v1_interface,
							1);
	} else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		if (version < 3)
			return;
		app->dmabuf = wl_registry_bind(registry, name,
							&zwp_linux_dmabuf_v1_interface,
							3);
		zwp_linux_dmabuf_v1_add_listener(app->dmabuf, &dmabuf_listener, app);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	/* Dynamic output removals will just fail the respective shot. */
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove
};

static void
screenshooter_output_capture(struct screenshooter_output *output)
{
	screenshooter_buffer_destroy(output->buffer);
	if (output->app->buffer_type == SHM_BUFFER) {
		output->buffer = screenshot_create_shm_buffer(output->app,
							      output->buffer_width,
							      output->buffer_height,
							      output->fmt);
	} else if (output->app->buffer_type == DMA_BUFFER) {
		output->buffer = screenshot_create_dma_buffer(output->app,
							      output->buffer_width,
							      output->buffer_height,
							      output->fmt);
	}
	abort_oom_if_null(output->buffer);

	weston_capture_source_v1_capture(output->source,
					 output->buffer->wl_buffer);
	output->app->waitcount++;
}

static void
screenshot_write_png(const struct buffer_size *buff_size,
		     struct wl_list *output_list)
{
	pixman_image_t *shot;
	cairo_surface_t *surface;
	struct screenshooter_output *output;
	FILE *fp;
	char filepath[PATH_MAX];

	shot = pixman_image_create_bits(PIXMAN_a8r8g8b8,
					buff_size->width, buff_size->height,
					NULL, 0);
	abort_oom_if_null(shot);

	wl_list_for_each(output, output_list, link) {
		pixman_image_composite32(PIXMAN_OP_SRC,
					 output->buffer->image, /* src */
					 NULL, /* mask */
					 shot, /* dest */
					 0, 0, /* src x,y */
					 0, 0, /* mask x,y */
					 output->offset_x, output->offset_y, /* dst x,y */
					 output->buffer_width, output->buffer_height);
	}

	surface = cairo_image_surface_create_for_data((void *)pixman_image_get_data(shot),
						      CAIRO_FORMAT_ARGB32,
						      pixman_image_get_width(shot),
						      pixman_image_get_height(shot),
						      pixman_image_get_stride(shot));

	fp = file_create_dated(getenv("XDG_PICTURES_DIR"), "wayland-screenshot-",
			       ".png", filepath, sizeof(filepath));
	if (fp) {
		fclose (fp);
		cairo_surface_write_to_png(surface, filepath);
		printf("Take screenshoot done!\n");
	}
	cairo_surface_destroy(surface);
	pixman_image_unref(shot);
}

static int
screenshot_set_buffer_size(struct buffer_size *buff_size,
			   struct wl_list *output_list)
{
	struct screenshooter_output *output;
	buff_size->min_x = buff_size->min_y = INT_MAX;
	buff_size->max_x = buff_size->max_y = INT_MIN;
	int position = 0;

	wl_list_for_each_reverse(output, output_list, link) {
		output->offset_x = position;
		position += output->buffer_width;
	}

	wl_list_for_each(output, output_list, link) {
		buff_size->min_x = MIN(buff_size->min_x, output->offset_x);
		buff_size->min_y = MIN(buff_size->min_y, output->offset_y);
		buff_size->max_x =
			MAX(buff_size->max_x, output->offset_x + output->buffer_width);
		buff_size->max_y =
			MAX(buff_size->max_y, output->offset_y + output->buffer_height);
	}

	if (buff_size->max_x <= buff_size->min_x ||
	    buff_size->max_y <= buff_size->min_y)
		return -1;

	buff_size->width = buff_size->max_x - buff_size->min_x;
	buff_size->height = buff_size->max_y - buff_size->min_y;

	return 0;
}

static void
cleanup_gbm(struct screenshooter_app *app)
{
	if (app->gbm.device)
		gbm_device_destroy(app->gbm.device);

	if (app->gbm.drm_fd >= 0)
		close(app->gbm.drm_fd);
}

static int
setup_gbm(struct screenshooter_app *app, const char *drm_render_node)
{
	app->gbm.drm_fd = open(drm_render_node, O_RDWR);
	if (app->gbm.drm_fd < 0) {
		fprintf(stderr, "failed to open drm render node %s\n",
			drm_render_node);
		return -1;
	}

	app->gbm.device = gbm_create_device(app->gbm.drm_fd);
	if (app->gbm.device == NULL) {
		fprintf(stderr, "failed to create gbm device %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void
print_usage_and_exit(void)
{
	printf("usage flags:\n"
	       "\t'-b,--buffer-type=<>'"
	       "\n\t\t0 to generate screenshot using SHM buffer (default), "
	       "\n\t\t1 to generate screenshot using DMA buffer\n"
	       "\t'-s,--source-type=<>'"
	       "\n\t\t0 to use framebuffer source (default), "
	       "\n\t\t1 to use writeback source\n"
	       "\t'-d,--drm-render-node=<>'"
	       "\n\t\tthe full path to the drm render node to use, "
	       "default=%s\n",
	       DRM_RENDER_NODE);
	exit(0);
}

static int
check_arg(const char* c)
{
	if (!strcmp(c, "1"))
		return 1;

	if (!strcmp(c, "0"))
		return 0;

	print_usage_and_exit();

	return 0;
}

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct screenshooter_output *output;
	struct screenshooter_output *tmp_output;
	struct buffer_size buff_size = {};
	struct screenshooter_app app = {};

	int c, option_index;
	char *drm_device_node = DRM_RENDER_NODE;
	app.buffer_type = SHM_BUFFER;
	app.source_type = FRAMEBUFFER_SOURCE;
	
	static struct option long_options[] = {
		{"buffer-type", required_argument, 0,  'b' },
		{"source-type", required_argument, 0,  's' },
		{"drm-render-node", required_argument, 0, 'd'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "hb:s:d:",
			long_options, &option_index)) != -1) {
		switch(c) {
		case 'b':
			app.buffer_type = check_arg(optarg);
			break;
		case 's':
			app.source_type = check_arg(optarg);
			break;
		case 'd':
			drm_device_node = optarg;
			break;
		default:
			print_usage_and_exit();
		}
	}

	wl_list_init(&app.output_list);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	app.registry = wl_display_get_registry(display);
	wl_registry_add_listener(app.registry, &registry_listener, &app);

	/* Process wl_registry advertisements */
	wl_display_roundtrip(display);

	if (!app.capture_factory) {
		fprintf(stderr, "Error: display does not support weston_capture_v1\n");
		return -1;
	}
	if (app.source_type == FRAMEBUFFER_SOURCE) {
		printf("Info: Only support shm buffer with framebuffer source\n");
		app.buffer_type = SHM_BUFFER;
	} else {
		if ((app.buffer_type == DMA_BUFFER) && (app.dmabuf) && (setup_gbm(&app, drm_device_node))) {
			printf("Warn: failed to set up gbm device => Try to use shm buffer!\n");
			app.buffer_type = SHM_BUFFER;
		} else if ((app.buffer_type == DMA_BUFFER) && (!app.dmabuf)) {
			printf("Warn: display does not support dma buffer => Try to use shm buffer!\n");
			app.buffer_type = SHM_BUFFER;
		}
		printf("Info: take screenshot with %s and writeback source\n", 
				(app.buffer_type == DMA_BUFFER) ? "dma buffer" : "shm buffer");
	}

	if((app.buffer_type == SHM_BUFFER) && (!app.shm)) {
		fprintf(stderr, "Error: display does not support wl_shm\n");
		return -1;
	}

	/* Process initial events for wl_output and weston_capture_source_v1 */
	wl_display_roundtrip(display);

	do {
		app.retry = false;

		wl_list_for_each(output, &app.output_list, link)
			screenshooter_output_capture(output);

		while (app.waitcount > 0 && !app.failed) {
			if (wl_display_dispatch(display) < 0)
				app.failed = true;
			assert(app.waitcount >= 0);
		}
	} while (app.retry && !app.failed);

	if (!app.failed) {
		if (screenshot_set_buffer_size(&buff_size, &app.output_list) < 0)
			return -1;
		screenshot_write_png(&buff_size, &app.output_list);
	} else {
		fprintf(stderr, "Error: screenshot or protocol failure\n");
	}

	wl_list_for_each_safe(output, tmp_output, &app.output_list, link)
		destroy_output(output);

	if (app.buffer_type == DMA_BUFFER)
		cleanup_gbm(&app);
	weston_capture_v1_destroy(app.capture_factory);
	if (app.shm)
		wl_shm_destroy(app.shm);
	if (app.dmabuf)
		zwp_linux_dmabuf_v1_destroy(app.dmabuf);
	wl_registry_destroy(app.registry);
	wl_display_disconnect(display);

	return 0;
}
