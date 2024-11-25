/*
 * Copyright © 2024 Pengutronix
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

#include "libweston/libweston-internal.h"

struct etna_renderer_display_options {
	struct weston_renderer_options base;
	/** Array of pixel formats acceptable for the base surface */
	const struct pixel_format_info **formats;
	/** The \c formats array length */
	unsigned formats_count;
};

struct etna_renderer_interface {
	/**
	 * Initialize etnaviv renderer
	 *
	 * \param ec The weston_compositor where to initialize.
	 * \return 0 on success, -1 on failure.
	 */
	int (*display_create)(struct weston_compositor *ec);

	int (*output_create)(struct weston_output *output);
	void (*output_destroy)(struct weston_output *output);

	struct weston_renderbuffer *
	(*renderbuffer_from_dmabuf)(struct weston_output *output, int dmabuf_fd,
				    const struct pixel_format_info *format,
				    int32_t width, int32_t height,
				    int32_t stride);

	struct weston_renderbuffer *
	(*create_renderbuffer)(struct weston_output *output,
			       const struct pixel_format_info *format,
			       int32_t width, int32_t height);
};
