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

#include "config.h"

#include <etnaviv_drmif.h>
#include <pixman.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include "etna-renderer.h"
#include "libweston/backend.h"
#include "libweston/libweston.h"
#include "libweston/libweston-internal.h"
#include "libweston/linux-dmabuf.h"
#include "libweston/pixel-formats.h"
#include "output-capture.h"
#include "shared/helpers.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/xalloc.h"

#include "common.xml.h"
#include "cmdstream.xml.h"
#include "state_2d.xml.h"
#include "state.xml.h"

struct etna_renderer {
	struct weston_renderer base;
	struct wl_signal destroy_signal;

	int drm_fd;
	struct etna_device *etna_dev;
	struct etna_gpu *etna_gpu;
	struct etna_pipe *etna_pipe;
	struct etna_cmd_stream *etna_stream;

	struct weston_drm_format_array supported_formats;
};

struct etna_renderbuffer {
	struct weston_renderbuffer base;

	struct etna_bo *bo;
	int32_t width;
	int32_t height;
	int32_t stride;
	int de_format;

	struct wl_list link;

	const struct pixel_format_info *format;
};

struct etna_buffer_state {
	struct wl_listener destroy_listener;

	pixman_region32_t damage;
	bool needs_full_upload;

	uint32_t clear_color;
	struct etna_bo *bo;
	int32_t stride;
	int32_t height;
	int32_t width;
	int de_format;
	int tiled;
};

struct etna_output_state {
	struct wl_list renderbuffer_list;
};

struct etna_surface_state {
	struct weston_surface *surface;

	struct etna_buffer_state *buffer;

	/* These buffer references should really be attached to paint nodes
	 * rather than either buffer or surface state */
	struct weston_buffer_reference buffer_ref;
	struct weston_buffer_release_reference buffer_release_ref;

	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;
};

static inline struct etna_renderbuffer *
to_etna_renderbuffer(struct weston_renderbuffer *renderbuffer)
{
	return container_of(renderbuffer, struct etna_renderbuffer, base);
}

static int etna_drm_to_de_format(uint32_t drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
		return DE_FORMAT_X8R8G8B8;
	case DRM_FORMAT_ARGB8888:
		return DE_FORMAT_A8R8G8B8;
	case DRM_FORMAT_RGB565:
		return DE_FORMAT_R5G6B5;
	default:
		weston_log("unsupported DRM format 0x%x\n", drm_format);
		return -1;
	}
}

static inline struct etna_output_state *
get_output_state(struct weston_output *output)
{
	return (struct etna_output_state *)output->renderer_state;
}

static inline struct etna_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct etna_renderer *)ec->renderer;
}

static void
destroy_buffer_state(struct etna_buffer_state *ebs)
{
	if (ebs->bo)
		etna_bo_del(ebs->bo);

	wl_list_remove(&ebs->destroy_listener.link);
	pixman_region32_fini(&ebs->damage);

	free(ebs);
}

static void
surface_state_destroy(struct etna_surface_state *ess)
{
	wl_list_remove(&ess->surface_destroy_listener.link);
	wl_list_remove(&ess->renderer_destroy_listener.link);

	ess->surface->renderer_state = NULL;

	if (ess->buffer && ess->buffer_ref.buffer->type == WESTON_BUFFER_SHM)
		destroy_buffer_state(ess->buffer);
	ess->buffer = NULL;

	weston_buffer_reference(&ess->buffer_ref, NULL,
				BUFFER_WILL_NOT_BE_ACCESSED);
	weston_buffer_release_reference(&ess->buffer_release_ref, NULL);

	free(ess);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct etna_surface_state *ess;

	ess = container_of(listener, struct etna_surface_state,
			   surface_destroy_listener);

	surface_state_destroy(ess);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct etna_surface_state *ess;

	ess = container_of(listener, struct etna_surface_state,
			   renderer_destroy_listener);

	surface_state_destroy(ess);
}

static int
etna_renderer_create_surface(struct weston_surface *surface)
{
	struct etna_renderer *er = get_renderer(surface->compositor);
	struct etna_surface_state *ess;

	ess = xzalloc(sizeof *ess);

	ess->surface = surface;

	surface->renderer_state = ess;

	ess->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &ess->surface_destroy_listener);

	ess->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&er->destroy_signal,
		      &ess->renderer_destroy_listener);

	return 0;
}

static inline struct etna_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		etna_renderer_create_surface(surface);

	return (struct etna_surface_state *)surface->renderer_state;
}

static inline void
etna_emit_load_state(struct etna_cmd_stream *stream,
		     const uint16_t offset, const uint16_t count)
{
	uint32_t v;

	v = (VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE | VIV_FE_LOAD_STATE_HEADER_OFFSET(offset) |
	    (VIV_FE_LOAD_STATE_HEADER_COUNT(count) & VIV_FE_LOAD_STATE_HEADER_COUNT__MASK));

	etna_cmd_stream_emit(stream, v);
}

static inline void
etna_set_state(struct etna_cmd_stream *stream, uint32_t address, uint32_t value)
{
	etna_cmd_stream_reserve(stream, 2);
	etna_emit_load_state(stream, address >> 2, 1);
	etna_cmd_stream_emit(stream, value);
}

static inline void
etna_set_state_from_bo(struct etna_cmd_stream *stream,
		       uint32_t address, struct etna_bo *bo)
{
	etna_cmd_stream_reserve(stream, 2);
	etna_emit_load_state(stream, address >> 2, 1);

	etna_cmd_stream_reloc(stream, &(struct etna_reloc){
		.bo = bo,
		.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE,
		.offset = 0,
	});
}

static void
draw_paint_node(struct weston_paint_node *pnode, struct etna_renderbuffer *rb)
{
	struct weston_matrix src_transform = pnode->output_to_buffer_matrix;
	struct etna_renderer *er = get_renderer(pnode->surface->compositor);
	struct etna_surface_state *ess = get_surface_state(pnode->surface);
	struct etna_buffer_state *ebs = ess->buffer;
	struct weston_buffer *buffer = ess->buffer_ref.buffer;
	struct etna_cmd_stream *stream = er->etna_stream;
	pixman_region32_t *damage = &rb->base.damage;
	pixman_region32_t repaint;
	pixman_box32_t *rects;
	int nrects;
	uint32_t dst_cfg, rot = 0;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint, &pnode->visible, damage);

	weston_region_global_to_output(&repaint, pnode->output, &repaint);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	if (buffer->type == WESTON_BUFFER_SOLID) {
		dst_cfg = VIVS_DE_DEST_CONFIG_COMMAND_CLEAR;

		etna_set_state(stream, VIVS_DE_CLEAR_PIXEL_VALUE32,
			       ebs->clear_color);
	} else {
		dst_cfg = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
		uint32_t stride = ebs->stride;

		etna_set_state_from_bo(stream, VIVS_DE_SRC_ADDRESS, ebs->bo);
		etna_set_state(stream, VIVS_DE_SRC_STRIDE, stride);
		etna_set_state(stream, VIVS_DE_SRC_ROTATION_HEIGHT,
			       VIVS_DE_SRC_ROTATION_HEIGHT_HEIGHT(ebs->height));
		etna_set_state(stream, VIVS_DE_SRC_ROTATION_CONFIG,
			       VIVS_DE_SRC_ROTATION_CONFIG_WIDTH(ebs->width));
		etna_set_state(stream, VIVS_DE_SRC_CONFIG,
			       VIVS_DE_SRC_CONFIG_SOURCE_FORMAT(ebs->de_format) |
			       VIVS_DE_SRC_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
			       (ebs->tiled ? VIVS_DE_BLOCK4_SRC_CONFIG_TILED_ENABLE : 0));
		etna_set_state(stream, VIVS_DE_SRC_EX_CONFIG,
				ebs->tiled == 2 ? VIVS_DE_SRC_EX_CONFIG_SUPER_TILED_ENABLE : 0);

		/* FIXME: handle stretch blits */
		etna_set_state(stream, VIVS_DE_STRETCH_FACTOR_LOW, 0);
		etna_set_state(stream, VIVS_DE_STRETCH_FACTOR_HIGH, 0);
	}

	dst_cfg |= VIVS_DE_DEST_CONFIG_FORMAT(rb->de_format) |
		   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
		   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
		   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE;
	etna_set_state(stream, VIVS_DE_DEST_CONFIG, dst_cfg);

	if (pnode->valid_transform) {
		switch (pnode->transform) {
		case WL_OUTPUT_TRANSFORM_NORMAL:
			rot = VIVS_DE_ROT_ANGLE_SRC(DE_ROT_MODE_ROT0);
			break;
		case WL_OUTPUT_TRANSFORM_90:
			rot = VIVS_DE_ROT_ANGLE_SRC(DE_ROT_MODE_ROT90);
			weston_matrix_translate(&src_transform,
						-ebs->width, 0, 0);
			weston_matrix_rotate_xy(&src_transform, 0, -1);
			break;
		case WL_OUTPUT_TRANSFORM_180:
			rot = VIVS_DE_ROT_ANGLE_SRC(DE_ROT_MODE_ROT180);
			weston_matrix_translate(&src_transform,
						-ebs->width, -ebs->height, 0);
			weston_matrix_rotate_xy(&src_transform, -1, 0);
			break;
		case WL_OUTPUT_TRANSFORM_270:
			rot = VIVS_DE_ROT_ANGLE_SRC(DE_ROT_MODE_ROT270);
			weston_matrix_translate(&src_transform,
						0, -ebs->height, 0);
			weston_matrix_rotate_xy(&src_transform, 0, 1);
			break;
		default:
			weston_log("unhandled output transform\n");
		}
	}

	etna_set_state(stream, VIVS_DE_ROT_ANGLE, rot);

	if (pnode->view->alpha == 1.0f &&
	    pixel_format_is_opaque(buffer->pixel_format)) {
		etna_set_state(stream, VIVS_DE_ALPHA_CONTROL,
			       VIVS_DE_ALPHA_CONTROL_ENABLE_OFF);
	} else {
		uint32_t alpha_modes = VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL;
		uint32_t alpha = pnode->view->alpha * 255.0f;
		uint32_t color = VIVS_DE_COLOR_MULTIPLY_MODES_SRC_GLOBAL_PREMULTIPLY_DISABLE;

		etna_set_state(stream, VIVS_DE_ALPHA_CONTROL,
			       VIVS_DE_ALPHA_CONTROL_ENABLE_ON);

		if (pnode->view->alpha < 1.0f) {
			etna_set_state(stream, VIVS_DE_GLOBAL_SRC_COLOR, alpha << 24);

			if (pixel_format_is_opaque(buffer->pixel_format))
				alpha_modes = VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;
			else
				alpha_modes = VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_SCALED;

			color = VIVS_DE_COLOR_MULTIPLY_MODES_SRC_GLOBAL_PREMULTIPLY_ALPHA;
		}

		alpha_modes |= VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ONE) |
			      VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_INVERSED);
		etna_set_state(stream, VIVS_DE_ALPHA_MODES, alpha_modes);

		color |= VIVS_DE_COLOR_MULTIPLY_MODES_SRC_PREMULTIPLY_DISABLE |
			 VIVS_DE_COLOR_MULTIPLY_MODES_DST_PREMULTIPLY_DISABLE |
			 VIVS_DE_COLOR_MULTIPLY_MODES_DST_DEMULTIPLY_DISABLE;
		etna_set_state(stream, VIVS_DE_COLOR_MULTIPLY_MODES, color);
	}

	rects = pixman_region32_rectangles(&repaint, &nrects);

	for (int i = 0; i < nrects; i++) {
		struct weston_coord src_origin;

		src_origin = weston_matrix_transform_coord(&src_transform,
				weston_coord(rects[i].x1, rects[i].y1));

		etna_set_state(stream, VIVS_DE_SRC_ORIGIN,
			       VIVS_DE_SRC_ORIGIN_X((uint16_t)(src_origin.x)) |
			       VIVS_DE_SRC_ORIGIN_Y((uint16_t)(src_origin.y)));

		etna_set_state(stream, VIVS_DE_SRC_SIZE,
			       VIVS_DE_SRC_SIZE_X(rects[i].x2 - rects[i].x1) |
			       VIVS_DE_SRC_SIZE_Y(rects[i].y2 - rects[i].y1));

#if 0
		weston_log("draw rect %d/%d -> %d/%d, src origin %d/%d\n",
			   rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2,
			   (uint16_t)src_origin.x, (uint16_t)src_origin.y);
#endif

		etna_cmd_stream_emit(stream, VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D |
				     VIV_FE_DRAW_2D_HEADER_COUNT(1));
		etna_cmd_stream_emit(stream, 0xdeadbeef);
		etna_cmd_stream_emit(stream,
				     VIV_FE_DRAW_2D_TOP_LEFT_X(rects[i].x1) |
				     VIV_FE_DRAW_2D_TOP_LEFT_Y(rects[i].y1));
		etna_cmd_stream_emit(stream,
				     VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(rects[i].x2) |
				     VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(rects[i].y2));
	}

	etna_set_state(stream, 1, 0);
	etna_set_state(stream, 1, 0);
	etna_set_state(stream, 1, 0);

	/* Some state changes between node paints need a flush/stall (e.g
	 * changing the rotation config, or blending with a overlapping node).
	 * Instead of tracking those conditions always flush and stall for now.
	 */
	etna_set_state(stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
	etna_set_state(stream, VIVS_GL_SEMAPHORE_TOKEN,
		       VIVS_GL_SEMAPHORE_TOKEN_FROM(SYNC_RECIPIENT_FE) |
		       VIVS_GL_SEMAPHORE_TOKEN_TO(SYNC_RECIPIENT_PE));
	etna_cmd_stream_emit(stream, VIV_FE_STALL_HEADER_OP_STALL);
	etna_cmd_stream_emit(stream, VIV_FE_STALL_TOKEN_FROM(SYNC_RECIPIENT_FE) |
			     VIV_FE_STALL_TOKEN_TO(SYNC_RECIPIENT_PE));

out:
	pixman_region32_fini(&repaint);
}

static void
etna_renderer_do_capture(struct weston_buffer *into,
			 struct etna_renderbuffer *from)
{
	void *src, *dst;
	int dst_stride;
	void *data;
	void *map;

	assert(into->type == WESTON_BUFFER_SHM);
	assert(into->shm_buffer);

	dst_stride = into->width * (into->pixel_format->bpp / 8);
	data = wl_shm_buffer_get_data(into->shm_buffer);
	map = etna_bo_map(from->bo);

	etna_bo_cpu_prep(from->bo, DRM_ETNA_PREP_READ);
	wl_shm_buffer_begin_access(into->shm_buffer);

	src = map;
	dst = data;
	for (int y = 0; y < into->height; y++, dst += dst_stride, src += from->stride) {
		memcpy(dst, src, from->stride);
	}

	wl_shm_buffer_end_access(into->shm_buffer);
	etna_bo_cpu_fini(from->bo);
}

static void
etna_renderer_do_capture_tasks(struct weston_output *output,
			       enum weston_output_capture_source source,
			       struct etna_renderbuffer *from)
{
	const struct pixel_format_info *pfmt = from->format;
	struct weston_capture_task *ct;
	int width = from->width;
	int height = from->height;

	while ((ct = weston_output_pull_capture_task(output, source, width,
						     height, pfmt))) {
		struct weston_buffer *buffer = weston_capture_task_get_buffer(ct);

		assert(buffer->width == width);
		assert(buffer->height == height);
		assert(buffer->pixel_format->format == pfmt->format);

		if (buffer->type != WESTON_BUFFER_SHM) {
			weston_capture_task_retire_failed(ct, "etnaviv: unsupported buffer");
			continue;
		}

		etna_renderer_do_capture(buffer, from);
		weston_capture_task_retire_complete(ct);
	}
}

static void
etna_renderer_repaint_output(struct weston_output *output,
			   pixman_region32_t *output_damage,
			   struct weston_renderbuffer *renderbuffer)
{
	struct etna_output_state *eos = get_output_state(output);
	struct etna_renderer *er = get_renderer(output->compositor);
	struct etna_renderbuffer *rb;
	struct etna_cmd_stream *stream = er->etna_stream;
	struct weston_paint_node *pnode;

	/* Accumulate damage in all renderbuffers */
	wl_list_for_each(rb, &eos->renderbuffer_list, link) {
		pixman_region32_union(&rb->base.damage,
				      &rb->base.damage,
				      output_damage);
	}

	rb = to_etna_renderbuffer(renderbuffer);

	/* set some common states */
	etna_set_state(stream, VIVS_DE_ROP,
		       VIVS_DE_ROP_ROP_FG(0xcc) |
		       VIVS_DE_ROP_ROP_BG(0xcc) |
		       VIVS_DE_ROP_TYPE_ROP4);

	etna_set_state(stream, VIVS_DE_PE_CONTROL, 0);
	etna_set_state(stream, VIVS_DE_PE_DITHER_LOW, 0xffffffff);
	etna_set_state(stream, VIVS_DE_PE_DITHER_HIGH, 0xffffffff);
	etna_set_state(stream, VIVS_DE_CONFIG, 0);
	etna_set_state(stream, VIVS_DE_SRC_ORIGIN_FRACTION, 0);

	/* FIXME: handle different supertile versions */
	etna_set_state(stream, VIVS_DE_TILE_CONFIG,
		       VIVS_DE_TILE_CONFIG_SUPERTILE_VERSION(1));

	/* set target */
	etna_set_state_from_bo(stream, VIVS_DE_DEST_ADDRESS, rb->bo);
	etna_set_state(stream, VIVS_DE_DEST_STRIDE, rb->stride);
	etna_set_state(stream, VIVS_DE_DEST_ROTATION_HEIGHT, rb->height);
	etna_set_state(stream, VIVS_DE_DEST_ROTATION_CONFIG,
		       VIVS_DE_DEST_ROTATION_CONFIG_WIDTH(rb->width));

	/* set clip (handle damage here?) */
	etna_set_state(stream, VIVS_DE_CLIP_TOP_LEFT,
		       VIVS_DE_CLIP_TOP_LEFT_X(0) |
		       VIVS_DE_CLIP_TOP_LEFT_Y(0));
	etna_set_state(stream, VIVS_DE_CLIP_BOTTOM_RIGHT,
		       VIVS_DE_CLIP_BOTTOM_RIGHT_X(rb->width) |
		       VIVS_DE_CLIP_BOTTOM_RIGHT_Y(rb->height));

	wl_list_for_each_reverse(pnode, &output->paint_node_z_order_list,
				 z_order_link) {
		if (pnode->plane == &output->primary_plane)
			draw_paint_node(pnode, rb);
	}

	etna_cmd_stream_flush(stream);

	pixman_region32_clear(&rb->base.damage);

	etna_renderer_do_capture_tasks(output,
				       WESTON_OUTPUT_CAPTURE_SOURCE_FRAMEBUFFER,
				       rb);
}

static void
etna_renderer_flush_damage(struct weston_paint_node *pnode)
{
	struct weston_surface *surface = pnode->surface;
	struct weston_buffer *buffer = surface->buffer_ref.buffer;
	struct etna_surface_state *ess = get_surface_state(surface);
	struct etna_buffer_state *ebs = ess->buffer;
	void *data, *map;
	int src_stride;

	assert(buffer && ebs);

	pixman_region32_union(&ebs->damage, &ebs->damage, &surface->damage);

	if (!buffer->shm_buffer)
		return;

	if (!pixman_region32_not_empty(&ebs->damage) &&
	    !ebs->needs_full_upload)
		return;

	src_stride = buffer->width * (buffer->pixel_format->bpp / 8);
	data = wl_shm_buffer_get_data(buffer->shm_buffer);
	map = etna_bo_map(ebs->bo);

	etna_bo_cpu_prep(ebs->bo, DRM_ETNA_PREP_WRITE);
	wl_shm_buffer_begin_access(buffer->shm_buffer);

	if (ebs->needs_full_upload) {
		void *src, *dst;

		src = data;
		dst = map;
		for (int y = 0; y < buffer->height; y++, dst += ebs->stride, src += src_stride) {
			memcpy(dst, src, src_stride);
		}
	} else {
		pixman_box32_t *rectangles;
		int nrects;

		rectangles = pixman_region32_rectangles(&ebs->damage, &nrects);
		for (int i = 0; i < nrects; i++) {
			int pixel_stride, bytes;
			pixman_box32_t r;
			void *src, *dst;

			r = weston_surface_to_buffer_rect(surface, rectangles[i]);
			pixel_stride = buffer->pixel_format->bpp / 8;
			src = data + src_stride * r.y1 + pixel_stride * r.x1;
			dst = map + ebs->stride * r.y1 + pixel_stride * r.x1;
			bytes = pixel_stride * (r.x2 - r.x1);
			for (int y = r.y1; y < r.y2; y++, dst += ebs->stride, src += src_stride) {
				memcpy(dst, src, bytes);
			}
		}
	}

	wl_shm_buffer_end_access(buffer->shm_buffer);
	etna_bo_cpu_fini(ebs->bo);

	pixman_region32_clear(&ebs->damage);
	ebs->needs_full_upload = false;

	weston_buffer_reference(&ess->buffer_ref, buffer,
				BUFFER_WILL_NOT_BE_ACCESSED);
	weston_buffer_release_reference(&ess->buffer_release_ref, NULL);
}

static void
handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct weston_buffer *buffer = data;
	struct etna_buffer_state *ebs =
		container_of(listener, struct etna_buffer_state, destroy_listener);

	assert(ebs == buffer->renderer_private);
	buffer->renderer_private = NULL;

	destroy_buffer_state(ebs);
}

static struct etna_buffer_state *
ensure_renderer_etna_buffer_state(struct weston_surface *surface,
				  struct weston_buffer *buffer)
{
	struct etna_surface_state *ess = get_surface_state(surface);
	struct etna_buffer_state *ebs = buffer->renderer_private;

	if (ebs) {
		ess->buffer = ebs;
		return ebs;
	}

	ebs = xzalloc(sizeof(*ebs));
	ebs->destroy_listener.notify = handle_buffer_destroy;
	wl_signal_add(&buffer->destroy_signal, &ebs->destroy_listener);
	pixman_region32_init(&ebs->damage);

	buffer->renderer_private = ebs;
	ess->buffer = ebs;

	return ebs;
}
static void
etna_renderer_attach_solid(struct weston_surface *surface,
			   struct weston_buffer *buffer)
{
	struct etna_buffer_state *ebs;

	ebs = ensure_renderer_etna_buffer_state(surface, buffer);

	ebs->clear_color =
		(int)(buffer->solid.a * 255.0f) << 24 |
		(int)(buffer->solid.r * 255.0f) << 16 |
		(int)(buffer->solid.g * 255.0f) << 8 |
		(int)(buffer->solid.b * 255.0f) << 0;
}

static void
etna_renderer_attach_shm(struct weston_surface *surface,
			 struct weston_buffer *buffer)
{
	struct etna_renderer *er = get_renderer(surface->compositor);
	struct etna_surface_state *ess = get_surface_state(surface);
	struct weston_buffer *old_buffer = ess->buffer_ref.buffer;
	struct etna_buffer_state *ebs;
	int de_format = etna_drm_to_de_format(buffer->pixel_format->format);

	if (de_format < 0) {
		weston_log("unhandled buffer format 0x%0x\n",
			   buffer->pixel_format->format);
		return;
	}

	/* Reuse previous SHM buffer state, if possible. */
	assert(!ess->buffer ||
	       (old_buffer && old_buffer->type == WESTON_BUFFER_SHM));
	if (ess->buffer &&
	    buffer->width == old_buffer->width &&
	    buffer->height == old_buffer->height &&
	    buffer->pixel_format == old_buffer->pixel_format)
		return;

	if (ess->buffer)
		destroy_buffer_state(ess->buffer);
	ess->buffer = NULL;

	ebs = xzalloc(sizeof(*ebs));

	wl_list_init(&ebs->destroy_listener.link);
	pixman_region32_init(&ebs->damage);

	buffer->renderer_private = ebs;
	ess->buffer = ebs;

	ebs->de_format = de_format;

	/* FIXME: check alignment restrictions */
	ebs->height = ROUND_UP_N(buffer->height, 8);
	ebs->width = ROUND_UP_N(buffer->width, 8);
	ebs->stride = (buffer->pixel_format->bpp / 8) * ebs->width;
	ebs->bo = etna_bo_new(er->etna_dev, ebs->height * ebs->stride,
			      DRM_ETNA_GEM_CACHE_WC);

	ebs->needs_full_upload = true;
}

static bool
etna_renderer_attach_dmabuf(struct weston_surface *surface,
			    struct weston_buffer *buffer)
{
	struct etna_surface_state *ess = get_surface_state(surface);
	struct etna_buffer_state *ebs;

	assert(buffer->renderer_private);
	ebs = buffer->renderer_private;

	ess->buffer = ebs;

	return true;
}

static void
etna_renderer_attach(struct weston_paint_node *pnode)
{
	struct weston_surface *surface = pnode->surface;
	struct weston_buffer *buffer = surface->buffer_ref.buffer;
	struct etna_surface_state *ess = get_surface_state(surface);

	if (ess->buffer_ref.buffer == buffer)
		return;

	/* SHM buffer state is allocated per-surface instead of per-buffer */
	if (ess->buffer && ess->buffer_ref.buffer->type == WESTON_BUFFER_SHM) {
		if (!buffer || buffer->type != WESTON_BUFFER_SHM) {
			destroy_buffer_state(ess->buffer);
			ess->buffer = NULL;
		}
	} else {
		ess->buffer = NULL;
	}

	if (!buffer)
		goto out;

	switch (buffer->type) {
	case WESTON_BUFFER_SOLID:
		etna_renderer_attach_solid(surface, buffer);
		break;
	case WESTON_BUFFER_SHM:
		etna_renderer_attach_shm(surface, buffer);
		break;
	case WESTON_BUFFER_DMABUF:
		etna_renderer_attach_dmabuf(surface, buffer);
		break;
	default:
		weston_log("%s: unhandled buffer type %d\n", __func__, buffer->type);
	}

	weston_buffer_reference(&ess->buffer_ref, buffer,
				BUFFER_MAY_BE_ACCESSED);
	weston_buffer_release_reference(&ess->buffer_release_ref,
					surface->buffer_release_ref.buffer_release);
	return;

out:
	assert(!ess->buffer);
	weston_buffer_reference(&ess->buffer_ref, NULL,
				BUFFER_WILL_NOT_BE_ACCESSED);
	weston_buffer_release_reference(&ess->buffer_release_ref, NULL);
}

static void
etna_renderer_buffer_init(struct weston_compositor *ec,
			  struct weston_buffer *buffer)
{
	struct etna_buffer_state *ebs;

	if (buffer->type != WESTON_BUFFER_DMABUF)
		return;

	assert(!buffer->renderer_private);
	ebs = linux_dmabuf_buffer_get_user_data(buffer->dmabuf);
	assert(ebs);
	linux_dmabuf_buffer_set_user_data(buffer->dmabuf, NULL, NULL);
	buffer->renderer_private = ebs;
	ebs->destroy_listener.notify = handle_buffer_destroy;
	wl_signal_add(&buffer->destroy_signal, &ebs->destroy_listener);
}

static void
etna_renderer_destroy_dmabuf(struct linux_dmabuf_buffer *dmabuf)
{
	struct etna_buffer_state *ebs =
		linux_dmabuf_buffer_get_user_data(dmabuf);

	linux_dmabuf_buffer_set_user_data(dmabuf, NULL, NULL);
	destroy_buffer_state(ebs);
}

static bool
etna_renderer_import_dmabuf(struct weston_compositor *ec,
			  struct linux_dmabuf_buffer *dmabuf)
{
	int de_format = etna_drm_to_de_format(dmabuf->attributes.format);
	struct etna_renderer *er = get_renderer(ec);
	struct etna_buffer_state *ebs;

	if (de_format < 0) {
		weston_log("%s: unhandled dmabuf format 0x%x",__func__,
			   dmabuf->attributes.format);
		return false;
	}

	ebs = xzalloc(sizeof(*ebs));

	ebs->bo = etna_bo_from_dmabuf(er->etna_dev, dmabuf->attributes.fd[0]);
	if (!ebs->bo) {
		free(ebs);
		return false;
	}

	wl_list_init(&ebs->destroy_listener.link);
	pixman_region32_init(&ebs->damage);
	ebs->de_format = de_format;
	ebs->width = dmabuf->attributes.width;
	ebs->height = dmabuf->attributes.height;
	ebs->stride = dmabuf->attributes.stride[0];

	if (dmabuf->attributes.modifier == DRM_FORMAT_MOD_VIVANTE_TILED)
		ebs->tiled = 1;
	if (dmabuf->attributes.modifier == DRM_FORMAT_MOD_VIVANTE_SUPER_TILED)
		ebs->tiled = 2;

	linux_dmabuf_buffer_set_user_data(dmabuf, ebs,
			etna_renderer_destroy_dmabuf);

	return true;
}

static const struct weston_drm_format_array *
etna_renderer_get_supported_formats(struct weston_compositor *ec)
{
	struct etna_renderer *er = get_renderer(ec);

	return &er->supported_formats;
}

static void
etna_renderer_destroy(struct weston_compositor *ec)
{
	struct etna_renderer *er = get_renderer(ec);

	wl_signal_emit(&er->destroy_signal, er);

	etna_cmd_stream_del(er->etna_stream);
	etna_pipe_del(er->etna_pipe);
	etna_gpu_del(er->etna_gpu);
	etna_device_del(er->etna_dev);
	drmClose(er->drm_fd);

	free(er);
	ec->renderer = NULL;
}
static const int supported_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB565,
};

static const uint64_t supported_modifiers[] = {
	DRM_FORMAT_MOD_VIVANTE_SUPER_TILED,
	DRM_FORMAT_MOD_VIVANTE_TILED,
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static void
etna_renderer_populate_dmabuf_formats(struct weston_compositor *ec,
				      struct etna_renderer *er)
{
	struct weston_dmabuf_feedback_tranche *tranche;
	struct stat dev_stat;

	weston_drm_format_array_init(&er->supported_formats);

	for (unsigned int i = 0; i < ARRAY_LENGTH(supported_formats); i++) {
		struct weston_drm_format *fmt;

		fmt = weston_drm_format_array_add_format(&er->supported_formats,
							 supported_formats[i]);
		if (!fmt)
			break;

		for (unsigned int j = 0; j < ARRAY_LENGTH(supported_modifiers); j++) {
			int ret;

			ret = weston_drm_format_add_modifier(fmt, supported_modifiers[j]);
			if (ret)
				break;
		}
	}

	ec->dmabuf_feedback_format_table =
		weston_dmabuf_feedback_format_table_create(&er->supported_formats);
	if (!ec->dmabuf_feedback_format_table)
		return;

	if (fstat(er->drm_fd, &dev_stat) != 0)
		return;

	ec->default_dmabuf_feedback = weston_dmabuf_feedback_create(dev_stat.st_rdev);
	if (!ec->default_dmabuf_feedback)
		return;

	tranche =
		weston_dmabuf_feedback_tranche_create(ec->default_dmabuf_feedback,
						      ec->dmabuf_feedback_format_table,
						      dev_stat.st_rdev, 0,
						      RENDERER_PREF);
	if (!tranche) {
		weston_dmabuf_feedback_destroy(ec->default_dmabuf_feedback);
		ec->default_dmabuf_feedback = NULL;
	}
}

static int
etna_renderer_init(struct weston_compositor *ec)
{
	struct etna_renderer *er;

	er = xzalloc(sizeof *er);

	er->base.repaint_output = etna_renderer_repaint_output;
	er->base.flush_damage = etna_renderer_flush_damage;
	er->base.attach = etna_renderer_attach;
	er->base.buffer_init = etna_renderer_buffer_init;
	er->base.import_dmabuf = etna_renderer_import_dmabuf;
	er->base.get_supported_formats = etna_renderer_get_supported_formats;
	er->base.destroy = etna_renderer_destroy;
	er->base.type = WESTON_RENDERER_ETNA;

	wl_signal_init(&er->destroy_signal);

	ec->renderer = &er->base;

	er->drm_fd = drmOpenWithType("etnaviv", NULL, DRM_NODE_RENDER);
	if (er->drm_fd < 0)
		goto out_free_renderer;

	er->etna_dev = etna_device_new(er->drm_fd);
	if (!er->etna_dev)
		goto out_close_fd;


	/* search for a 2D capable GPU core */
	for (int gpu_core = 0; ;gpu_core++) {
		uint64_t feat;

		if (er->etna_gpu)
			etna_gpu_del(er->etna_gpu);

		er->etna_gpu = etna_gpu_new(er->etna_dev, gpu_core);
		if (!er->etna_gpu)
			break;

		if (etna_gpu_get_param(er->etna_gpu,
				       ETNA_GPU_FEATURES_0, &feat))
			continue;

		if (!(feat & chipFeatures_PIPE_2D))
			continue;

		er->etna_pipe = etna_pipe_new(er->etna_gpu, ETNA_PIPE_2D);
		if (er->etna_pipe)
			break;
	}

	if (!er->etna_pipe)
		goto out_free_etna_dev;

	er->etna_stream = etna_cmd_stream_new(er->etna_pipe, 0x2000, NULL, NULL);
	if (!er->etna_stream)
		goto out_free_pipe;

	etna_renderer_populate_dmabuf_formats(ec, er);

	return 0;

out_free_pipe:
	etna_pipe_del(er->etna_pipe);
out_free_etna_dev:
	etna_device_del(er->etna_dev);
out_close_fd:
	drmClose(er->drm_fd);
out_free_renderer:
	free(er);

	return -1;
}

static int
etna_renderer_output_create(struct weston_output *output)
{
	struct etna_output_state *eos;

	eos = zalloc(sizeof *eos);
	if (eos == NULL)
		return -1;

	wl_list_init(&eos->renderbuffer_list);

	output->renderer_state = eos;

	return 0;
}

static void
etna_renderer_output_destroy(struct weston_output *output)
{
	struct etna_output_state *eos = get_output_state(output);
	struct etna_renderbuffer *rb, *tmp;

	wl_list_for_each_safe(rb, tmp, &eos->renderbuffer_list, link) {
		wl_list_remove(&rb->link);
		weston_renderbuffer_unref(&rb->base);
	}

	free(eos);
}

static void
etna_renderbuffer_destroy(struct weston_renderbuffer *renderbuffer)
{
	struct etna_renderbuffer *rb = to_etna_renderbuffer(renderbuffer);

	etna_bo_del(rb->bo);

	pixman_region32_fini(&rb->base.damage);
	free(rb);
}

static struct weston_renderbuffer *
etna_renderbuffer_from_dmabuf(struct weston_output *output, int dmabuf_fd,
			      const struct pixel_format_info *format,
			      int32_t width, int32_t height, int32_t stride)
{
	struct weston_compositor *ec = output->compositor;
	struct etna_output_state *eos = get_output_state(output);
	struct etna_renderer *er = get_renderer(ec);
	struct etna_renderbuffer *rb;

	rb = xzalloc(sizeof *rb);

	rb->format = format;
	rb->de_format = etna_drm_to_de_format(format->format);
	if (rb->de_format < 0)
		goto out_free;

	rb->bo = etna_bo_from_dmabuf(er->etna_dev, dmabuf_fd);
	if (!rb->bo)
		goto out_free;

	rb->width = width;
	rb->height = height;
	rb->stride = stride;

	pixman_region32_init(&rb->base.damage);
	rb->base.refcount = 1;
	rb->base.destroy = etna_renderbuffer_destroy;
	wl_list_insert(&eos->renderbuffer_list, &rb->link);

	weston_output_update_capture_info(output,
					  WESTON_OUTPUT_CAPTURE_SOURCE_FRAMEBUFFER,
					  width, height, format);

	return &rb->base;

out_free:
	free(rb);
	return NULL;
}

static struct weston_renderbuffer *
etna_create_renderbuffer(struct weston_output *output,
			 const struct pixel_format_info *format,
			 int32_t width, int32_t height)
{
	struct weston_compositor *ec = output->compositor;
	struct etna_output_state *eos = get_output_state(output);
	struct etna_renderer *er = get_renderer(ec);
	struct etna_renderbuffer *rb;
	uint32_t stride;

	rb = xzalloc(sizeof *rb);
	rb->format = format;
	rb->de_format = etna_drm_to_de_format(format->format);
	if (rb->de_format < 0)
		goto out_free;

	stride = width * (format->bpp / 8);

	rb->bo = etna_bo_new(er->etna_dev, height * stride, DRM_ETNA_GEM_CACHE_WC);
	if (!rb->bo)
		goto out_free;

	rb->width = width;
	rb->height = height;
	rb->stride = stride;

	pixman_region32_init(&rb->base.damage);
	/*
	 * One reference is kept on the renderbuffer_list,
	 * the other is returned to the calling backend.
	 */
	rb->base.refcount = 2;
	rb->base.destroy = etna_renderbuffer_destroy;
	wl_list_insert(&eos->renderbuffer_list, &rb->link);

	weston_output_update_capture_info(output,
					  WESTON_OUTPUT_CAPTURE_SOURCE_FRAMEBUFFER,
					  width, height, format);

	return &rb->base;

out_free:
	free(rb);
	return NULL;
}

WL_EXPORT struct etna_renderer_interface etna_renderer_interface = {
	.display_create = etna_renderer_init,
	.output_create = etna_renderer_output_create,
	.output_destroy = etna_renderer_output_destroy,
	.renderbuffer_from_dmabuf = etna_renderbuffer_from_dmabuf,
	.create_renderbuffer = etna_create_renderbuffer,
};
