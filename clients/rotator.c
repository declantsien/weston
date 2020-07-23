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

#include <stdint.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <wayland-client.h>
#include <libweston/transform.h>
#include <libweston/config-parser.h>

#include "weston-rotator-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "shared/helpers.h"
#include "shared/xalloc.h"

#ifdef USE_LIBIIO
#include <iio.h>
#define BUFFER_SIZE		16
#define ROTATION_THRESHOLD	128
#define BUFFER_SIZE_READ	ROTATION_THRESHOLD
#define CUMULATIVE_READS	10
#endif

#define OPT_TRANSFORM_USER_SPEC		1
#define OPT_OUTPUT_NAME_USER_SPEC	1 << 1
#define DEFAULT_SAMPLING_FREQUENCY	13
#define MAX_TRANSFORMS			8 /* normal, rotate-90, etc */

static int running = 1;

//#define DEBUG

struct client;

struct config {
	char *device_name;
	char *channel_x_name, *channel_y_name;
	char *sampling_frequency_name;
	char *sampling_frequency_available_name;
	int wanted_sampling_frequency;
	int threshold;
	int cumulative_reads;
	struct wl_array allowed_rotations;
};

struct output_data {
	struct wl_output *output;
	struct client *client;
	int width;
	int height;
	char *output_name;

	struct wl_list link;	/** client::output_list */
};

struct xdg_output_v1_info {
	struct zxdg_output_v1 *xdg_output;
	struct output_data *output;

	struct {
		int32_t x, y;
		int32_t width, height;
	} logical;

	char *name, *description;
	struct wl_list link;	/** client::xdg_output_list */
};

struct client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct weston_rotator *rotator;

	struct wl_list output_list;	/** output_data::link */
	struct wl_list xdg_output_list;	/** xdg_output_v1_info::link */

	bool sent_done;
	uint32_t transform_done;
};


#ifdef USE_LIBIIO
struct accelerometer {
	struct iio_context *context;
	struct iio_device *device;
	struct iio_channel *channel_x, *channel_y;
	long long int sampling_frequency;
};

static struct config *
read_configuration(const char *device_name)
{
	struct config *configuration;
	const char *config_file;
	struct weston_config *config;
	struct weston_config_section *s;

	char *allowed_rotations;
	int transform, *array_items;

	config_file = weston_config_get_name_from_env();
	config = weston_config_parse(config_file);
	if (config == NULL)
		return NULL;

	s = weston_config_get_section(config, "accelerometer", "name",
				      device_name);
	if (s == NULL) {
		weston_config_destroy(config);
		return NULL;
	}

	configuration = zalloc(sizeof(*configuration));
	if (!configuration) {
		weston_config_destroy(config);
		return NULL;
	}

	weston_config_section_get_string(s, "name",
			&configuration->device_name, NULL);

	weston_config_section_get_string(s, "channel_x",
			&configuration->channel_x_name, "accel_x");
	weston_config_section_get_string(s, "channel_y",
			&configuration->channel_y_name, "accel_y");

	weston_config_section_get_string(s, "sampling_frequency_attr",
				 &configuration->sampling_frequency_name,
				 "sampling_frequency");
	weston_config_section_get_string(s, "sampling_frequency_attr_available",
			 &configuration->sampling_frequency_available_name,
			 "sampling_frequency_available");
	weston_config_section_get_int(s, "wanted_sampling_frequency",
			&configuration->wanted_sampling_frequency, 0);

	weston_config_section_get_int(s, "threshold",
			      &configuration->threshold, ROTATION_THRESHOLD);
	weston_config_section_get_string(s, "allowed_rotations",
					 &allowed_rotations, NULL);
	weston_config_section_get_int(s, "cumulative_reads",
			      &configuration->cumulative_reads, CUMULATIVE_READS);

	weston_config_destroy(config);

	wl_array_init(&configuration->allowed_rotations);
	if (allowed_rotations) {
		size_t rot_token_idx = 0;

		char *rot_token = strtok(allowed_rotations, ",");
		while (rot_token && ++rot_token_idx) {
			if (weston_parse_transform(rot_token, &transform) == -1)
				break;

			array_items =
				wl_array_add(&configuration->allowed_rotations,
					     sizeof(int));
			*array_items = transform;
			rot_token = strtok(NULL, ",");
		}
		free(allowed_rotations);
	} else {
		array_items =
			wl_array_add(&configuration->allowed_rotations,
				     sizeof(int));
		*array_items = WL_OUTPUT_TRANSFORM_NORMAL;

		array_items =
			wl_array_add(&configuration->allowed_rotations,
				     sizeof(int));
		*array_items = WL_OUTPUT_TRANSFORM_90;

		array_items =
			wl_array_add(&configuration->allowed_rotations,
				     sizeof(int));
		*array_items = WL_OUTPUT_TRANSFORM_180;

		array_items =
			wl_array_add(&configuration->allowed_rotations,
				     sizeof(int));
		*array_items = WL_OUTPUT_TRANSFORM_270;
	}

	return configuration;
}

static void
destroy_configuration(struct config *config)
{
	if (config) {
		free(config->device_name);
		free(config->channel_x_name);
		free(config->channel_y_name);
		free(config->sampling_frequency_name);
		free(config->sampling_frequency_available_name);

		wl_array_release(&config->allowed_rotations);
		free(config);
	}
}

static struct accelerometer *
initialize_iio(struct config *config)
{
	struct accelerometer *accel;
	int ret;

	char dst_sampling_available_freq[BUFFER_SIZE_READ];
	unsigned long int first_sampling_available_freq = 0;
	size_t freq_available_len;

	accel = zalloc(sizeof *accel);
	if (!accel)
		return NULL;

	accel->context = iio_create_local_context();
	if (!accel->context)
		goto err_context;

	if (config->device_name)
		accel->device = iio_context_find_device(accel->context,
							config->device_name);

	if (!accel->device) {
		fprintf(stderr, "couldn't find device %s\n",
				config->device_name);
		goto err_device;
	}

	accel->channel_x = iio_device_find_channel(accel->device,
						   config->channel_x_name, false);
	if (!accel->channel_x) {
		fprintf(stderr, "couldn't find x channel %s\n",
				config->channel_x_name);
		goto err_channel;
	}

	accel->channel_y = iio_device_find_channel(accel->device,
						   config->channel_y_name, false);
	if (!accel->channel_y) {
		fprintf(stderr, "couldn't find y channel %s\n",
				config->channel_y_name);
		goto err_channel;
	}

	freq_available_len = iio_device_attr_read(accel->device,
				     config->sampling_frequency_available_name,
				     dst_sampling_available_freq,
				     sizeof(dst_sampling_available_freq));
	if (freq_available_len > 0) {
		char *str_first_available_freq =
			strchr(dst_sampling_available_freq, ' ');
		if (str_first_available_freq)
			first_sampling_available_freq =
				strtoul(str_first_available_freq, NULL, 10);
	}

	if (config->wanted_sampling_frequency) {
		ret = iio_device_attr_write_longlong(accel->device,
						     config->sampling_frequency_name,
						     config->wanted_sampling_frequency);
		if (ret < 0) {
			/* try to use the first available freq if one found */
			if (first_sampling_available_freq) {
				config->wanted_sampling_frequency =
					first_sampling_available_freq;
				ret = iio_device_attr_write_longlong(accel->device,
					     config->sampling_frequency_name,
					     config->wanted_sampling_frequency);
				if (ret < 0) {
					fprintf(stderr, "couldn't set sampling frequency to "
							"%d\n", config->wanted_sampling_frequency);
					goto err_channel;
				}
			} else {
				fprintf(stderr, "couldn't set sampling frequency to "
						"%d\n", config->wanted_sampling_frequency);
				goto err_channel;
			}
		}
	}

	ret = iio_device_attr_read_longlong(accel->device,
					    config->sampling_frequency_name,
					    &accel->sampling_frequency);
	if (ret < 0) {
		if (first_sampling_available_freq)
			accel->sampling_frequency = first_sampling_available_freq;
		else
			accel->sampling_frequency = DEFAULT_SAMPLING_FREQUENCY;
		fprintf(stderr, "couldn't retrieve sampling frequency, "
				"defaulting to %lld\n",
				accel->sampling_frequency);
	}

	fprintf(stdout, "Sampling for %s at frequency %lld, threshold %d, "
			"cumulative_reads %d\n", config->device_name,
			accel->sampling_frequency, config->threshold,
			config->cumulative_reads);

	return accel;

err_channel:
err_device:
	iio_context_destroy(accel->context);
err_context:
	free(accel);
	return NULL;
}

static void
destroy_accelerometer(struct accelerometer *accel)
{
	if (accel->context)
		iio_context_destroy(accel->context);

	free(accel);
}

static bool
capture_rotation(struct accelerometer *accel, int *x, int *y)
{
	char buf[BUFFER_SIZE];
	ssize_t ret;

	ret = iio_channel_attr_read(accel->channel_x, "raw", buf, BUFFER_SIZE);
	if (ret < 0) {
		iio_strerror(-ret, buf, BUFFER_SIZE);
		fprintf(stderr, "%s\n", buf);
		return false;
	}
	*x = atoi(buf);

	ret = iio_channel_attr_read(accel->channel_y, "raw", buf, BUFFER_SIZE);
	if (ret < 0) {
		iio_strerror(-ret, buf, BUFFER_SIZE);
		fprintf(stderr, "%s\n", buf);
		return false;
	}
	*y = atoi(buf);

	return true;
}

static uint32_t
get_transform(int x, int y, int threshold)
{
	if (y > threshold && y > x)
		return WL_OUTPUT_TRANSFORM_NORMAL;
	if (x > threshold && x > y)
		return WL_OUTPUT_TRANSFORM_90;
	if (y < -threshold && y < x)
		return WL_OUTPUT_TRANSFORM_180;
	if (x < -threshold && x < y)
		return WL_OUTPUT_TRANSFORM_270;

	return UINT32_MAX;
}

/* someone might customize this even further as to use a percentage
 * rather than the biggest/highest value
 */
static uint32_t
handle_accelometer_filter_data(struct wl_array *passed_transforms)
{
	uint32_t *trans;
	uint32_t maximums[MAX_TRANSFORMS] = {};
	uint32_t trans_max = WL_OUTPUT_TRANSFORM_NORMAL;

	wl_array_for_each(trans, passed_transforms) {
		maximums[*trans]++;
	}

	for (size_t i = 0; i < ARRAY_LENGTH(maximums); i++) {
#ifdef DEBUG
		fprintf(stdout, "maximums[%lu]=%u\n", i, maximums[i]);
#endif
		if (maximums[i] > trans_max) {
			trans_max = i;
		}
	}

	return trans_max;
}

static int
handle_accelometer_reading(struct client *client)
{
	const char *accelerometer_device;
	struct accelerometer *accel = NULL;
	struct config *config = NULL;
	int ret = 0;
	int reads_idx = 0;
	int x, y;

	uint32_t transform, previous_transform;
	uint32_t *allowed_transform;

	struct wl_array transforms_read;
	uint32_t *transform_array_item;

	wl_array_init(&transforms_read);

	accelerometer_device = getenv("WESTON_ACCELEROMETER");
	if (accelerometer_device == NULL) {
		fprintf(stderr, "compositor must pass an accelerometer device.\n");
		return -1;
	}

	config = read_configuration(accelerometer_device);
	if (!config) {
		fprintf(stderr, "failed to read configuration.\n");
		return -1;
	}

	accel = initialize_iio(config);
	if (!accel) {
		fprintf(stderr, "failed to initialize iio subsystem\n");
		ret = -1;
		goto out_err;
	}

	previous_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	while (running && ret != -1) {
		bool found = false;

		if (!capture_rotation(accel, &x, &y)) {
			fprintf(stderr, "failed to capture accelerometer values\n");
			break;
		}

		transform = get_transform(x, y, config->threshold);

		if (transform == UINT32_MAX) {
			fprintf(stderr, "Invalid transform received!\n");
			continue;
		}


		wl_array_for_each(allowed_transform, &config->allowed_rotations) {
			if (transform == *allowed_transform) {
				found = true;
				break;
			}
		}

		if (!found) {
			fprintf(stderr, "Transform not found to be allowed\n");
			continue;
		}

		if (reads_idx++ > config->cumulative_reads) {
			transform =
				handle_accelometer_filter_data(&transforms_read);
			reads_idx = 0;
			wl_array_release(&transforms_read);
			wl_array_init(&transforms_read);
		} else {
			transform_array_item =
				wl_array_add(&transforms_read, sizeof(uint32_t));
			*transform_array_item = transform;
			continue;
		}

		if (transform != previous_transform) {
			weston_rotator_rotate(client->rotator,
					      accelerometer_device, NULL, transform);

			while (!client->sent_done)
				wl_display_roundtrip(client->display);

			if (client->transform_done != UINT32_MAX &&
			    client->transform_done != transform)
				fprintf(stdout, "Compositor could not set %s orientation\n",
					weston_transform_to_string(client->transform_done));
			client->sent_done = false;
		}
		previous_transform = transform;
	}

out_err:
	if (accel)
		destroy_accelerometer(accel);
	if (config)
		destroy_configuration(config);

	wl_array_release(&transforms_read);

	return ret;
}
#else
static int
handle_accelometer_reading(struct client *client)
{
	fprintf(stderr, "weston-rotator client has been built "
			"without libiio-dev support\n");
	return 0;
}
#endif

static void
add_xdg_output_v1_info(struct client *client,
                       struct output_data *output);


static void
display_handle_geometry(void *data, struct wl_output *wl_output,
		int x, int y, int physical_width, int physical_height,
		int subpixel, const char *make, const char *model, int transform)
{
	(void) wl_output;
	(void) x;
	(void) y;
	(void) physical_width;
	(void) physical_height;
	(void) subpixel;
	(void) data;
}

static void
display_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int width, int height, int refresh)
{
	(void) data;
	(void) wl_output;
	(void) flags;
	(void) width;
	(void) height;

	(void) refresh;
}

static void
display_handle_scale(void *data, struct wl_output *wl_output, int scale)
{
	(void) data;
	(void) wl_output;
	(void) scale;
}

static void
display_handle_done(void *data, struct wl_output *wl_output)
{
        (void) data;
        (void) wl_output;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode,
	display_handle_done,
	display_handle_scale
};

static void
handle_xdg_output_v1_logical_position(void *data, struct zxdg_output_v1 *output,
                                      int32_t x, int32_t y)
{
	struct xdg_output_v1_info *xdg_output = data;
	xdg_output->logical.x = x;
	xdg_output->logical.y = y;
}

static void
handle_xdg_output_v1_logical_size(void *data, struct zxdg_output_v1 *output,
                                      int32_t width, int32_t height)
{
	struct xdg_output_v1_info *xdg_output = data;
	xdg_output->logical.width = width;
	xdg_output->logical.height = height;
}

static void
handle_xdg_output_v1_done(void *data, struct zxdg_output_v1 *output)
{
	/* Don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
handle_xdg_output_v1_name(void *data, struct zxdg_output_v1 *output,
                          const char *name)
{
	struct xdg_output_v1_info *xdg_output = data;
	xdg_output->name = strdup(name);
}

static void
handle_xdg_output_v1_description(void *data, struct zxdg_output_v1 *output,
                          const char *description)
{
	struct xdg_output_v1_info *xdg_output = data;
	xdg_output->description = strdup(description);
}

static const struct zxdg_output_v1_listener xdg_output_v1_listener = {
	.logical_position = handle_xdg_output_v1_logical_position,
	.logical_size = handle_xdg_output_v1_logical_size,
	.done = handle_xdg_output_v1_done,
	.name = handle_xdg_output_v1_name,
	.description = handle_xdg_output_v1_description,
};

static void
add_xdg_output_v1_info(struct client *client,
                       struct output_data *output)
{
	struct xdg_output_v1_info *xdg_output = zalloc(sizeof(*xdg_output));

	wl_list_insert(&client->xdg_output_list, &xdg_output->link);

	xdg_output->xdg_output =
		zxdg_output_manager_v1_get_xdg_output(client->xdg_output_manager,
						      output->output);

	zxdg_output_v1_add_listener(xdg_output->xdg_output,
				    &xdg_output_v1_listener, xdg_output);
	xdg_output->output = output;
}

static void
rotator_done(void *data, struct weston_rotator *weston_rotator, uint32_t transform)
{
	struct client *client = data;
	client->sent_done = true;
	client->transform_done = transform;
}

static const struct weston_rotator_listener rotator_listener = {
	rotator_done,
};

static void
handle_global(void *data, struct wl_registry *registry,
		uint32_t id, const char *interface, uint32_t version)
{
	struct client *client = data;

	if (strcmp(interface, "weston_rotator") == 0) {
		client->rotator = wl_registry_bind(registry, id,
						   &weston_rotator_interface,
						   version);
		if (client->rotator)
			weston_rotator_add_listener(client->rotator,
						    &rotator_listener, client);
	} else if (strcmp(interface, "wl_output") == 0) {
		struct output_data *output_data = zalloc(sizeof(*output_data));
                output_data->output = wl_registry_bind(registry, id,
						       &wl_output_interface,
						       version);
		output_data->client = client;
                wl_output_add_listener(output_data->output,
				       &output_listener, output_data);
		wl_list_insert(&client->output_list, &output_data->link);
	} else if(strcmp(interface, "zxdg_output_manager_v1") == 0) {
		client->xdg_output_manager = wl_registry_bind(registry, id,
				&zxdg_output_manager_v1_interface, version);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove
};

static void
signal_int(int sig, siginfo_t *si, void *unused)
{
	running = 0;
}

static void
print_usage_and_exit(void)
{
	fprintf(stderr, "Usage: weston-rotator [OPTIONS]\n");

	fprintf(stderr, "  -o,--output <output_name>    specify the output that "
			"needs to rotate\n");
	fprintf(stderr, "  -t,--transform <transform>   specify the transform: "
			"normal, rotate-90,rotate-180, rotoate-270, flipped-90, "
			"flipped-180, flipped-270\n");

	exit(EXIT_FAILURE);
}

static struct client *
client_create(void)
{
	struct client *client = zalloc(sizeof(*client));

	wl_list_init(&client->output_list);
	wl_list_init(&client->xdg_output_list);
	client->transform_done = UINT32_MAX;	/** not set */
	client->sent_done = false;

	client->display = wl_display_connect(NULL);
	if (client->display == NULL) {
		fprintf(stderr, "Failed to connect server\n");
		free(client);
		return NULL;
	}

	client->registry = wl_display_get_registry(client->display);
	wl_registry_add_listener(client->registry, &registry_listener, client);

	wl_display_dispatch(client->display);
	wl_display_roundtrip(client->display);

	return client;
}

static void
client_destroy(struct client *client)
{

	if (client->rotator)
		weston_rotator_destroy(client->rotator);

	if (client->xdg_output_manager) {
		struct xdg_output_v1_info *output;
		wl_list_for_each(output, &client->xdg_output_list, link)
			zxdg_output_v1_destroy(output->xdg_output);

		zxdg_output_manager_v1_destroy(client->xdg_output_manager);
	}

	wl_registry_destroy(client->registry);
	wl_display_disconnect(client->display);

	free(client);
}

static int
handle_user_interactive(struct client *client, uint32_t transform, const char *output_name)
{
	struct xdg_output_v1_info *output;
	struct output_data *output_data;
	struct wl_output *wl_output = NULL;
	int ret = 0;

	if (!client->xdg_output_manager) {
		fprintf(stderr, "compositor doesn't support xdg_output\n");
		return -1;
	}

	wl_list_for_each(output_data, &client->output_list, link)
		add_xdg_output_v1_info(client, output_data);

	/* do another round-trip for xdg_output */
	wl_display_roundtrip(client->display);

	/* verify we have some xdg_outputs now ... */
	wl_list_for_each(output, &client->xdg_output_list, link) {
		if (output->name &&
		    strcmp(output->name, output_name) == 0) {
			wl_output = output->output->output;
			break;
		}
	}

	if (!wl_output) {
		fprintf(stderr, "Couldn't find an output named %s\n", output_name);
		return -1;
	}

	fprintf(stdout, "Setting output %s for '%s' orientation\n", output_name,
			weston_transform_to_string(transform));
	weston_rotator_rotate(client->rotator,
			      NULL, wl_output, transform);

	while (!client->sent_done)
		wl_display_roundtrip(client->display);

	if (client->transform_done != UINT32_MAX)
		if (client->transform_done == transform)
			fprintf(stdout, "Compositor set %s orientation for output %s\n",
				weston_transform_to_string(client->transform_done),
				output_name);

	return ret;
}


int main(int argc, char *argv[])
{
	const char *output_name = NULL;
	int opts = 0;
	int ret = 0;
	int c, option_index;
	uint32_t transform;
	char *rotate_transform;

	struct client *client;

	struct sigaction sa;

	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = signal_int;
	sigaction(SIGINT, &sa, NULL);

	static struct option long_options[] = {
		{"transform", 	required_argument, 0,  't' },
		{"output", 	required_argument, 0,  'o' },
		{"help",	no_argument      , 0,  'h' },
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "ht:o:",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 't':
			rotate_transform = optarg;
			opts |= OPT_TRANSFORM_USER_SPEC;
			break;
		case 'o':
			output_name = optarg;
			opts |= OPT_OUTPUT_NAME_USER_SPEC;
			break;
		default:
			print_usage_and_exit();
		}
	}

	if ((opts & OPT_TRANSFORM_USER_SPEC) &&
	    weston_parse_transform(rotate_transform, &transform) == -1) {
		fprintf(stderr, "Failed to parse the transform\n");
		return -1;
	}

	client = client_create();
	if (!client)
		return -1;

	if (!client->rotator) {
		fprintf(stderr, "compositor doesn't support rotator\n");
		ret = -1;
		goto out;
	}

	if (opts & OPT_TRANSFORM_USER_SPEC && !output_name) {
		fprintf(stderr, "specified a transform manually but not output specified\n");
		ret = -1;
		goto out;
	}

	if ((opts & OPT_TRANSFORM_USER_SPEC) &&
	    (opts & OPT_OUTPUT_NAME_USER_SPEC)) {
		ret = handle_user_interactive(client, transform, output_name);
	} else {
		ret = handle_accelometer_reading(client);
	}
out:
	client_destroy(client);
	return ret;
}
