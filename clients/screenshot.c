/*
 * Copyright © 2008 Kristian Høgsberg
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
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <cairo.h>
#include <getopt.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include <wayland-client.h>
#include "weston-screenshooter-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "shared/os-compatibility.h"
#include "shared/xalloc.h"
#include "shared/file-util.h"

/* The screenshooter is a good example of a custom object exposed by
 * the compositor and serves as a test bed for implementing client
 * side marshalling outside libwayland.so */


struct screenshooter_output {
	struct wl_output *output;
	struct wl_buffer *buffer;
	int width, height, offset_x, offset_y;
	void *data;
	struct wl_list link;
};

struct buffer_size {
	int width, height;

	int min_x, min_y;
	int max_x, max_y;
};

struct linux_dmabuf_buffer {
	int fd;
	int pitch, size;
	void *map;
	struct gbm_bo *bo;
	struct wl_buffer *buffer;
	struct wl_list link;
};

struct screenshooter_data {
	struct wl_shm *shm;
	struct wl_list output_list;

	struct weston_screenshooter *screenshooter;
	int buffer_copy_done;

	struct {
		int drm_fd;
		char const *drm_render_node;
		struct gbm_device *gbm;
	} gbm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	int argb8888_format_found;
	struct wl_list dmabuf_buffer_list;
};

static void
gbm_device_init(struct screenshooter_data *sh_data)
{
        sh_data->gbm.drm_fd = open(sh_data->gbm.drm_render_node, O_RDWR);
        if (sh_data->gbm.drm_fd < 0) {
                fprintf(stderr, "failed to open drm device\n");
                return;
        }

        sh_data->gbm.gbm = gbm_create_device(sh_data->gbm.drm_fd);
        if (!sh_data->gbm.gbm) {
                fprintf(stderr, "failed to create gbm device\n");
                goto error;
        }

        return;
error:
        close(sh_data->gbm.drm_fd);
}

static void
gbm_device_deinit(struct screenshooter_data *sh_data)
{
        if (sh_data->gbm.gbm)
                gbm_device_destroy(sh_data->gbm.gbm);

        if (sh_data->gbm.drm_fd > 0)
                close(sh_data->gbm.drm_fd);
}

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
                 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
        struct screenshooter_data *sh_data = data;

        switch (format) {
                case DRM_FORMAT_ARGB8888:
                        sh_data->argb8888_format_found = 1;
                default:
                        break;
        }
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
        dmabuf_format,
        dmabuf_modifiers
};

static void
display_handle_geometry(void *data,
			struct wl_output *wl_output,
			int x,
			int y,
			int physical_width,
			int physical_height,
			int subpixel,
			const char *make,
			const char *model,
			int transform)
{
	struct screenshooter_output *output;

	output = wl_output_get_user_data(wl_output);

	if (wl_output == output->output) {
		output->offset_x = x;
		output->offset_y = y;
	}
}

static void
display_handle_mode(void *data,
		    struct wl_output *wl_output,
		    uint32_t flags,
		    int width,
		    int height,
		    int refresh)
{
	struct screenshooter_output *output;

	output = wl_output_get_user_data(wl_output);

	if (wl_output == output->output && (flags & WL_OUTPUT_MODE_CURRENT)) {
		output->width = width;
		output->height = height;
	}
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode
};

static void
screenshot_done(void *data, struct weston_screenshooter *screenshooter)
{
	struct screenshooter_data *sh_data = data;
	sh_data->buffer_copy_done = 1;
}

static const struct weston_screenshooter_listener screenshooter_listener = {
	screenshot_done
};

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	static struct screenshooter_output *output;
	struct screenshooter_data *sh_data = data;

	if (strcmp(interface, "wl_output") == 0) {
		output = xmalloc(sizeof *output);
		output->output = wl_registry_bind(registry, name,
						  &wl_output_interface, 1);
		wl_list_insert(&sh_data->output_list, &output->link);
		wl_output_add_listener(output->output, &output_listener, output);
	} else if (strcmp(interface, "wl_shm") == 0) {
		sh_data->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, "weston_screenshooter") == 0) {
		sh_data->screenshooter = wl_registry_bind(registry, name,
							  &weston_screenshooter_interface,
							  1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		if (version < 3)
			return;
		sh_data->dmabuf = wl_registry_bind(registry,name,
						   &zwp_linux_dmabuf_v1_interface, 3);

		zwp_linux_dmabuf_v1_add_listener(sh_data->dmabuf, &dmabuf_listener,
						 sh_data);

		gbm_device_init(sh_data);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	/* XXX: unimplemented */
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove
};

static void
create_succeeded(void *data,
                 struct zwp_linux_buffer_params_v1 *params,
                 struct wl_buffer *new_buffer)
{
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct linux_dmabuf_buffer *buffer = data;

	fprintf(stderr, "zwp_linux_buffer_params.create failed\n");

	zwp_linux_buffer_params_v1_destroy(params);

	buffer->buffer = NULL;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
        create_succeeded,
        create_failed
};

static void
cleanup_dmabuf_buffer(struct linux_dmabuf_buffer *buffer)
{
	if (buffer->map != MAP_FAILED)
		munmap(buffer->map, buffer->size);

	if (buffer->buffer)
		wl_buffer_destroy(buffer->buffer);

	if (buffer->fd > 0)
		close(buffer->fd);

	if (buffer->bo != NULL)
		gbm_bo_destroy(buffer->bo);

	free(buffer);
}

static struct wl_buffer *
screenshot_create_dma_buffer(int width, int height,
			     void **data_out,
			     struct screenshooter_data *sh_data)
{
	struct linux_dmabuf_buffer *buffer;
	struct zwp_linux_buffer_params_v1 *params;
	uint32_t format = DRM_FORMAT_ARGB8888;

	buffer = xmalloc(sizeof *buffer);

	buffer->bo = gbm_bo_create(sh_data->gbm.gbm, width,
				   height, format,
				   GBM_BO_USE_SCANOUT);
	if (!buffer->bo) {
		fprintf(stderr, "gbm_bo_create failed\n");
		free(buffer);
		return NULL;
	}

	buffer->pitch = gbm_bo_get_stride(buffer->bo);
	buffer->size = buffer->pitch * height;
	buffer->fd = gbm_bo_get_fd(buffer->bo);
	if (buffer->fd < 0) {
		goto error;
	}

	params = zwp_linux_dmabuf_v1_create_params(sh_data->dmabuf);

	zwp_linux_buffer_params_v1_add(params, buffer->fd, 0,
				       0, buffer->pitch, 0, 0);

	zwp_linux_buffer_params_v1_add_listener(params,
						&params_listener,
						buffer);

	buffer->buffer = zwp_linux_buffer_params_v1_create_immed(
						params, width,
						height, format, 0);
	if (!buffer) {
		fprintf(stderr, "got invalid wl_buffer\n");
		goto error;
	}

	buffer->map = mmap(NULL, buffer->size, PROT_READ,
			   MAP_SHARED, buffer->fd, 0);
	if (buffer->map == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		goto error;
	}

	*data_out = buffer->map;

	wl_list_insert(&sh_data->dmabuf_buffer_list, &buffer->link);

	return buffer->buffer;
error:
	cleanup_dmabuf_buffer(buffer);

	return NULL;
}

static struct wl_buffer *
screenshot_create_shm_buffer(int width, int height, void **data_out,
			     struct wl_shm *shm)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	int fd, size, stride;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %s\n",
			size, strerror(errno));
		return NULL;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(shm, fd, size);
	close(fd);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					   WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	*data_out = data;

	return buffer;
}

static void
screenshot_write_png(const struct buffer_size *buff_size,
		     struct wl_list *output_list)
{
	int output_stride, buffer_stride, i;
	cairo_surface_t *surface;
	void *data, *d, *s;
	struct screenshooter_output *output, *next;
	FILE *fp;
	char filepath[PATH_MAX];

	buffer_stride = buff_size->width * 4;

	data = xmalloc(buffer_stride * buff_size->height);
	if (!data)
		return;

	wl_list_for_each_safe(output, next, output_list, link) {
		output_stride = output->width * 4;
		s = output->data;
		d = data + (output->offset_y - buff_size->min_y) * buffer_stride +
			   (output->offset_x - buff_size->min_x) * 4;

		for (i = 0; i < output->height; i++) {
			memcpy(d, s, output_stride);
			d += buffer_stride;
			s += output_stride;
		}

		free(output);
	}

	surface = cairo_image_surface_create_for_data(data,
						      CAIRO_FORMAT_ARGB32,
						      buff_size->width,
						      buff_size->height,
						      buffer_stride);

	fp = file_create_dated(getenv("XDG_PICTURES_DIR"), "wayland-screenshot-",
			       ".png", filepath, sizeof(filepath));
	if (fp) {
		fclose (fp);
		cairo_surface_write_to_png(surface, filepath);
	}
	cairo_surface_destroy(surface);
	free(data);
}

static int
screenshot_set_buffer_size(struct buffer_size *buff_size, struct wl_list *output_list)
{
	struct screenshooter_output *output;
	buff_size->min_x = buff_size->min_y = INT_MAX;
	buff_size->max_x = buff_size->max_y = INT_MIN;
	int position = 0;

	wl_list_for_each_reverse(output, output_list, link) {
		output->offset_x = position;
		position += output->width;
	}

	wl_list_for_each(output, output_list, link) {
		buff_size->min_x = MIN(buff_size->min_x, output->offset_x);
		buff_size->min_y = MIN(buff_size->min_y, output->offset_y);
		buff_size->max_x =
			MAX(buff_size->max_x, output->offset_x + output->width);
		buff_size->max_y =
			MAX(buff_size->max_y, output->offset_y + output->height);
	}

	if (buff_size->max_x <= buff_size->min_x ||
	    buff_size->max_y <= buff_size->min_y)
		return -1;

	buff_size->width = buff_size->max_x - buff_size->min_x;
	buff_size->height = buff_size->max_y - buff_size->min_y;

	return 0;
}

static void
cleanup_dmabuf_resources(struct screenshooter_data *sh_data)
{
	struct linux_dmabuf_buffer *buf, *buf_nxt;

	wl_list_for_each_safe(buf, buf_nxt, &sh_data->dmabuf_buffer_list, link)
		cleanup_dmabuf_buffer(buf);

	gbm_device_deinit(sh_data);
}

static void
print_usage_and_exit(void)
{
	printf("usage flags:\n"
		"\t'-r,--drm-render-node=<>'"
		"\n\t\tthe full path to the drm render node to use\n"
		"\t'-s,--use-dma_buffer'"
		"\n\t\tuse dma buffer for screenshot\n");
	exit(0);
}

int main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct screenshooter_output *output;
	struct buffer_size buff_size = {};
	struct screenshooter_data sh_data = {};
	char const *drm_render_node = "/dev/dri/renderD128";
	int c, option_index, ret = 0, use_dma_buffer = 0;

	static struct option long_options[] = {
		{ "drm-render-node",  required_argument, 0,  'r' },
		{ "use-dma-buffer",   no_argument,       0,  'd' },
		{ "help",             no_argument,       0,  'h' },
		{ 0, 0, 0, 0}
        };

        while ((c = getopt_long(argc, argv, "hr:d",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'r':
			drm_render_node = optarg;
			break;
		case 'd':
			use_dma_buffer = 1;
			break;
		default:
			print_usage_and_exit();
		}
	}

	sh_data.gbm.drm_render_node = drm_render_node;

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	wl_list_init(&sh_data.output_list);

	if (use_dma_buffer)
		wl_list_init(&sh_data.dmabuf_buffer_list);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &sh_data);
	wl_display_dispatch(display);
	wl_display_roundtrip(display);
	if (sh_data.screenshooter == NULL) {
		fprintf(stderr, "display doesn't support screenshooter\n");
		return -1;
	}

	if (use_dma_buffer &&
	    (sh_data.dmabuf == NULL || sh_data.gbm.gbm == NULL ||
	     !sh_data.argb8888_format_found)) {
		fprintf(stderr, "display doesn't support dmabuf screenshot\n");
		return -1;
	}

	weston_screenshooter_add_listener(sh_data.screenshooter,
					  &screenshooter_listener,
					  &sh_data);

	if (screenshot_set_buffer_size(&buff_size, &sh_data.output_list))
		return -1;


	wl_list_for_each(output, &sh_data.output_list, link) {
		if (use_dma_buffer)
			output->buffer =
				screenshot_create_dma_buffer(output->width,
							     output->height,
							     &output->data,
							     &sh_data);
		else
			output->buffer =
				screenshot_create_shm_buffer(output->width,
							     output->height,
							     &output->data,
							     sh_data.shm);

		if (!output->buffer) {
			ret = -1;
			goto error;
		}

		weston_screenshooter_shoot(sh_data.screenshooter,
					   output->output,
					   output->buffer);
		sh_data.buffer_copy_done = 0;
		while (!sh_data.buffer_copy_done)
			wl_display_roundtrip(display);
	}

	screenshot_write_png(&buff_size, &sh_data.output_list);
error:
	if (use_dma_buffer)
		cleanup_dmabuf_resources(&sh_data);

	return ret;
}
