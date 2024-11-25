/*
 * Copyright 2023 Collabora, Ltd.
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

#include "color-representation.h"

#include "shared/string-helpers.h"
#include "shared/weston-assert.h"
#include "shared/xalloc.h"

#include "color-representation-v1-server-protocol.h"

static void
cr_destroy(struct wl_client *client,
	   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
cr_set_alpha_mode(struct wl_client *client,
		  struct wl_resource *resource,
		  uint32_t alpha_mode)
{
	fprintf(stderr, "%s %u\n", __func__, alpha_mode);
}

static void
cr_set_coefficients_and_range(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t coefficients,
			      uint32_t range)
{
	fprintf(stderr, "%s %u %u\n", __func__, coefficients, range);
}

static void
cr_set_chroma_location(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t code_point)
{
	fprintf(stderr, "%s %u\n", __func__, code_point);
}

static const struct wp_color_representation_v1_interface cr_implementation = {
	.destroy = cr_destroy,
	.set_alpha_mode = cr_set_alpha_mode,
	.set_coefficients_and_range = cr_set_coefficients_and_range,
	.set_chroma_location = cr_set_chroma_location,
};

static void
cr_manager_destroy(struct wl_client *client,
                   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
cr_manager_create(struct wl_client *client,
                  struct wl_resource *resource,
                  uint32_t id,
                  struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	resource = wl_resource_create(client,
				      &wp_color_representation_v1_interface,
				      1, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &cr_implementation, surface,
				       NULL);
}

static const struct wp_color_representation_manager_v1_interface
cr_manager_implementation = {
        .destroy = cr_manager_destroy,
	.create = cr_manager_create,
};

static void
bind_color_representation(struct wl_client *client, void *data, uint32_t version,
                          uint32_t id)
{
        struct wl_resource *resource;
        struct weston_compositor *compositor = data;

	resource = wl_resource_create(client,
				      &wp_color_representation_manager_v1_interface,
				      version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &cr_manager_implementation,
				       compositor, NULL);

	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(resource,
										  WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_IDENTITY,
										  WP_COLOR_REPRESENTATION_V1_RANGE_FULL);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(resource,
										  WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_BT709,
										  WP_COLOR_REPRESENTATION_V1_RANGE_LIMITED);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(resource,
										  WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_BT601,
										  WP_COLOR_REPRESENTATION_V1_RANGE_LIMITED);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(resource,
										  WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_BT2020,
										  WP_COLOR_REPRESENTATION_V1_RANGE_LIMITED);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(resource,
										  WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_SMPTE240,
										  WP_COLOR_REPRESENTATION_V1_RANGE_LIMITED);

	wp_color_representation_manager_v1_send_supported_chroma_location(resource, 0);
	wp_color_representation_manager_v1_send_supported_chroma_location(resource, 2);
}

/** Advertise color-management support
 *
 * Calling this initializes the color-management protocol support, so that
 * xx_color_manager_v2_interface will be advertised to clients. Essentially it
 * creates a global. Do not call this function multiple times in the
 * compositor's lifetime. There is no way to deinit explicitly, globals will be
 * reaped when the wl_display gets destroyed.
 *
 * \param compositor The compositor to init for.
 * \return Zero on success, -1 on failure.
 */
int
weston_compositor_enable_color_representation_protocol(struct weston_compositor *compositor)
{
        uint32_t version = 1;

        if (!wl_global_create(compositor->wl_display,
                              &wp_color_representation_manager_v1_interface,
                              version, compositor, bind_color_representation))
                return -1;

        return 0;
}
