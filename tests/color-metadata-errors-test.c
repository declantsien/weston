/*
 * Copyright 2022 Collabora, Ltd.
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
#include "weston-test-fixture-compositor.h"

#include "weston-private.h"
#include "libweston-internal.h"
#include "backend.h"
#include "id-number-allocator.h"
#include "shared/xalloc.h"

static FILE *logfile;

static int
logger(const char *fmt, va_list arg)
{
	return vfprintf(logfile, fmt, arg);
}

struct mock_color_manager {
	struct weston_color_manager base;
	struct weston_hdr_metadata_type1 *test_hdr_meta;
};

static struct weston_output_color_outcome *
mock_create_output_color_outcome(struct weston_color_manager *cm_base,
				 struct weston_output *output)
{
	struct mock_color_manager *cm = container_of(cm_base, typeof(*cm), base);
	struct weston_output_color_outcome *co;

	co = xzalloc(sizeof *co);

	co->hdr_meta = *cm->test_hdr_meta;

	return co;
}

static struct weston_color_profile *
mock_cm_ref_stock_sRGB_color_profile(struct weston_color_manager *mock_cm)
{
	struct weston_color_profile *mock_cprof;

	mock_cprof = xzalloc(sizeof(*mock_cprof));

	mock_cprof->cm = mock_cm;
	mock_cprof->ref_count = 1;
	mock_cprof->description = xstrdup("mock cprof");
	mock_cprof->id = weston_idalloc_get_id(mock_cm->compositor->color_profile_id_generator);

	return mock_cprof;
}

static void
mock_cm_destroy_color_profile(struct weston_color_profile *mock_cprof)
{
	free(mock_cprof->description);
	free(mock_cprof);
}

struct mode_testcase {
	bool color_management;
	uint32_t supported_eotf_mask;
	uint32_t supported_colorimetry_mask;
	const char *eotf_mode;
	const char *colorimetry_mode;
	enum weston_eotf_mode expected_eotf_mode;
	enum weston_colorimetry_mode expected_colorimetry_mode;
	int expected_retval;
	const char *expected_error;
};

static const struct mode_testcase mode_config_cases[] = {
	/* Defaults */
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT, NULL, NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	/* Color management off, EOTF modes */
	{
		false, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "sdr", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		false, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "hdr-gamma", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: EOTF mode hdr-gamma on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "st2084", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: EOTF mode st2084 on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "hlg", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: EOTF mode hlg on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "nonosense", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error in config for output 'mockoutput': 'nonosense' is not a valid EOTF mode. Try one of: sdr hdr-gamma st2084 hlg\n"
	},
	/* Color management on, EOTF modes */
	{
		true, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "sdr", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "hdr-gamma", NULL,
		WESTON_EOTF_MODE_TRADITIONAL_HDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "st2084", NULL,
		WESTON_EOTF_MODE_ST2084, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "hlg", NULL,
		WESTON_EOTF_MODE_HLG, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_ALL_MASK, WESTON_COLORIMETRY_MODE_DEFAULT, "nonosense", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error in config for output 'mockoutput': 'nonosense' is not a valid EOTF mode. Try one of: sdr hdr-gamma st2084 hlg\n"
	},
	/* unsupported EOTF mode */
	{
		true,
		WESTON_EOTF_MODE_SDR | WESTON_EOTF_MODE_TRADITIONAL_HDR | WESTON_EOTF_MODE_ST2084,
		WESTON_COLORIMETRY_MODE_DEFAULT, "hlg", NULL,
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: output 'mockoutput' does not support EOTF mode hlg.\n"
	},
	/* Color management off, colorimetry modes */
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "default",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "bt2020cycc",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: Colorimetry mode bt2020cycc on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "bt2020ycc",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: Colorimetry mode bt2020ycc on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "bt2020rgb",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: Colorimetry mode bt2020rgb on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "p3d65",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: Colorimetry mode p3d65 on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "p3dci",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: Colorimetry mode p3dci on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "ictcp",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: Colorimetry mode ictcp on output 'mockoutput' requires color-management=true in weston.ini\n"
	},
	{
		false, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "imagine that",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error in config for output 'mockoutput': 'imagine that' is not a valid colorimetry mode. Try one of: default bt2020cycc bt2020ycc bt2020rgb p3d65 p3dci ictcp\n"
	},
	/* Color management on, colorimetry modes */
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "default",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "bt2020cycc",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_BT2020_CYCC,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "bt2020ycc",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_BT2020_YCC,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "bt2020rgb",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_BT2020_RGB,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "p3d65",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_P3D65,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "p3dci",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_P3DCI,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "ictcp",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ICTCP,
		0, ""
	},
	{
		true, WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_ALL_MASK, NULL, "imagine that",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error in config for output 'mockoutput': 'imagine that' is not a valid colorimetry mode. Try one of: default bt2020cycc bt2020ycc bt2020rgb p3d65 p3dci ictcp\n"
	},
	/* Unsupported colorimetry mode */
	{
		true, WESTON_EOTF_MODE_SDR,
		WESTON_COLORIMETRY_MODE_DEFAULT | WESTON_COLORIMETRY_MODE_BT2020_RGB | WESTON_COLORIMETRY_MODE_BT2020_CYCC | WESTON_COLORIMETRY_MODE_P3D65,
		NULL, "ictcp",
		WESTON_EOTF_MODE_SDR, WESTON_COLORIMETRY_MODE_DEFAULT,
		-1, "Error: output 'mockoutput' does not support colorimetry mode ictcp.\n"
	},
};

static struct weston_config *
create_mode_config(const struct mode_testcase *t)
{
	struct compositor_setup setup;
	struct weston_config *wc;

	compositor_setup_defaults(&setup);
	weston_ini_setup(&setup,
			 cfgln("[output]"),
			 cfgln("name=mockoutput"),

			 t->eotf_mode ?
				cfgln("eotf-mode=%s", t->eotf_mode) :
				cfgln(""),

			 t->colorimetry_mode ?
				cfgln("colorimetry-mode=%s", t->colorimetry_mode) :
				cfgln("")
			);

	wc = weston_config_parse(setup.config_file);
	free(setup.config_file);

	return wc;
}

/*
 * Manufacture various weston.ini and check what
 * wet_output_set_eotf_mode() and wet_output_set_colorimetry_mode() says.
 * Tests for the return value and the error messages logged.
 */
TEST_P(mode_config_error, mode_config_cases)
{
	const struct mode_testcase *t = data;
	struct mock_color_manager mock_cm = {
		.base.create_output_color_outcome = mock_create_output_color_outcome,
		.base.ref_stock_sRGB_color_profile = mock_cm_ref_stock_sRGB_color_profile,
		.base.destroy_color_profile = mock_cm_destroy_color_profile,
	};
	struct weston_compositor mock_compositor = {
		.color_manager = &mock_cm.base,
		.color_profile_id_generator = weston_idalloc_create(&mock_compositor),
	};

	struct weston_config *wc;
	struct weston_config_section *section;
	int retval;
	int attached;
	char *logbuf;
	size_t logsize;
	struct weston_head mock_head = {};
	struct weston_output mock_output = {};

	mock_cm.base.compositor = &mock_compositor;

	wl_list_init(&mock_compositor.plane_list);

	weston_output_init(&mock_output, &mock_compositor, "mockoutput");
	weston_head_init(&mock_head, "mockhead");
	weston_head_set_supported_eotf_mask(&mock_head, t->supported_eotf_mask);
	weston_head_set_supported_colorimetry_mask(&mock_head, t->supported_colorimetry_mask);
	attached = weston_output_attach_head(&mock_output, &mock_head);
	assert(attached == 0);

	logfile = open_memstream(&logbuf, &logsize);
	weston_log_set_handler(logger, logger);

	wc = create_mode_config(t);
	section = weston_config_get_section(wc, "output", "name", "mockoutput");
	assert(section);

	retval = wet_output_set_eotf_mode(&mock_output, section, t->color_management);
	if (retval == 0) {
		retval = wet_output_set_colorimetry_mode(&mock_output, section,
							 t->color_management);
	}

	assert(fclose(logfile) == 0);
	logfile = NULL;

	testlog("retval %d, logs:\n%s\n", retval, logbuf);

	assert(retval == t->expected_retval);
	assert(strcmp(logbuf, t->expected_error) == 0);
	assert(weston_output_get_eotf_mode(&mock_output) == t->expected_eotf_mode);
	assert(weston_output_get_colorimetry_mode(&mock_output) == t->expected_colorimetry_mode);

	weston_config_destroy(wc);
	free(logbuf);
	weston_output_release(&mock_output);
	weston_head_release(&mock_head);
	weston_idalloc_destroy(mock_compositor.color_profile_id_generator);
}
