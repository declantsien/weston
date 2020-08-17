#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdbool.h>
#include <drm_fourcc.h>

#include <libweston/libweston.h>
#include <libweston/backend.h>
#include <libweston/backend-headless.h>

#ifdef BUILD_HEADLESS_GBM
#include <gbm.h>
#endif

#include "shared/helpers.h"
#include "linux-explicit-synchronization.h"
#include "linux-dmabuf.h"
#include "presentation-time-server-protocol.h"
#include <libweston/windowed-output-api.h>

enum headless_renderer_type {
	HEADLESS_NOOP,
	HEADLESS_PIXMAN,
	HEADLESS_GL,
	HEADLESS_GL_GBM,
};

struct headless_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct weston_seat fake_seat;
	enum headless_renderer_type renderer_type;

	struct gl_renderer_interface *glri;

	int drm_fd;
	struct gbm_device *gbm;
};

struct headless_head {
	struct weston_head base;
};

struct headless_output {
	struct weston_output base;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;
	uint32_t *image_buf;
	pixman_image_t *image;

	struct gbm_surface *gbm_surface;
	uint32_t gbm_format;
	uint32_t gbm_bo_flags;
};

static const uint32_t headless_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static inline struct headless_head *
to_headless_head(struct weston_head *base)
{
	return container_of(base, struct headless_head, base);
}

static inline struct headless_output *
to_headless_output(struct weston_output *base)
{
	return container_of(base, struct headless_output, base);
}

static inline struct headless_backend *
to_headless_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct headless_backend, base);
}

#ifdef BUILD_HEADLESS_GBM
bool
gbm_create_device_headless(struct headless_backend *b);

int
headless_output_repaint_gbm(struct headless_output *output,
			    pixman_region32_t *damage);

int
headless_gl_renderer_init_gbm(struct headless_backend *b);

int
headless_output_enable_gl_gbm(struct headless_output *output);

void
headless_output_disable_gl_gbm(struct headless_output *output);
#else
inline static bool
gbm_create_device_headless(struct headless_backend *b)
{
	return false;
}

inline static int
headless_output_repaint_gbm(struct headless_output *output,
                            pixman_region32_t *damage)
{
	return -1;
}

inline static int
headless_gl_renderer_init_gbm(struct headless_backend *b)
{
	return -1;
}

inline static int
headless_output_enable_gl_gbm(struct headless_output *output)
{
	return -1;
}

inline static void
headless_output_disable_gl_gbm(struct headless_output *output)
{
}
#endif

