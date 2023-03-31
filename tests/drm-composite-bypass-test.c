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

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include <libudev.h>

#include <wayland-client.h>
#include "shared/helpers.h"
#include "shared/platform.h"
#include <libweston/zalloc.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "weston-direct-display-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"

#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"
#include "weston-output-capture-client-protocol.h"

#define BUFFER_DRM_FORMAT DRM_FORMAT_XRGB8888
#define BUFFER_DRM_FORMAT_MODIFIER DRM_FORMAT_MOD_LINEAR

#define NUM_BUFFERS 3

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct weston_direct_display_v1 *direct_display;
	bool drm_format_modifier_supported;
	int alloc_dev_fd;
};

struct hwbuffer {
	struct display *display;
	struct wl_buffer *buffer;
	void *data;
	int busy;
	int width;
	int height;
	int dmabuf_fd;
	uint32_t stride;
	uint32_t offset;
	uint32_t size;
	uint32_t handle;
	struct zwp_linux_buffer_release_v1 *buffer_release;
};

struct window {
	struct display *display;
	struct client *client;
	int width, height;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct hwbuffer buffers[NUM_BUFFERS];
	struct wl_callback *callback;
	struct buffer *screenshot;
	bool wait_for_configure;
	bool initialized;
	int n_redraws;
};

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct hwbuffer *mybuf = data;
	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static void
buffer_free(struct hwbuffer *buf)
{
	if (buf->buffer_release)
		zwp_linux_buffer_release_v1_destroy(buf->buffer_release);
	if (buf->buffer)
		wl_buffer_destroy(buf->buffer);
}

static void
create_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct hwbuffer *buffer = data;

	buffer->buffer = new_buffer;
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	zwp_linux_buffer_params_v1_destroy(params);
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct hwbuffer *buffer = data;

	buffer->buffer = NULL;
	zwp_linux_buffer_params_v1_destroy(params);

	assert(0 && "error: zwp_linux_buffer_params.create failed");
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	create_succeeded,
	create_failed
};

static void
create_dumb_buffer(struct display *display, struct hwbuffer *buffer)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buffer->width;
	creq.height = buffer->height;
	creq.bpp = 32;
	ret = drmIoctl(display->alloc_dev_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	assert(!ret && "error: could not create dumb buffer");

	buffer->handle = creq.handle;
	buffer->stride = creq.pitch;
	buffer->size = creq.size;

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buffer->handle;
	ret = drmIoctl(display->alloc_dev_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	assert(!ret && "error: could not prepare dumb buffer for memory mapping");

	/* map buffer in memory so we can paint it */
	buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			    display->alloc_dev_fd, mreq.offset);
	assert(buffer->data != MAP_FAILED && "error: failed to mmap dumb buffer");
}

static void
create_dmabuf_buffer(struct display *display, struct hwbuffer *buffer, int width, int height)
{
	static uint32_t flags = 0;
	struct zwp_linux_buffer_params_v1 *params;
	int ret;

	buffer->display = display;
	buffer->width = width;
	buffer->height = height;

	create_dumb_buffer(display, buffer);
	ret = drmPrimeHandleToFD(display->alloc_dev_fd, buffer->handle, 0, &buffer->dmabuf_fd);
	assert(!ret && "error: could not create dmabuf for dumb buffer");

	params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);

	/* enable direct scanout; the dmabuf won't be imported by the GPU but
	 * directly scanned-out by the display controller */
	weston_direct_display_v1_enable(display->direct_display, params);

	zwp_linux_buffer_params_v1_add(params, buffer->dmabuf_fd, 0, 0,
				       buffer->stride,
				       BUFFER_DRM_FORMAT_MODIFIER >> 32,
				       BUFFER_DRM_FORMAT_MODIFIER & 0xffffffff);
	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);
	zwp_linux_buffer_params_v1_create(params, buffer->width, buffer->height,
					  BUFFER_DRM_FORMAT, flags);
}

static struct hwbuffer *
window_next_buffer(struct window *window)
{
	for (int i = 0; i < NUM_BUFFERS; i++)
		if (!window->buffers[i].busy)
			return &window->buffers[i];
	return NULL;
}

/* check if every pixel of the image is green */
static bool
check_pixels_are_green(pixman_image_t *image)
{
	int width, height;
	int stride;
	int x, y;
	uint32_t r, g, b;
	uint32_t *pixels;
	uint32_t *pixel;
	uint32_t green_pixel;
	pixman_format_code_t fmt;

	fmt = pixman_image_get_format(image);
	width = pixman_image_get_width(image);
	height = pixman_image_get_height(image);
	stride = pixman_image_get_stride(image);
	pixels = pixman_image_get_data(image);

	assert(PIXMAN_FORMAT_BPP(fmt) == 32);

	/* green color */
	r = 0; g = 255; b = 0;

	green_pixel = (255U << 24) | (r << 16) | (g << 8) | b;

	for (x = 0; x < width; x++) {
		for (y = 0; y < height; y++) {
			pixel = pixels + (y * stride / 4) + x;
			if (*pixel != green_pixel)
				return false;
		}
	}
	return true;
}

/* paint the buffer of green */
static void
paint_buffer_green(struct window *window, struct hwbuffer *buffer)
{
	int width, height;
	int stride;
	int x, y;
	uint32_t r, g, b;
	uint32_t *pixels;
	uint32_t *pixel;
	pixman_image_t *image;
	pixman_format_code_t fmt;

	image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
					 buffer->width, buffer->height,
					 buffer->data, buffer->stride);

	fmt = pixman_image_get_format(image);
	width = pixman_image_get_width(image);
	height = pixman_image_get_height(image);
	stride = pixman_image_get_stride(image);
	pixels = pixman_image_get_data(image);

	assert(PIXMAN_FORMAT_BPP(fmt) == 32);

	/* green color */
	r = 0; g = 255; b = 0;

	for (x = 0; x < width; x++) {
		for (y = 0; y < height; y++) {
			pixel = pixels + (y * stride / 4) + x;
			*pixel =  (255U << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct hwbuffer *buffer;

	/* get a dmabuf that is not busy */
	buffer = window_next_buffer(window);
	assert(buffer && "error: all buffers are busy");

	paint_buffer_green(window, buffer);

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);
	buffer->busy = 1;

	/* we already performed 5 pageflips, now take a screenshot */
	if (++window->n_redraws >= 5) {
		testlog("Taking a screenshot\n");
		window->screenshot = client_capture_output(window->client,
							   window->client->output,
							   WESTON_CAPTURE_V1_SOURCE_WRITEBACK);
	}
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
			     uint32_t serial)
{
	struct window *window = data;

	xdg_surface_ack_configure(surface, serial);

	if (window->initialized && window->wait_for_configure)
		redraw(window, NULL, 0);

	window->wait_for_configure = false;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_handle_configure,
};

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *toplevel,
			      int32_t width, int32_t height,
			      struct wl_array *states)
{
}

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	assert(0 && "error: could not turn surface into XDG toplevel");
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	xdg_toplevel_handle_configure,
	xdg_toplevel_handle_close,
};

static void
destroy_window(struct window *window)
{
	buffer_destroy(window->screenshot);

	if (window->callback)
		wl_callback_destroy(window->callback);

	for (int i = 0; i < NUM_BUFFERS; i++)
		if (window->buffers[i].buffer)
			buffer_free(&window->buffers[i]);

	if (window->xdg_toplevel)
		xdg_toplevel_destroy(window->xdg_toplevel);
	if (window->xdg_surface)
		xdg_surface_destroy(window->xdg_surface);

	wl_surface_destroy(window->surface);
	free(window);
}

static struct window *
create_window(struct display *display, int width, int height, struct client *client)
{
	struct window *window;

	window = zalloc(sizeof *window);
	assert(window && "error: failed to allocate memory for window");

	window->display = display;
	window->client = client;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);

	window->xdg_surface = xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
	assert(window->xdg_surface);
	xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, window);

	window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
	assert(window->xdg_toplevel);
	xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window);

	window->wait_for_configure = true;
	wl_surface_commit(window->surface);

	/* create our dmabufs */
	for (int i = 0; i < NUM_BUFFERS; i++)
		create_dmabuf_buffer(display, &window->buffers[i], width, height);

	/* retrieve dmabuf objects */
	wl_display_roundtrip(display->display);

	window->initialized = true;

	return window;
}

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct display *display = data;

	/**
	 * TODO: also check the modifier, not only the format.
	 *
	 * This is a hack to be able to have this test on our CI.
	 *
	 * VGEM -> allows us to create LINEAR dumb buffers
	 * VKMS -> supports LINEAR buffers and the explicit modifier API
	 * renderer (llvmpipe?) -> only supports implicit modifiers
	 *
	 * When we bind to linux dma-buf on the client, we received the
	 * supported format/modifier pairs supported by the renderer. In this
	 * case, MOD_INVALID for whatever format.
	 *
	 * If we respect that and create a dma-buf buffer for the VGEM dumb
	 * buffer (which is linear) with MOD_INVALID, Weston won't be able to
	 * pass that directly to the VKMS planes. Weston correctly refuses to
	 * give a MOD_INVALID buffer to the KMS device, because it does not know
	 * if the buffer layout considered by the allocator is the same
	 * considered by the KMS device.
	 *
	 * So the fix would be adding support for explicit modifiers on
	 * llvmpipe.
	 **/
	if (format == BUFFER_DRM_FORMAT)
		display->drm_format_modifier_supported = true;
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	/* deprecated */
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifiers
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	xdg_wm_base_ping,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		d->wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		if (version < 3)
			return;
		d->dmabuf = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
	} else if (strcmp(interface, "weston_direct_display_v1") == 0) {
		d->direct_display = wl_registry_bind(registry, id, &weston_direct_display_v1_interface, 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
destroy_display(struct display *display)
{
	if (display->dmabuf)
		zwp_linux_dmabuf_v1_destroy(display->dmabuf);

	if (display->wm_base)
		xdg_wm_base_destroy(display->wm_base);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	if (display->registry)
		wl_registry_destroy(display->registry);

	if (display->display) {
		wl_display_flush(display->display);
		wl_display_disconnect(display->display);
	}

	free(display);
}

static struct display *
create_display(int alloc_dev_fd)
{
	struct display *display = NULL;

	display = zalloc(sizeof *display);
	assert(display && "error: failed to allocate memory for window");

	display->alloc_dev_fd = alloc_dev_fd;

	display->display = wl_display_connect(NULL);
	assert(display->display && "error: could not connect to display");

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry, &registry_listener, display);

	wl_display_roundtrip(display->display);
	assert(display->dmabuf && "error: dmabuf extension is not supported");

	wl_display_roundtrip(display->display);
	assert(display->drm_format_modifier_supported && "error: DRM format or DRM format modifier not available");

	assert(display->direct_display && "error: direct display extension is not supported");
	assert(display->wm_base && "error: xdg shell is not supported");

	return display;
}

static int
open_drm_device(const char *name)
{
	struct udev *udev;
	struct udev_device *udev_device;
	const char *node;
	int fd;

	udev = udev_new();
	assert(udev);

	udev_device = udev_device_new_from_subsystem_sysname(udev, "drm", name);
	if (!udev_device)
		goto err;

	node = udev_device_get_devnode(udev_device);
	if (!node)
		goto err_dev;

	fd = open(node, O_RDWR | O_CLOEXEC);

	udev_device_unref(udev_device);
	udev_unref(udev);

	return fd;

err_dev:
	udev_device_unref(udev_device);
err:
	udev_unref(udev);
	return -1;
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.shell = SHELL_TEST_DESKTOP;
	setup.backend = WESTON_BACKEND_DRM;
	setup.renderer = WESTON_RENDERER_GL;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

/** Composite-bypass test
 *
 * Creates buffers that can be given to the primary plane of the graphics card
 * so they can be displayed directly, bypassing the renderers. In order to
 * achieve this, we need hardware buffers. As we want to run this in our CI
 * using VKMS, we use VGEM in order to create "hardware buffers" in the absence
 * of a real graphics card. Then these buffers can be exported to VKMS with the
 * linux-dmabuf extension.
 *
 * As it skips rendering, it depends on the writeback screenshooter in order to
 * validate if everything went well. So you need to run this test with
 * WESTON_TEST_SUITE_DRM_DEVICE set to a card that supports writeback
 * connectors (VKMS supports that).
 *
 * If you want to run this test locally, the easiest way is to set
 * WESTON_TEST_SUITE_DRM_DEVICE to VKMS and also WESTON_TEST_SUITE_ALLOC_DEVICE
 * to VGEM.
 *
 * Also, we must assure that the resolution of our buffers is the same of the
 * mode selected for the output. This way we assure that Weston only needs to
 * display what is in our buffer. If the buffer is smaller than the output
 * dimensions, Weston needs to display a black background behind it. If the
 * buffer is bigger, it simply won't fit the primary plane. These are reasons to
 * make Weston use overlay planes (if the card has support) or fallback to
 * composition. It is obvious that the idea of the test is to avoid the
 * renderers, but you may ask yourself why do we need to avoid the usage of
 * overlay planes as well. The reason is that not every graphics card has
 * support for overlay planes, including some older versions of VKMS. We want
 * this test to pass even when we have only a primary plane available.
 */
TEST(drm_composite_bypass)
{
	const char *alloc_dev_node;
	int alloc_dev_fd;
	struct display *display;
	struct client *client;
	struct window *window;
	int width, height;
	bool pixels_are_green;
	int ret;

	/* This client is useful only to take screenshots of the output and to
	 * have access to the output dimensions, but we are not going to need it
	 * for anything else. */
	client = create_client();
	assert(client);

	/* Dimensions of our buffers must be the same of the output. Please take
	 * a look at the header of this function if you want to understand the
	 * reason why. */
	width = client->output->width;
	height = client->output->height;

	/* Get allocation device node and then open it. */
	alloc_dev_node = getenv("WESTON_TEST_SUITE_ALLOC_DEVICE");
	if (!alloc_dev_node)
		testlog("WESTON_TEST_SUITE_ALLOC_DEVICE not set. Please set " \
			"it accordingly to run this test\n");
	assert(alloc_dev_node != NULL && "error: could not find allocation device node");
	alloc_dev_fd = open_drm_device(alloc_dev_node);
	assert(alloc_dev_fd >= 0 && "error: could not open allocator device");

	/* Create display and window. */
	display = create_display(alloc_dev_fd);
	window = create_window(display, width, height, client);
	assert(window->wait_for_configure == false && "error: could not create XDG surface");

	/* Hide the cursor */
	wl_pointer_set_cursor(client->input->pointer->wl_pointer, 0, NULL, 0, 0);

	/* This will perform some pageflips and then take a screenshot. */
	redraw(window, NULL, 0);
	do {
		ret = wl_display_dispatch(display->display);
	} while (ret != -1 && window->n_redraws < 5);

	assert(window->screenshot);

	/* Check if all the pixels of the screenshot are green, as we've colored
	 * the buffer in redraw(). We can't use a reference image here, as we
	 * don't know the mode that the output is using, and so we don't have a
	 * hardcoded resolution for the screenshot. It depends on the graphics
	 * card being used and the modes exposed. In the future we can have more
	 * robust tests that take this into consideration. */
	pixels_are_green = check_pixels_are_green(window->screenshot->image);
	testlog("Screenshot %s what we were expecting\n",
		pixels_are_green ? "matches" : "does not match");

	destroy_window(window);
	destroy_display(display);
	close(alloc_dev_fd);

	assert(pixels_are_green);
}
