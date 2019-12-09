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

#include <stdint.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <libweston/libweston.h>
#include <libweston/pixel-formats.h>
#include <libweston/linux-dmabuf.h>
#include "shared/helpers.h"
#include "transmitter-internal.h"
#include "linux-dmabuf.h"

static void
drm_fb_destroy(struct drm_fb *fb)
{
	if (fb->fb_id != 0)
		drmModeRmFB(fb->fd, fb->fb_id);
	weston_buffer_reference(&fb->buffer_ref, NULL);
	weston_buffer_release_reference(&fb->buffer_release_ref, NULL);
	free(fb);
}

static void
drm_fb_destroy_gbm(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;

	assert(fb->type == BUFFER_GBM_SURFACE || fb->type == BUFFER_CLIENT ||
	       fb->type == BUFFER_CURSOR);
	drm_fb_destroy(fb);
}

static int
drm_fb_addfb(struct transmitter_backend *b, struct drm_fb *fb)
{
	int ret = -EINVAL;
#ifdef HAVE_DRM_ADDFB2_MODIFIERS
	uint64_t mods[4] = {};
	size_t i;
#endif

	/* If we have a modifier set, we must only use the WithModifiers
	 * entrypoint; we cannot import it through legacy ioctls. */
	if (b->fb_modifiers && fb->modifier != DRM_FORMAT_MOD_INVALID) {
		/* KMS demands that if a modifier is set, it must be the same
		 * for all planes. */
#ifdef HAVE_DRM_ADDFB2_MODIFIERS
		for (i = 0; i < ARRAY_LENGTH(mods) && fb->handles[i]; i++)
			mods[i] = fb->modifier;
		ret = drmModeAddFB2WithModifiers(fb->fd, fb->width, fb->height,
						 fb->format->format,
						 fb->handles, fb->strides,
						 fb->offsets, mods, &fb->fb_id,
						 DRM_MODE_FB_MODIFIERS);
#endif
		return ret;
	}

	ret = drmModeAddFB2(fb->fd, fb->width, fb->height, fb->format->format,
			    fb->handles, fb->strides, fb->offsets, &fb->fb_id,
			    0);
	if (ret == 0)
		return 0;

	/* Legacy AddFB can't always infer the format from depth/bpp alone, so
	 * check if our format is one of the lucky ones. */
	if (!fb->format->depth || !fb->format->bpp)
		return ret;

	/* Cannot fall back to AddFB for multi-planar formats either. */
	if (fb->handles[1] || fb->handles[2] || fb->handles[3])
		return ret;

	ret = drmModeAddFB(fb->fd, fb->width, fb->height,
			   fb->format->depth, fb->format->bpp,
			   fb->strides[0], fb->handles[0], &fb->fb_id);
	return ret;
}

struct drm_fb *
drm_fb_ref(struct drm_fb *fb)
{
	fb->refcnt++;
	return fb;
}

static void
drm_fb_destroy_dmabuf(struct drm_fb *fb)
{
	/* We deliberately do not close the GEM handles here; GBM manages
	 * their lifetime through the BO. */
	if (fb->bo)
		gbm_bo_destroy(fb->bo);
	drm_fb_destroy(fb);
}

static struct drm_fb *
drm_fb_get_from_dmabuf(struct linux_dmabuf_buffer *dmabuf,
		       struct transmitter_backend *backend, bool is_opaque)
{
#ifdef HAVE_GBM_FD_IMPORT
	struct drm_fb *fb;
	struct gbm_import_fd_data import_legacy = {
		.width = dmabuf->attributes.width,
		.height = dmabuf->attributes.height,
		.format = dmabuf->attributes.format,
		.stride = dmabuf->attributes.stride[0],
		.fd = dmabuf->attributes.fd[0],
	};
	struct gbm_import_fd_modifier_data import_mod = {
		.width = dmabuf->attributes.width,
		.height = dmabuf->attributes.height,
		.format = dmabuf->attributes.format,
		.num_fds = dmabuf->attributes.n_planes,
		.modifier = dmabuf->attributes.modifier[0],
	};
	int i;

	/* XXX: TODO:
	 *
	 * Currently the buffer is rejected if any dmabuf attribute
	 * flag is set.  This keeps us from passing an inverted /
	 * interlaced / bottom-first buffer (or any other type that may
	 * be added in the future) through to an overlay.  Ultimately,
	 * these types of buffers should be handled through buffer
	 * transforms and not as spot-checks requiring specific
	 * knowledge. */
	if (dmabuf->attributes.flags)
		return NULL;

	fb = zalloc(sizeof *fb);
	if (fb == NULL)
		return NULL;

	fb->refcnt = 1;
	fb->type = BUFFER_DMABUF;

	static_assert(ARRAY_LENGTH(import_mod.fds) ==
		      ARRAY_LENGTH(dmabuf->attributes.fd),
		      "GBM and linux_dmabuf FD size must match");
	static_assert(sizeof(import_mod.fds) == sizeof(dmabuf->attributes.fd),
		      "GBM and linux_dmabuf FD size must match");
	memcpy(import_mod.fds, dmabuf->attributes.fd, sizeof(import_mod.fds));

	static_assert(ARRAY_LENGTH(import_mod.strides) ==
		      ARRAY_LENGTH(dmabuf->attributes.stride),
		      "GBM and linux_dmabuf stride size must match");
	static_assert(sizeof(import_mod.strides) ==
		      sizeof(dmabuf->attributes.stride),
		      "GBM and linux_dmabuf stride size must match");
	memcpy(import_mod.strides, dmabuf->attributes.stride,
	       sizeof(import_mod.strides));

	static_assert(ARRAY_LENGTH(import_mod.offsets) ==
		      ARRAY_LENGTH(dmabuf->attributes.offset),
		      "GBM and linux_dmabuf offset size must match");
	static_assert(sizeof(import_mod.offsets) ==
		      sizeof(dmabuf->attributes.offset),
		      "GBM and linux_dmabuf offset size must match");
	memcpy(import_mod.offsets, dmabuf->attributes.offset,
	       sizeof(import_mod.offsets));

	/* The legacy FD-import path does not allow us to supply modifiers,
	 * multiple planes, or buffer offsets. */
	if (dmabuf->attributes.modifier[0] != DRM_FORMAT_MOD_INVALID ||
	    import_mod.num_fds > 1 ||
	    import_mod.offsets[0] > 0) {
		fb->bo = gbm_bo_import(backend->gbm, GBM_BO_IMPORT_FD_MODIFIER,
				       &import_mod,
				       GBM_BO_USE_SCANOUT);
	} else {
		fb->bo = gbm_bo_import(backend->gbm, GBM_BO_IMPORT_FD,
				       &import_legacy,
				       GBM_BO_USE_SCANOUT);
	}

	if (!fb->bo)
		goto err_free;

	fb->width = dmabuf->attributes.width;
	fb->height = dmabuf->attributes.height;
	fb->modifier = dmabuf->attributes.modifier[0];
	fb->size = 0;
	fb->fd = backend->drm.fd;

	static_assert(ARRAY_LENGTH(fb->strides) ==
		      ARRAY_LENGTH(dmabuf->attributes.stride),
		      "drm_fb and dmabuf stride size must match");
	static_assert(sizeof(fb->strides) == sizeof(dmabuf->attributes.stride),
		      "drm_fb and dmabuf stride size must match");
	memcpy(fb->strides, dmabuf->attributes.stride, sizeof(fb->strides));
	static_assert(ARRAY_LENGTH(fb->offsets) ==
		      ARRAY_LENGTH(dmabuf->attributes.offset),
		      "drm_fb and dmabuf offset size must match");
	static_assert(sizeof(fb->offsets) == sizeof(dmabuf->attributes.offset),
		      "drm_fb and dmabuf offset size must match");
	memcpy(fb->offsets, dmabuf->attributes.offset, sizeof(fb->offsets));

	fb->format = pixel_format_get_info(dmabuf->attributes.format);
	if (!fb->format) {
		weston_log("couldn't look up format info for 0x%lx\n",
			   (unsigned long) dmabuf->attributes.format);
		goto err_free;
	}

	if (is_opaque)
		fb->format = pixel_format_get_opaque_substitute(fb->format);

	if (backend->min_width > fb->width ||
	    fb->width > backend->max_width ||
	    backend->min_height > fb->height ||
	    fb->height > backend->max_height) {
		weston_log("bo geometry out of bounds\n");
		goto err_free;
	}

	fb->num_planes = dmabuf->attributes.n_planes;
	for (i = 0; i < dmabuf->attributes.n_planes; i++) {
		union gbm_bo_handle handle;

		handle = gbm_bo_get_handle_for_plane(fb->bo, i);
		if (handle.s32 == -1)
			goto err_free;
		fb->handles[i] = handle.u32;
	}

	if (drm_fb_addfb(backend, fb) != 0)
		goto err_free;

	return fb;

err_free:
	drm_fb_destroy_dmabuf(fb);
#endif
	return NULL;
}

struct drm_fb *
drm_fb_get_from_bo(struct gbm_bo *bo, struct transmitter_backend *backend,
		   bool is_opaque, enum drm_fb_type type)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
#ifdef HAVE_GBM_MODIFIERS
	int i;
#endif

	if (fb) {
		assert(fb->type == type);
		return drm_fb_ref(fb);
	}

	fb = zalloc(sizeof *fb);
	if (fb == NULL)
		return NULL;

	fb->type = type;
	fb->refcnt = 1;
	fb->bo = bo;
	fb->fd = backend->drm.fd;

	fb->width = gbm_bo_get_width(bo);
	fb->height = gbm_bo_get_height(bo);
	fb->format = pixel_format_get_info(gbm_bo_get_format(bo));
	fb->size = 0;

#ifdef HAVE_GBM_MODIFIERS
	fb->modifier = gbm_bo_get_modifier(bo);
	fb->num_planes = gbm_bo_get_plane_count(bo);
	for (i = 0; i < fb->num_planes; i++) {
		fb->strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		fb->handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
		fb->offsets[i] = gbm_bo_get_offset(bo, i);
	}
#else
	fb->num_planes = 1;
	fb->strides[0] = gbm_bo_get_stride(bo);
	fb->handles[0] = gbm_bo_get_handle(bo).u32;
	fb->modifier = DRM_FORMAT_MOD_INVALID;
#endif

	if (!fb->format) {
		weston_log("couldn't look up format 0x%lx\n",
			   (unsigned long) gbm_bo_get_format(bo));
		goto err_free;
	}

	/* We can scanout an ARGB buffer if the surface's opaque region covers
	 * the whole output, but we have to use XRGB as the KMS format code. */
	if (is_opaque)
		fb->format = pixel_format_get_opaque_substitute(fb->format);

	if (backend->min_width > fb->width ||
	    fb->width > backend->max_width ||
	    backend->min_height > fb->height ||
	    fb->height > backend->max_height) {
		weston_log("line no:423 bo geometry out of bounds\n");
		goto err_free;
	}

	if (drm_fb_addfb(backend, fb) != 0) {
		if (type == BUFFER_GBM_SURFACE)
			weston_log("failed to create kms fb: %s\n",
				   strerror(errno));
		goto err_free;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_gbm);

	return fb;

err_free:
	free(fb);
	return NULL;
}
