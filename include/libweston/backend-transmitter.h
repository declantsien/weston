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

#ifndef WESTON_TRANSMITTER_API_H
#define WESTON_TRANSMITTER_API_H

#include <stdint.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include "ivi-shell/ivi-layout-export.h"
#include <libweston/plugin-registry.h>
#include <libweston/libweston.h>

#define WESTON_TRANSMITTER_BACKEND_CONFIG_VERSION 3
#define WESTON_TRANSMITTER_OUTPUT_API_NAME "weston_transmitter_output_api_v1"
#define WESTON_TRANSMITTER_API_NAME "transmitter_v1"

struct weston_transmitter;
struct weston_transmitter_remote;
struct weston_transmitter_surface;
struct drm_fb;
struct libinput_device;

struct weston_transmitter_output_api {
	/** Seat to be used by the output. Set to NULL to use default seat. */
	void (*set_seat)(struct weston_output *output, const char *seat);

	/** Initialize a transmitter output with specified width and height.
	 * Returns 0 on success, -1 on failure.
	 */
	int (*output_set_size)(struct weston_output *output,
				int width, int height);
	/** Create new head */
	int (*create_head)(struct weston_compositor *compositor,
			   const char *name);

	/** create new weston_transmitter_remote */
	int (*create_remote)(char *model, char *addr, char *port, int *width,
			     int *height, struct weston_compositor *c);
};

static inline const struct weston_transmitter_output_api *
weston_transmitter_output_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, WESTON_TRANSMITTER_OUTPUT_API_NAME,
				    sizeof(struct weston_transmitter_output_api));

	return (const struct weston_transmitter_output_api *)api;
}

typedef int (*submit_frame_cb)(struct weston_output *output, int fd,
			       int stride, struct drm_fb *buffer);

/** The backend configuration struct.
 *
 * weston_transmitter_backend_config contains the configuration used by a
 * transmitter backend.
 */
struct weston_transmitter_backend_config {
	struct weston_backend_config base;

	/** The tty to be used. Set to 0 to use the current tty. */
	int tty;

	/** The seat to be used for input and output. */
	char *seat_id;

	/** The pixel format of the framebuffer to be used.
	 *
	 * Valid values are:
	 * - NULL - The default format ("xrgb8888") will be used;
	 * - "xrgb8888";
	 * - "rgb565"
	 * - "xrgb2101010"
	 */
	char *gbm_format;

	/** Callback used to configure input devices.
	 *
	 * This function will be called by the backend when a new input device
	 * needs to be configured.
	 * If NULL the device will use the default configuration.
	 */
	void (*configure_device)(struct weston_compositor *compositor,
				 struct libinput_device *device);

	/** Specific DRM device to open like "card0" */
	char *specific_device;
};

/** See weston_transmitter_api::remote_get_status */
enum weston_transmitter_connection_status {
	/** The connection hand-shake is not yet complete */
	WESTON_TRANSMITTER_CONNECTION_INITIALIZING,

	/** The connection is live and ready to be used. */
	WESTON_TRANSMITTER_CONNECTION_READY,

	/** The connection is dead. */
	WESTON_TRANSMITTER_CONNECTION_DISCONNECTED,
};

/** See weston_transmitter_api::surface_get_stream_status */
enum weston_transmitter_stream_status {
	/** The stream hand-shake is not yet complete. */
	WESTON_TRANSMITTER_STREAM_INITIALIZING,

	/** The stream is carrying surface content updates as needed. */
	WESTON_TRANSMITTER_STREAM_LIVE,

	/** The stream has failed and disconnected permanently. */
	WESTON_TRANSMITTER_STREAM_FAILED,
};

/** The Transmitter Base API
 *
 * It provides remoting of weston_surfaces over the network.
 * Shells use this API to create remote connections and
 * push surfaces over the network.
 */
struct weston_transmitter_api {
	/** Fetch the Transmitter context
	 *
	 * \param compositor The compositor instance.
	 * \return The weston_transmitter context, which is always the same
	 * for the given compositor instance.
	 */
	struct weston_transmitter *
	(*transmitter_get)(struct weston_compositor *compositor);

	/** Connect to a remote server via Transmitter.
	 *
	 * \param txr The Transmitter context.
	 * \param status Listener to inform of connection status changes.
	 * \return A handle to the remote connection, or NULL on failure.
	 *
	 * This call attempts to open a connection asynchronously. The
	 * connection is not usable until the listener signals it is ready.
	 * The listener may also signal that the connection failed instead.
	 *
	 * The listener callback argument is the weston_transmitter_remote
	 * returned by this function. Use remote_get_status() to fetch the
	 * current status.
	 *
	 */
	struct weston_transmitter_remote *
	(*connect_to_remote)(struct weston_transmitter *txr);

	/** Retrieve the connection status.
	 *
	 * If the status is WESTON_TRANSMITTER_CONNECTION_DISCONNECTED,
	 * you have to shut the remote down completely. There is no automatic
	 * reconnect.
	 */
	enum weston_transmitter_connection_status
	(*remote_get_status)(struct weston_transmitter_remote *remote);

	/** Destroy/disconnect a remote connection.
	 *
	 * Disconnects if connected, and destroys the connection.
	 * The connection status handler is not called.
	 *
	 * The caller is responsible for destroying all
	 * weston_transmitter_surfaces before calling this.
	 */
	void
	(*remote_destroy)(struct weston_transmitter_remote *remote);

	/** Push a weston_surface to be transmitted to a remote.
	 *
	 * \param ws The surface to push.
	 * \param remote The remote connection to use.
	 * \param stream_status Listener for stream status changes.
	 * \return The Transmitter surface handle.
	 *
	 * The surface cannot be visible on the remote until the stream
	 * status listener signals WESTON_TRANSMITTER_STREAM_LIVE. After that,
	 * surface updates made by the application will be automatically
	 * streamed to the remote, and input events from the remote will be
	 * delivered to the application.
	 *
	 * The listener callback argument is the weston_transmitter_surface
	 * returned by this function. Use surface_get_stream_status() to
	 * fetch the current status.
	 */
	struct weston_transmitter_surface *
	(*surface_push_to_remote)(struct weston_surface *ws,
				  struct weston_transmitter_remote *remote,
				  struct wl_listener *stream_status);

	/** Retrieve the surface content stream status.
	 *
	 * If the status is WESTON_TRANSMITTER_STREAM_FAILED, remoting the
	 * surface has stopped. There is no automatic retry.
	 */
	enum weston_transmitter_stream_status
	(*surface_get_stream_status)(struct weston_transmitter_surface *txs);

	/** Stop remoting a weston_surface
	 *
	 * \param txs Transmitter surface handle to be stopped and freed.
	 * The surface stream status handler is not called.
	 */
	void
	(*surface_destroy)(struct weston_transmitter_surface *txs);

	/** Notify of weston_surface being configured
	 *
	 * \param txs The Transmitter surface handle.
	 * \param dx The x delta given in wl_surface.attach request.
	 * \param dy The y delta given in wl_surface.attach request.
	 *
	 * Notifies Transmitter of new surface confguration. Transmitter will
	 * forward the arguments, window state, and reference the buffer for
	 * image transmission.
	 *
	 * Shells are meant to call this function for remoted surfaces in
	 * the weston_surface::configure handler.
	 *
	 * XXX: Is this necessary if we have weston_surface::apply_state_signal?
	 *
	 * Essentially this is just an elaborate way to forward dx,dy.
	 */
	void
	(*surface_configure)(struct weston_transmitter_surface *txs,
			     int32_t dx, int32_t dy);

	void
	(*surface_gather_state)(struct weston_transmitter_surface *txs);

	/** Notify that surface is connected to receiver
	 *
	 * \param txr The Transmitter context.
	 * \param connected_listener Listener for connected_signal.
	 */
	void
	(*register_connection_status)(struct weston_transmitter *txr,
				      struct wl_listener *connected_listener);

	/** get weston_surface from weston_transmitter_surface
	 *
	 * \param txs The Transmitter surface.
	 */
	struct weston_surface *
	(*get_weston_surface)(struct weston_transmitter_surface *txs);
};

static inline const struct weston_transmitter_api *
weston_get_transmitter_api(struct weston_compositor *compositor)
{
	return weston_plugin_api_get(compositor, WESTON_TRANSMITTER_API_NAME,
				     sizeof(struct weston_transmitter_api));
}

#define WESTON_TRANSMITTER_IVI_API_NAME "transmitter_ivi_v1"

/** For relaying configure events from Transmitter to shell. */
typedef void (*weston_transmitter_ivi_resize_handler_t)(void *data,
							int32_t width,
							int32_t height);

/** The Transmitter IVI-shell API
 * Contains the IVI-shell specifics required to remote an ivi-surface.
 */
struct weston_transmitter_ivi_api {
	/** Set IVI-id for a transmitter surface
	 *
	 * \param txs The transmitted surface.
	 * \param ivi_id The IVI-surface id as specified by the
	 * ivi_application.surface_create request.
	 */
	void
	(*set_ivi_id)(struct weston_transmitter_surface *txs, uint32_t ivi_id);

	/** Set callback to relay configure events.
	 *
	 * \param txs The transmitted surface.
	 * \param cb The callback function pointer.
	 * \param data User data to be passed to the callback.
	 *
	 * The arguments to the callback function are user data, and width and
	 * height from the configure event from the remote compositor.
	 * The shell must relay this event to the application.
	 */
	void
	(*set_resize_callback)(struct weston_transmitter_surface *txs,
			       weston_transmitter_ivi_resize_handler_t cb,
			       void *data);
};

static inline const struct weston_transmitter_ivi_api *
weston_get_transmitter_ivi_api(struct weston_compositor *compositor)
{
	return weston_plugin_api_get(compositor,
				     WESTON_TRANSMITTER_IVI_API_NAME,
				     sizeof(struct weston_transmitter_ivi_api));
}

/** Identifies outputs created by the Transmitter by make */
#define WESTON_TRANSMITTER_OUTPUT_MAKE "Weston-Transmitter"

struct weston_transmitter {
	struct weston_compositor *compositor;
	struct wl_listener compositor_destroy_listener;
	struct wl_list remote_list; /* transmitter_remote::link */
	struct wl_listener stream_listener;
	struct wl_signal connected_signal;
	struct wl_event_loop *loop;
	struct waltham_renderer_interface *waltham_renderer;
};

struct weston_transmitter_remote {
	struct weston_transmitter *transmitter;
	struct wl_list link;
	const char *name;
	char *model;
	char *addr;
	char *port;
	int *width;
	int *height;

	enum weston_transmitter_connection_status status;
	struct wl_signal connection_status_signal;
	struct wl_signal conn_establish_signal;

	struct wl_list output_list; /* weston_transmitter_output::link */
	struct wl_list surface_list; /* weston_transmitter_surface::link */
	struct wl_list seat_list; /* weston_transmitter_seat::link */

	struct wl_listener establish_listener;
	struct wl_event_source *establish_timer; /* for establish connection */
	struct wl_event_source *retry_timer; /* for retry connection */

	struct waltham_display *display; /* waltham */
	struct wl_event_source *source;

	/* waltham */
	struct wthp_surface *wthp_surf;
	struct wthp_ivi_surface *wthp_ivi_surface;

	/* To create only one wthp_surface and wthp_ivi_surface */
	int count;
};

struct weston_transmitter_surface {
	struct weston_transmitter_remote *remote;
	struct wl_list link; /* weston_transmitter_remote::surface_list */
	struct wl_signal destroy_signal; /* data: weston_transmitter_surface */

	enum weston_transmitter_stream_status status;
	struct wl_signal stream_status_signal;

	struct weston_surface *surface;
	struct wl_listener surface_destroy_listener;
	const struct ivi_layout_interface *lyt;

	weston_transmitter_ivi_resize_handler_t resize_handler;
	void *resize_handler_data;

	int32_t attach_dx; /* wl_surface.attach(buffer, dx, dy) */
	int32_t attach_dy; /* wl_surface.attach(buffer, dx, dy) */
	struct wl_list frame_callback_list; /* weston_frame_callback::link */
	struct wl_list feedback_list; /* weston_presentation_feedback::link */

	/* waltham */
	struct wthp_blob_factory *wthp_blob;
	struct wthp_buffer *wthp_buf;
	struct wthp_ivi_application *wthp_ivi_application;
};

struct weston_transmitter_output_info {
	uint32_t subpixel; /* enum wl_output_subpixel */
	uint32_t transform; /* enum wl_output_transform */
	int32_t scale;
	int32_t x;
	int32_t y;
	int32_t width_mm;
	int32_t height_mm;
	char *model;
	struct weston_mode mode;
};

/** epoll structure */
struct watch {
	struct waltham_display *display;
	int fd;
	void (*cb)(struct watch *w, uint32_t events);
};

struct waltham_display {
	struct wth_connection *connection;
	struct watch conn_watch;
	struct wth_display *display;
	bool running;
	struct wthp_registry *registry;
	struct wthp_callback *bling;
	struct wthp_compositor *compositor;
	struct wthp_blob_factory *blob_factory;
	struct wthp_seat *seat;
	struct wthp_pointer *pointer;
	struct wthp_keyboard *keyboard;
	struct wthp_touch *touch;
	struct wthp_ivi_application *application;
	struct wtimer *fiddle_timer;
	struct weston_transmitter_remote *remote;
	char *addr;
	char *port;
};

/** a timerfd based timer */
struct wtimer {
	struct watch watch;
	void (*func)(struct wtimer *, void *);
	void *data;
};

struct ivi_layout_surface {
	struct wl_list link; /* ivi_layout::surface_list */
	struct wl_signal property_changed;
	int32_t update_count;
	uint32_t id_surface;
	struct ivi_layout *layout;
	struct weston_surface *surface;
	struct ivi_layout_surface_properties prop;

	struct {
		struct ivi_layout_surface_properties prop;
	} pending;

	struct wl_list view_list; /* ivi_layout_view::surf_link */
};

void transmitter_surface_ivi_resize(struct weston_transmitter_surface *txs,
				    int32_t width, int32_t height);

#endif /* WESTON_TRANSMITTER_API_H */
