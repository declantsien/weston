/*
 * Copyright © 2024 Robert Bosch GmbH
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

#include <assert.h>
#include <time.h>
#include <pixman.h>
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
#include <cairo.h>
#include <getopt.h>
#include <xf86drm.h>
#include <gbm.h>
#include <png.h>

#include <wayland-client.h>
#include "shared/string-helpers.h"
#include "shared/os-compatibility.h"
#include "shared/weston-drm-fourcc.h"
#include "weston-output-capture-client-protocol.h"
#include "shared/xalloc.h"
#include "shared/file-util.h"
#include "pixel-formats.h"
#include "shared/weston-drm-fourcc.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define DRM_RENDER_NODE "/dev/dri/card0"
#define MAX_BUFFER_PLANES 4

#define SHM_BUFFER 0
#define DMA_BUFFER 1

struct buffer {
    struct wl_buffer *wl_buffer;
    void * data;
    size_t len;
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

struct screenshooter_app {
    struct wl_display *wl_display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_compositor * wl_compositor;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct weston_capture_v1 *capture_factory;
    struct  {
        struct xdg_surface *xdg_surface;
        struct xdg_toplevel *xdg_toplevel;
        struct wl_surface *wl_surface;
        cairo_surface_t	*ctx_image;
        struct buffer *buffer;
        uint32_t surface_id;
        int x;
        int y;
        int width;
        int height;
        int stride;
        char * reference_image_name;
    } surface;
    int buffer_type;
    uint32_t last_serial;

    struct wl_list output_list; /* struct screenshooter_output::link */

    bool retry;
    bool failed;
    int waitcount;
    struct {
        int drm_fd;
        struct gbm_device *device;
    } gbm;
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
    struct buffer *buffer;
    char output_image_fullname[PATH_MAX];
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

static struct buffer *
screenshot_create_dma_buffer(struct screenshooter_app *app,
                 int width, int height,
                 const struct pixel_format_info *fmt)
{
    struct zwp_linux_buffer_params_v1 *params;
    struct buffer* buffer = NULL;
    uint32_t format = fmt->format;
    int ret;
    buffer = xzalloc(sizeof *buffer);
    if (buffer == NULL) {
        fprintf(stderr, "%s: error: failed allocate buffer\n", __func__);
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

static struct buffer *
screenshot_create_shm_buffer(struct screenshooter_app *app,
                 size_t width, size_t height,
                 const struct pixel_format_info *fmt)
{
    struct buffer *buffer;
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

    assert(width == stride / bytes_pp);
    assert(height == buffer->len / stride);

    buffer->buffer_type = SHM_BUFFER;

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

static void
screenshooter_buffer_destroy(struct buffer *buffer)
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
                                              WESTON_CAPTURE_V1_SOURCE_WRITEBACK);
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
pointer_handle_enter(void *data, struct wl_pointer *pointer,
             uint32_t serial, struct wl_surface *surface,
             wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    struct screenshooter_app *app = data;
    app->last_serial = serial;
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
             uint32_t serial, struct wl_surface *surface)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
              uint32_t time, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
              uint32_t time, uint32_t button, uint32_t state_w)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_axis(void *data, struct wl_pointer *pointer,
            uint32_t time, uint32_t axis, wl_fixed_t value)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *pointer,
               uint32_t source)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *pointer,
             uint32_t time, uint32_t axis)
{
    /* XXX: unimplemented */
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer,
                 uint32_t axis, int32_t discrete)
{
    /* XXX: unimplemented */
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
             enum wl_seat_capability caps)
{
    struct screenshooter_app *app = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_set_user_data(app->pointer, app);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
}

static void
seat_handle_name(void *data, struct wl_seat *seat,
         const char *name)
{
    /* XXX: unimplemented */
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name
};

static void
handle_global(void *data, struct wl_registry *registry,
          uint32_t name, const char *interface, uint32_t version)
{
    struct screenshooter_app *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->wl_compositor =
            wl_registry_bind(registry, name,
                             &wl_compositor_interface, version);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        create_output(app, name, version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name,
                                    &wl_shm_interface, version);
        /*
         * Not listening for format advertisements,
         * weston_capture_source_v1.format event tells us what to use.
         */
    } else if (strcmp(interface, weston_capture_v1_interface.name) == 0) {
        app->capture_factory = wl_registry_bind(registry, name,
                                                &weston_capture_v1_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name,
                                        &xdg_wm_base_interface, version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app->seat = wl_registry_bind(registry, name,
                                     &wl_seat_interface, version);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version < 3)
            return;
        app->dmabuf = wl_registry_bind(registry, name,
                                       &zwp_linux_dmabuf_v1_interface, 3);
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
    if (output->app->buffer_type == DMA_BUFFER) {
        output->buffer = screenshot_create_dma_buffer(output->app,
                                                      output->buffer_width,
                                                      output->buffer_height,
                                                      output->fmt);
        if(!output->buffer) {
            printf("Warn: Cannot create DMA buffer => Try to use shm buffer!\n");
        }
    }
    if (output->app->buffer_type == SHM_BUFFER || !output->buffer) {
        output->buffer = screenshot_create_shm_buffer(output->app,
                                                      output->buffer_width,
                                                      output->buffer_height,
                                                      output->fmt);
    }
    abort_oom_if_null(output->buffer);
    /* hide cursor */
    if(output->app->pointer)
        wl_pointer_set_cursor(output->app->pointer, output->app->last_serial,
                              NULL, 0, 0);

    weston_capture_source_v1_capture(output->source,
                                     output->buffer->wl_buffer);
    output->app->waitcount++;
}

static void
frame_callback_handler(void *data, struct wl_callback *callback, uint32_t time)
{
    int *done = data;

    *done = 1;

    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
    frame_callback_handler
};

static struct wl_callback *
frame_callback_set(struct wl_surface *surface, int *done)
{
    struct wl_callback *callback;

    *done = 0;
    callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &frame_listener, done);

    return callback;
}

static int
frame_callback_wait_nofail(struct screenshooter_app *app, int *done)
{
    while (!*done) {
        if (wl_display_dispatch(app->wl_display) < 0)
            return 0;
    }
    return 1;
}

static int
draw_image(struct screenshooter_app *app, char * path_file)
{
    cairo_surface_t *cairo_surface;
    const struct pixel_format_info *pfmt = pixel_format_get_info(DRM_FORMAT_ARGB8888);
    int done;
    void * data;

    cairo_surface = cairo_image_surface_create_from_png(path_file);
    if (NULL == cairo_surface) {
        printf("Failed to load_cairo_surface %s\n", path_file);
        return 1;
    }
    app->surface.ctx_image = cairo_surface;
    app->surface.width = cairo_image_surface_get_width(app->surface.ctx_image);
    app->surface.height = cairo_image_surface_get_height(app->surface.ctx_image);
    app->surface.stride = cairo_image_surface_get_stride(app->surface.ctx_image);
    app->surface.buffer = 
                    screenshot_create_shm_buffer(app, app->surface.width,
                                          app->surface.height,
                                        pfmt);

    data = cairo_image_surface_get_data(app->surface.ctx_image);
    memcpy(app->surface.buffer->data, data,
             app->surface.stride * app->surface.height);
    app->surface.buffer->len = app->surface.stride * app->surface.height;
    wl_surface_attach(app->surface.wl_surface,
             app->surface.buffer->wl_buffer, 0, 0);
    wl_surface_damage(app->surface.wl_surface, 0, 0,
             app->surface.width, app->surface.height);
    frame_callback_set(app->surface.wl_surface, &done);
    wl_surface_commit(app->surface.wl_surface);
    assert(frame_callback_wait_nofail(app, &done));
    return 0;
}

static void
create_image(char * imageFile, int width, int height)
{
    double font_size = 20.0;
    double x = 50.0, y = 100.0;
    char * text = "Hello world";

    cairo_surface_t* cairo_surface =
                 cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cairo_context = cairo_create(cairo_surface);
    /* Draw three alternating colored rectangles */
    cairo_set_source_rgb(cairo_context, 1.0, 0.0, 0.0); /* Red */
    cairo_rectangle(cairo_context, 0, 0, width/3, height);
    cairo_fill(cairo_context);

    cairo_set_source_rgb(cairo_context, 0.0, 1.0, 0.0); /* Green */
    cairo_rectangle(cairo_context, width/3, 0, width/3, height);
    cairo_fill(cairo_context);

    cairo_set_source_rgb(cairo_context, 0.0, 0.0, 1.0); /* Blue */
    cairo_rectangle(cairo_context, (width/3) * 2, 0, width/3, height);
    cairo_fill(cairo_context);
    /* set green text */
    cairo_select_font_face(cairo_context, "Arial", CAIRO_FONT_SLANT_NORMAL,
                     CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cairo_context, font_size);
    cairo_set_source_rgb(cairo_context, 0.0, 1.0, 0.0);
    cairo_move_to(cairo_context, x, y);
    cairo_show_text(cairo_context, text);
    /* save to png image */
    cairo_surface_write_to_png(cairo_surface, imageFile);
    cairo_destroy(cairo_context);
    cairo_surface_destroy(cairo_surface);
}

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
                 uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_handle_configure,
};

static int 
create_surface_and_display_image(struct screenshooter_output *output, struct screenshooter_app *app)
{
    app->surface.reference_image_name = "weston-verify-wb-conn-reference.png";
    int ret = 0;

    app->surface.wl_surface = wl_compositor_create_surface(app->wl_compositor);
    if (NULL == app->surface.wl_surface) {
        printf("Error: wl_compositor_create_surface failed.\n");
        return 1;
    } else {
        app->surface.xdg_surface =
            xdg_wm_base_get_xdg_surface(app->wm_base,
                            app->surface.wl_surface);
        xdg_surface_add_listener(app->surface.xdg_surface,
                     &xdg_surface_listener, app);
        assert(app->surface.xdg_surface);
        app->surface.xdg_toplevel =
                         xdg_surface_get_toplevel(app->surface.xdg_surface);
        xdg_toplevel_set_fullscreen(app->surface.xdg_toplevel,
                                    output->wl_output);
        wl_surface_commit(app->surface.wl_surface);
        wl_display_dispatch(app->wl_display);
        wl_display_roundtrip(app->wl_display);
        /* create surface with specific image */
        create_image(app->surface.reference_image_name,
                 output->buffer_width, output->buffer_height);
        if (draw_image(app, app->surface.reference_image_name))
            ret = 1;
    }
    return ret;
}

static void
save_to_image(pixman_image_t *shot, char ** output_image_fullname)
{
    FILE *fp;
    cairo_surface_t *cairo_surface;

    assert(shot);
    cairo_surface =
        cairo_image_surface_create_for_data((void *)pixman_image_get_data(shot),
                                            CAIRO_FORMAT_ARGB32,
                                            pixman_image_get_width(shot),
                                            pixman_image_get_height(shot),
                                            pixman_image_get_stride(shot));

    fp = file_create_dated(getenv("XDG_PICTURES_DIR"), "weston-verify-writeback-connector-",
                   ".png", *output_image_fullname, PATH_MAX);
    if (fp) {
        fclose (fp);
        cairo_surface_write_to_png(cairo_surface, *output_image_fullname);
    }
    cairo_surface_destroy(cairo_surface);
}

static bool
compare_images(const char* shot, const char* ref) {
    bool match = true;
    FILE* shot_fd;
    FILE* ref_fd;
    png_structp shot_png_ptr;
    png_structp ref_png_ptr;
    png_infop shot_info_ptr;
    png_infop ref_info_ptr;
    png_bytep shot_row;
    png_bytep ref_row;

    shot_fd = fopen(shot, "rb");
    if (!shot_fd) {
        printf("Failed to open file: %s\n", shot);
        return false;
    }

    ref_fd = fopen(ref, "rb");
    if (!ref_fd) {
        printf("Failed to open file: %s\n", ref);
        fclose(shot_fd);
        return false;
    }

    shot_png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    ref_png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

    if (!shot_png_ptr || !ref_png_ptr) {
        printf("Failed to initialize libpng structures.\n");
        match = false;
        goto close_fd;
    }

    /* Create libpng info structures */
    shot_info_ptr = png_create_info_struct(shot_png_ptr);
    ref_info_ptr = png_create_info_struct(ref_png_ptr);

    if (!shot_info_ptr || !ref_info_ptr) {
        printf("Failed to initialize libpng structures.\n");
        match = false;
        goto clear_png_resource;
    }

    /* Set file pointer to png struct */
    png_init_io(shot_png_ptr, shot_fd);
    png_init_io(ref_png_ptr, ref_fd);

    /* Read image information */
    png_read_info(shot_png_ptr, shot_info_ptr);
    png_read_info(ref_png_ptr, ref_info_ptr);

    /* Get image dimensions and color type */
    int shot_width = png_get_image_width(shot_png_ptr, shot_info_ptr);
    int shot_height = png_get_image_height(shot_png_ptr, shot_info_ptr);
    int shot_color_type = png_get_color_type(shot_png_ptr, shot_info_ptr);

    int ref_width = png_get_image_width(ref_png_ptr, ref_info_ptr);
    int ref_height = png_get_image_height(ref_png_ptr, ref_info_ptr);
    int ref_color_type = png_get_color_type(ref_png_ptr, ref_info_ptr);

    /* compare width, height, color type */
    if ((shot_width != ref_width)
        || (shot_height != ref_height)
        || (shot_color_type != ref_color_type)) {
        match = false;
        goto clear_png_resource;
    }

    /* Calculate the number of bytes per pixel */
    int bpp = png_get_rowbytes(shot_png_ptr, shot_info_ptr) / ref_width;
    shot_row = (png_bytep)malloc(bpp * shot_width);
    ref_row = (png_bytep)malloc(bpp * ref_width);

    /* Iterate through each row and compare pixel values */
    int row;
    for (row = 0; row < ref_height; row++) {
        png_read_row(shot_png_ptr, shot_row, NULL);
        png_read_row(ref_png_ptr, ref_row, NULL);
        if (memcmp(shot_row, ref_row, bpp * ref_width) != 0) {
            match = false;
            goto free_row;
        }
    }

free_row:
    free(shot_row);
    free(ref_row);
clear_png_resource:
    png_destroy_read_struct(&shot_png_ptr, &shot_info_ptr, NULL);
    png_destroy_read_struct(&ref_png_ptr, &ref_info_ptr, NULL);
close_fd:
    fclose(shot_fd);
    fclose(ref_fd);
    return match;
}

static bool
verify_image(struct screenshooter_output* output)
{
    bool match = false;
    pixman_image_t * shot = output->buffer->image;
    char *output_fullname = output->output_image_fullname;

    if (shot) {
        save_to_image(shot, &output_fullname);
        match = compare_images(output->output_image_fullname,
                     output->app->surface.reference_image_name);
        printf("Verify surface content: %s\n", match ? "PASSED" : "FAILED");
    } else {
        printf("No reference image: FAIL\n");
    }

    if (!match) {
        printf("Shot and ref image are saved to png files\n");
    } else {
        remove(output->output_image_fullname);
        remove(output->app->surface.reference_image_name);
    }

    return match;
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
           "\n\t\t0 to generate screenshot using SHM buffer, "
           "\n\t\t1 to generate screenshot using DMA buffer (default)\n"
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
    struct screenshooter_app app = {};
    int c, option_index;
    char *drm_device_node = DRM_RENDER_NODE;
    app.buffer_type = DMA_BUFFER;

    static struct option long_options[] = {
        {"buffer-type", required_argument, 0,  'b' },
        {"drm-render-node", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "hb:d:",
            long_options, &option_index)) != -1) {
        switch(c) {
        case 'b':
            app.buffer_type = check_arg(optarg);
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
    app.wl_display = display;

    app.registry = wl_display_get_registry(display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);

    wl_display_dispatch(app.wl_display);

    /* Process wl_registry advertisements */
    wl_display_roundtrip(display);

    if (!app.capture_factory) {
        fprintf(stderr, "Error: display does not support weston_capture_v1\n");
        return -1;
    }
    if ((app.buffer_type == DMA_BUFFER) && (app.dmabuf) && (setup_gbm(&app, drm_device_node))) {
        printf("Warn: failed to set up gbm device => Try to use shm buffer!\n");
        app.buffer_type = SHM_BUFFER;
    } else if ((app.buffer_type == DMA_BUFFER) && (!app.dmabuf)) {
        printf("Warn: display does not support dma buffer => Try to use shm buffer!\n");
        app.buffer_type = SHM_BUFFER;
    }

    if ((app.buffer_type == SHM_BUFFER) && (!app.shm)) {
        fprintf(stderr, "Error: display does not support wl_shm\n");
        return -1;
    }

    /* Process initial events for wl_output and weston_capture_source_v1 */
    wl_display_roundtrip(display);

    do {
        app.retry = false;

        wl_list_for_each(output, &app.output_list, link) {
            if (!output->buffer_width || !output->buffer_height || !output->fmt) {
                printf("Error: Writeback source is not supported\n");
                break;
            }
            app.waitcount = 0;
            /* create surface */
            if (create_surface_and_display_image(output, &app)) {
                printf("Failed when create surface and display image. Try with another output\n");
                continue;
            }
            screenshooter_output_capture(output);
            while (app.waitcount > 0 && !app.failed) {
                if (wl_display_dispatch(display) < 0)
                    app.failed = true;
                assert(app.waitcount >= 0);
            }
            if(!app.failed) {
                assert(output->buffer->image);
                if(verify_image(output)) {
                    /* dont need to verify at another output */
                    break;
                }
            }
        }
    } while (app.retry && !app.failed);

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
