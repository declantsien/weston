/*
 * Copyright © 2019 Advanced Driver Information Technology Joint Venture GmbH
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

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>

#include <libweston/libweston.h>
#include <libweston/backend-transmitter.h>
#include "waltham-renderer.h"

struct waltham_renderer {
	struct renderer base;
};

struct GstAppContext {
	GMainLoop *loop;
	GstBus *bus;
	GstElement *pipeline;
	GstElement *appsrc;
	GstBuffer *gstbuffer;
};

gboolean bus_message(GstBus *bus, GstMessage *message, gpointer p)
{
	struct GstAppContext *gstctx = p;

	switch(GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err;
		gchar *debug;

		gst_message_parse_error(message, &err, &debug);
		g_print("ERROR: %s\n", err->message);

		g_error_free(err);
		g_free(debug);
		g_main_loop_quit(gstctx->loop);
		break;
	}

	case GST_MESSAGE_STATE_CHANGED: {
		GstState oldstate, newstate;

		gst_message_parse_state_changed(message, &oldstate, &newstate,
		                                NULL);
		if (newstate == GST_STATE_NULL)
			fprintf(stderr, "%s: state is NULL\n",
			        GST_MESSAGE_SRC_NAME(message));
		break;
	}
	default:
		fprintf(stderr, "Unhandled message\n");
		break;
	}
	return TRUE;
}

static int
gst_pipe_init(struct transmitter_output *output, struct gst_settings *settings)
{
	struct GstAppContext *gstctx;
	gstctx = zalloc(sizeof(*gstctx));
	if (!gstctx) {
		weston_log("Enable to allocate memory\n");
		return -1;
	}
	GstCaps *caps;
	GError *gerror = NULL;
	FILE * pFile;
	long unsigned int lSize;
	char * pipe = NULL;
	size_t res;

	/* create gstreamer pipeline */
	gst_init(NULL, NULL);
	gstctx->loop = g_main_loop_new(NULL, FALSE);

	/* read pipeline from file */
	pFile = fopen("/etc/xdg/weston/transmitter_pipeline.cfg", "rb");
	if (pFile == NULL) {
		weston_log("File open error\n");
		return -1;
	}

	/* obtain file size */
	fseek(pFile, 0, SEEK_END);
	lSize = ftell(pFile);
	rewind(pFile);

	/* allocate memory to contain the whole file: */
	pipe = (char*) zalloc(sizeof(char) * lSize);
	if (pipe == NULL) {
		weston_log("Cannot allocate memory\n");
		return -1;
	}

	/* copy the file into the buffer: */
	res = fread(pipe, 1, lSize, pFile);
	if (res != lSize) {
		weston_log("File read error\n");
		return -1;
	}

	/* close file */
	fclose(pFile);
	weston_log("Parsing GST pipeline:%s", pipe);
	gstctx->pipeline = gst_parse_launch(pipe, &gerror);
	free(pipe);
	if (!gstctx->pipeline)
		weston_log("Could not create gstreamer pipeline.\n");

	gstctx->bus = gst_pipeline_get_bus((GstPipeline*) (
                                           (void*) gstctx->pipeline));
	gst_bus_add_watch(gstctx->bus, bus_message, &gstctx);

	gstctx->appsrc = (GstElement *) gst_bin_get_by_name(
			GST_BIN(gstctx->pipeline), "src");
	if (!gstctx->appsrc)
		return -1;

	caps = gst_caps_new_simple("video/x-raw",
				   "format", G_TYPE_STRING, "BGRx",
				   "width", G_TYPE_INT, settings->width,
				   "height", G_TYPE_INT, settings->height,
				   NULL);
	if (!caps)
		return -1;

	g_object_set(G_OBJECT(gstctx->appsrc),
		     "caps", caps,
		     "stream-type", 0,
		     "format", GST_FORMAT_TIME,
		     "is-live", TRUE,
		     NULL);
	gst_caps_unref(caps);

	gst_element_set_state((GstElement*)((void*)gstctx->pipeline),
			      GST_STATE_PLAYING);
	output->renderer->ctx = gstctx;
	return 0;
}

static int recorder_enable(struct transmitter_output *output)
{
	struct gst_settings *settings;
	struct weston_transmitter_remote* remote = output->remote;

	settings = malloc(sizeof(*settings));
	if(!settings)
		goto err;
	settings->ip = remote->addr;
	settings->port = atoi(remote->port);
	settings->width = output->renderer->surface_width;
	settings->height = output->renderer->surface_height;

	weston_log("gst-setting are :-->\n");
	weston_log("ip = %s \n", settings->ip);
	weston_log("port = %d \n", settings->port);
	weston_log("width = %d \n", settings->width);
	weston_log("height = %d \n", settings->height);

	gst_pipe_init(output, settings);

	return 0;
err:
	weston_log("[gst recorder] %s:"
		" invalid settings\n", output->base.name);
	free(settings);
	return -1;
}

static void waltham_renderer_repaint_output(struct transmitter_output *output)
{
	GstBuffer *gstbuffer;
	GstMemory *mem;
	GstAllocator *allocator;
	int stride = output->renderer->buf_stride;
	gsize offset = 0;

	if (!output->renderer->recorder_enabled) {
		recorder_enable(output);
		output->renderer->recorder_enabled = 1;
	}

	gstbuffer = gst_buffer_new();
	allocator = gst_dmabuf_allocator_new();
	mem = gst_dmabuf_allocator_alloc(allocator, output->renderer->dmafd,
					 stride * output->renderer->surface_height);
	gst_buffer_append_memory(gstbuffer, mem);
	gst_buffer_add_video_meta_full(gstbuffer,
				       GST_VIDEO_FRAME_FLAG_NONE,
				       GST_VIDEO_FORMAT_BGRx,
				       output->renderer->surface_width,
				       output->renderer->surface_height,
				       1, &offset, &stride);

	gst_app_src_push_buffer((GstAppSrc *)output->renderer->ctx->appsrc, gstbuffer);
	gst_object_unref(allocator);
}

static int waltham_renderer_display_create(struct transmitter_output *output)
{
	struct waltham_renderer *wth_renderer;

	wth_renderer = zalloc(sizeof *wth_renderer);
	if (wth_renderer == NULL)
		return -1;
	wth_renderer->base.repaint_output = waltham_renderer_repaint_output;

	output->renderer = &wth_renderer->base;

	return 0;
}

WL_EXPORT struct waltham_renderer_interface waltham_renderer_interface = {
		.display_create = waltham_renderer_display_create
};
