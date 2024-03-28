/*
 * Copyright 2024 Collabora, Ltd.
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

#include "weston-test-client-helper.h"
#include "shared/xalloc.h"

#include "color-management-v1-client-protocol.h"

#define NOT_SET -99

/**
 * This is used to know where to expect the error in the test.
 */
enum error_point {
	ERROR_POINT_NONE = 0,
	ERROR_POINT_PRIMARIES_NAMED,
	ERROR_POINT_PRIMARIES,
	ERROR_POINT_TF_NAMED,
	ERROR_POINT_TF_POWER,
	ERROR_POINT_MASTERING_DISPLAY_PRIMARIES,
	ERROR_POINT_TARGET_LUM,
	ERROR_POINT_TARGET_MAX_CLL,
	ERROR_POINT_TARGET_MAX_FALL,
	ERROR_POINT_IMAGE_DESC,
	ERROR_POINT_GRACEFUL_FAILURE,
};

struct image_description {
	struct xx_image_description_v4 *xx_image_desc;
	enum image_description_status {
		CM_IMAGE_DESC_NOT_CREATED = 0,
		CM_IMAGE_DESC_READY,
		CM_IMAGE_DESC_FAILED,
	} status;

	/* For graceful failures. */
	int32_t failure_reason;
};

struct color_manager {
	struct xx_color_manager_v4 *manager;

	/* Bitfield that holds what color features are supported. If enum
	 * xx_color_manager_v4_feature v is supported, bit v will be set
	 * to 1. */
	uint32_t supported_features;

	/* Bitfield that holds what rendering intents are supported. If enum
	 * xx_color_manager_v4_render_intent v is supported, bit v will be set
	 * to 1. */
	uint32_t supported_rendering_intents;

	/* Bitfield that holds what color primaries are supported. If enum
	 * xx_color_manager_v4_primaries v is supported, bit v will be set
	 * to 1. */
	uint32_t supported_color_primaries;

	/* Bitfield that holds what transfer functions are supported. If enum
	 * xx_color_manager_v4_transfer_function v is supported, bit v will be
	 * set to 1. */
	uint32_t supported_tf;
};

struct test_case {
	int32_t primaries_named;
	const struct weston_color_gamut *primaries;
	int32_t tf_named;
	float tf_power;
	const struct weston_color_gamut *target_primaries;
	float target_min_lum;
	int32_t target_max_lum;
	int32_t target_max_cll;
	int32_t target_max_fall;
	int32_t expected_error;
	enum error_point error_point;
};

static const struct weston_color_gamut color_gamut_sRGB = {
	.primary = { { 0.64, 0.33 }, /* RGB order */
		     { 0.30, 0.60 },
		     { 0.15, 0.06 },
	},
	.white_point = { 0.3127, 0.3290 },
};

static const struct weston_color_gamut color_gamut_invalid_primaries = {
	.primary = { { -100.00, 0.33 }, /* RGB order */
		     {    0.30, 0.60 },
		     {    0.15, 0.06 },
	},
	.white_point = { 0.3127, 0.3290 },
};

static const struct weston_color_gamut color_gamut_invalid_white_point = {
	.primary = { { 0.64, 0.33 }, /* RGB order */
		     { 0.30, 0.60 },
		     { 0.15, 0.06 },
	},
	.white_point = { 1.0, 1.0 },
};

static const struct test_case my_test_cases[] = {
	{
	  /* profile with primaries */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with primaries named */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_sRGB,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with tf power */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = NOT_SET,
	  .tf_power = 2.4f,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with target primaries */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = &color_gamut_sRGB,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with tf PQ and target lum */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 2.0f,
	  .target_max_lum = 3,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with tf PQ and max cll */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = 5,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with tf PQ and max fall */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = 5,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* profile with tf PQ, target lum, max cll, max fall */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 1.0f,
	  .target_max_lum = 3,
	  .target_max_cll = 2,
	  .target_max_fall = 2,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* set primaries twice */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = &color_gamut_sRGB,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_ALREADY_SET,
	  .error_point = ERROR_POINT_PRIMARIES,
	},
	{
	  /* set tf twice */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = 2.4f,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_ALREADY_SET,
	  .error_point = ERROR_POINT_TF_POWER,
	},
	{
	  /* bad tf power exponent */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = NOT_SET,
	  .tf_power = 0.9f,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INVALID_TF,
	  .error_point = ERROR_POINT_TF_POWER,
	},
	{
	  /* bad target luminance */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 5.0f,
	  .target_max_lum = 5,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INVALID_LUMINANCE,
	  .error_point = ERROR_POINT_TARGET_LUM,
	},
	{
	  /* target luminance set but tf is not PQ */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 1.0f,
	  .target_max_lum = 2,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INCONSISTENT_SET,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* target max cll set but tf is not PQ */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = 5,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INCONSISTENT_SET,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* target max fall set but tf is not PQ */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = 5.0f,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INCONSISTENT_SET,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* target max cll set but minor than min luminance */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 6.0f,
	  .target_max_lum = 7,
	  .target_max_cll = 5,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INCONSISTENT_SET,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* target max fall set but minor than min luminance */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 6.0f,
	  .target_max_lum = 7,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = 5,
	  .expected_error = XX_IMAGE_DESCRIPTION_CREATOR_PARAMS_V4_ERROR_INCONSISTENT_SET,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* bad primaries */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_invalid_primaries,
	  .tf_named = NOT_SET,
	  .tf_power = 5.0f,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_V4_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* bad white point */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_invalid_white_point,
	  .tf_named = NOT_SET,
	  .tf_power = 5.0f,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_V4_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* bad target primaries */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = &color_gamut_invalid_primaries,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_V4_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* bad target white point */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB,
	  .tf_power = NOT_SET,
	  .target_primaries = &color_gamut_invalid_white_point,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_V4_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* max cll out of luminance range */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 2.0f,
	  .target_max_lum = 3,
	  .target_max_cll = 1,
	  .target_max_fall = NOT_SET,
	  .expected_error = XX_IMAGE_DESCRIPTION_V4_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* max fall out of luminance range */
	  .primaries_named = XX_COLOR_MANAGER_V4_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 2.0f,
	  .target_max_lum = 3,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = 1,
	  .expected_error = XX_IMAGE_DESCRIPTION_V4_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
};

static void
image_desc_ready(void *data, struct xx_image_description_v4 *xx_image_description_v4,
		 uint32_t identity)
{
	struct image_description *image_desc = data;

	image_desc->status = CM_IMAGE_DESC_READY;
}

static void
image_desc_failed(void *data, struct xx_image_description_v4 *xx_image_description_v4,
		  uint32_t cause, const char *msg)
{
	struct image_description *image_desc = data;

	image_desc->status = CM_IMAGE_DESC_FAILED;
	image_desc->failure_reason = cause;

	testlog("Failed to create image description:\n" \
		"    cause: %u, msg: %s\n", cause, msg);
}

static const struct xx_image_description_v4_listener
image_desc_iface = {
	.ready = image_desc_ready,
	.failed = image_desc_failed,
};

static struct image_description *
image_description_create(struct xx_image_description_creator_params_v4 *image_desc_creator_param)
{
	struct image_description *image_desc = xzalloc(sizeof(*image_desc));

	image_desc->xx_image_desc =
		xx_image_description_creator_params_v4_create(image_desc_creator_param);

	xx_image_description_v4_add_listener(image_desc->xx_image_desc,
					     &image_desc_iface, image_desc);

	return image_desc;
}

static void
image_description_destroy(struct image_description *image_desc)
{
	xx_image_description_v4_destroy(image_desc->xx_image_desc);
	free(image_desc);
}

static void
cm_supported_intent(void *data, struct xx_color_manager_v4 *xx_color_manager_v4,
		    uint32_t render_intent)
{
	struct color_manager *cm = data;

	cm->supported_rendering_intents |= (1 << render_intent);
}

static void
cm_supported_feature(void *data, struct xx_color_manager_v4 *xx_color_manager_v4,
		     uint32_t feature)
{
	struct color_manager *cm = data;

	cm->supported_features |= (1 << feature);
}

static void
cm_supported_tf_named(void *data, struct xx_color_manager_v4 *xx_color_manager_v4,
		      uint32_t tf)
{
	struct color_manager *cm = data;

	cm->supported_tf |= (1 << tf);
}

static void
cm_supported_primaries_named(void *data, struct xx_color_manager_v4 *xx_color_manager_v4,
			     uint32_t primaries)
{
	struct color_manager *cm = data;

	cm->supported_color_primaries |= (1 << primaries);
}

static const struct xx_color_manager_v4_listener
cm_iface = {
	.supported_intent = cm_supported_intent,
	.supported_feature = cm_supported_feature,
	.supported_tf_named = cm_supported_tf_named,
	.supported_primaries_named = cm_supported_primaries_named,
};

static void
color_manager_init(struct color_manager *cm, struct client *client)
{
	memset(cm, 0, sizeof(*cm));

	cm->manager = bind_to_singleton_global(client,
					       &xx_color_manager_v4_interface,
					       1);
	xx_color_manager_v4_add_listener(cm->manager, &cm_iface, cm);

	client_roundtrip(client);

	/* Weston supports all color features. */
	assert(cm->supported_features == ((1 << XX_COLOR_MANAGER_V4_FEATURE_ICC_V2_V4) |
					  (1 << XX_COLOR_MANAGER_V4_FEATURE_PARAMETRIC) |
					  (1 << XX_COLOR_MANAGER_V4_FEATURE_SET_PRIMARIES) |
					  (1 << XX_COLOR_MANAGER_V4_FEATURE_SET_TF_POWER) |
					  (1 << XX_COLOR_MANAGER_V4_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) |
					  (1 << XX_COLOR_MANAGER_V4_FEATURE_EXTENDED_TARGET_VOLUME)));

	/* Weston supports all rendering intents. */
	assert(cm->supported_rendering_intents == ((1 << XX_COLOR_MANAGER_V4_RENDER_INTENT_PERCEPTUAL) |
						   (1 << XX_COLOR_MANAGER_V4_RENDER_INTENT_RELATIVE) |
						   (1 << XX_COLOR_MANAGER_V4_RENDER_INTENT_SATURATION) |
						   (1 << XX_COLOR_MANAGER_V4_RENDER_INTENT_ABSOLUTE) |
						   (1 << XX_COLOR_MANAGER_V4_RENDER_INTENT_RELATIVE_BPC)));

	/* Weston supports all primaries. */
	assert(cm->supported_color_primaries == ((1 << XX_COLOR_MANAGER_V4_PRIMARIES_SRGB) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_PAL_M) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_PAL) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_NTSC) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_GENERIC_FILM) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_BT2020) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_CIE1931_XYZ) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_DCI_P3) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_DISPLAY_P3) |
						 (1 << XX_COLOR_MANAGER_V4_PRIMARIES_ADOBE_RGB)));

	/* Weston supports only a few transfer functions, and we make use of
	 * them in our tests. */
	assert(cm->supported_tf == ((1 << XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_GAMMA22) |
				    (1 << XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_GAMMA28) |
				    (1 << XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_SRGB) |
				    (1 << XX_COLOR_MANAGER_V4_TRANSFER_FUNCTION_ST2084_PQ)));
}

static void
color_manager_fini(struct color_manager *cm)
{
	xx_color_manager_v4_destroy(cm->manager);
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.renderer = WESTON_RENDERER_GL;
	setup.shell = SHELL_TEST_DESKTOP;
	setup.logging_scopes = "log,color-lcms-profiles";
	setup.refresh = HIGHEST_OUTPUT_REFRESH;

	weston_ini_setup(&setup,
			 cfgln("[core]"),
			 cfgln("color-management=true"));

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

TEST_P(create_parametric_image_description, my_test_cases)
{
	struct client *client;
	struct color_manager cm;
	struct xx_image_description_creator_params_v4 *image_desc_creator_param = NULL;
	const struct test_case *args = data;
	struct image_description *image_desc = NULL;

	client = create_client_and_test_surface(100, 100, 100, 100);
	color_manager_init(&cm, client);

	image_desc_creator_param =
		xx_color_manager_v4_new_parametric_creator(cm.manager);

	if (args->primaries_named != NOT_SET)
		xx_image_description_creator_params_v4_set_primaries_named(image_desc_creator_param,
									   args->primaries_named);
	if (args->error_point == ERROR_POINT_PRIMARIES_NAMED) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->primaries)
		xx_image_description_creator_params_v4_set_primaries(image_desc_creator_param,
								     args->primaries->primary[0].x * 10000,
								     args->primaries->primary[0].y * 10000,
								     args->primaries->primary[1].x * 10000,
								     args->primaries->primary[1].y * 10000,
								     args->primaries->primary[2].x * 10000,
								     args->primaries->primary[2].y * 10000,
								     args->primaries->white_point.x * 10000,
								     args->primaries->white_point.y * 10000);
	if (args->error_point == ERROR_POINT_PRIMARIES) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->tf_named != NOT_SET)
		xx_image_description_creator_params_v4_set_tf_named(image_desc_creator_param,
								    args->tf_named);
	if (args->error_point == ERROR_POINT_TF_NAMED) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->tf_power != NOT_SET)
		xx_image_description_creator_params_v4_set_tf_power(image_desc_creator_param,
								    args->tf_power * 10000);
	if (args->error_point == ERROR_POINT_TF_POWER) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->target_primaries)
		xx_image_description_creator_params_v4_set_mastering_display_primaries(image_desc_creator_param,
										       args->target_primaries->primary[0].x * 10000,
										       args->target_primaries->primary[0].y * 10000,
										       args->target_primaries->primary[1].x * 10000,
										       args->target_primaries->primary[1].y * 10000,
										       args->target_primaries->primary[2].x * 10000,
										       args->target_primaries->primary[2].y * 10000,
										       args->target_primaries->white_point.x * 10000,
										       args->target_primaries->white_point.y * 10000);
	if (args->error_point == ERROR_POINT_MASTERING_DISPLAY_PRIMARIES) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->target_min_lum != NOT_SET && args->target_max_lum != NOT_SET)
		xx_image_description_creator_params_v4_set_mastering_luminance(image_desc_creator_param,
									       args->target_min_lum * 10000,
									       args->target_max_lum);
	if (args->error_point == ERROR_POINT_TARGET_LUM) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->target_max_cll != NOT_SET)
		xx_image_description_creator_params_v4_set_max_cll(image_desc_creator_param,
								   args->target_max_cll);
	if (args->error_point == ERROR_POINT_TARGET_MAX_CLL) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	if (args->target_max_fall != NOT_SET)
		xx_image_description_creator_params_v4_set_max_fall(image_desc_creator_param,
								    args->target_max_fall);
	if (args->error_point == ERROR_POINT_TARGET_MAX_FALL) {
		expect_protocol_error(client, &xx_image_description_creator_params_v4_interface,
				      args->expected_error);
		goto out;
	}

	image_desc = image_description_create(image_desc_creator_param);
	image_desc_creator_param = NULL;
	if (args->error_point == ERROR_POINT_IMAGE_DESC) {
		/* We expect a protocol error from unknown object, because the
		 * image_desc_creator_param wl_proxy will get destroyed with
		 * the create call above. It is a destructor request. */
		expect_protocol_error(client, NULL, args->expected_error);
		goto out;
	}

	while (image_desc->status == CM_IMAGE_DESC_NOT_CREATED)
		assert(wl_display_dispatch(client->wl_display) >= 0);

	if (args->error_point == ERROR_POINT_GRACEFUL_FAILURE) {
		assert(image_desc->status == CM_IMAGE_DESC_FAILED);
		assert(image_desc->failure_reason == args->expected_error);
		goto out;
	}

	assert(image_desc->status == CM_IMAGE_DESC_READY);

out:
	if (image_desc)
		image_description_destroy(image_desc);
	if (image_desc_creator_param)
		xx_image_description_creator_params_v4_destroy(image_desc_creator_param);
	color_manager_fini(&cm);
	client_destroy(client);
}
