/*
 * Copyright © 2023 Collabora, Ltd.
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

#include <string.h>

#include <libweston/libweston.h>

#include "shared/helpers.h"

WL_EXPORT const char *
weston_fixed_compression_rate_to_str(enum weston_fixed_compression_rate rate)
{
	switch (rate) {
	case WESTON_COMPRESSION_NONE:
		return "none";
	case WESTON_COMPRESSION_DEFAULT:
		return "default (driver-defined)";
	case WESTON_COMPRESSION_1BPC:
		return "1bpc";
	case WESTON_COMPRESSION_2BPC:
		return "2bpc";
	case WESTON_COMPRESSION_3BPC:
		return "3bpc";
	case WESTON_COMPRESSION_4BPC:
		return "4bpc";
	case WESTON_COMPRESSION_5BPC:
		return "5bpc";
	case WESTON_COMPRESSION_6BPC:
		return "6bpc";
	case WESTON_COMPRESSION_7BPC:
		return "7bpc";
	case WESTON_COMPRESSION_8BPC:
		return "8bpc";
	case WESTON_COMPRESSION_9BPC:
		return "9bpc";
	case WESTON_COMPRESSION_10BPC:
		return "10bpc";
	case WESTON_COMPRESSION_11BPC:
		return "11bpc";
	case WESTON_COMPRESSION_12BPC:
		return "12+bpc";
	default:
		unreachable("invalid enum");
	}
}
