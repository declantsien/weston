/*
 * Copyright © 2020 Collabora, Ltd.
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

#include <string.h>
#include <stdint.h>

#include "shared/helpers.h"
#include <libweston/transform.h>
#include <wayland-client.h>
#include <wayland-util.h>

static const struct { const char *name; uint32_t token; } transforms[] = {
	{ "normal",             WL_OUTPUT_TRANSFORM_NORMAL },
	{ "rotate-90",          WL_OUTPUT_TRANSFORM_90 },
	{ "rotate-180",         WL_OUTPUT_TRANSFORM_180 },
	{ "rotate-270",         WL_OUTPUT_TRANSFORM_270 },
	{ "flipped",            WL_OUTPUT_TRANSFORM_FLIPPED },
	{ "flipped-rotate-90",  WL_OUTPUT_TRANSFORM_FLIPPED_90 },
	{ "flipped-rotate-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
	{ "flipped-rotate-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
};

WL_EXPORT int
weston_parse_transform(const char *transform, uint32_t *out)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (strcmp(transforms[i].name, transform) == 0) {
			*out = transforms[i].token;
			return 0;
		}

	*out = WL_OUTPUT_TRANSFORM_NORMAL;
	return -1;
}

WL_EXPORT const char *
weston_transform_to_string(uint32_t output_transform)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (transforms[i].token == output_transform)
			return transforms[i].name;

	return "<illegal value>";
}
