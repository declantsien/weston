/*
 * Copyright © 2020 Collabora, Ltd.
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

#include <libweston/libweston.h>
#include "weston.h"	/* to spawn the client */
#include "weston-rotator-server-protocol.h"
#include "shared/helpers.h"
#include "shared/xalloc.h"
#include <libweston/weston-log.h>

/* WL_OUTPUT */
#include <wayland-server.h>

/*
 * the client will poll-out the IIO sysfs device and send back to this the
 * WL_OUTPUT transformation which will the request handler will use to set the
 * mode
 */

struct autorotator_process {
	char *accelerometer;
	struct weston_output *output;
	struct wl_client *client;
	struct weston_process process;

	struct wl_list link;		/** weston_rotator::process_list */
};

struct weston_rotator_ {
	struct weston_compositor *compositor;
	struct wl_global *global;
	struct wl_resource *resource;
	uint32_t transform;		/** current transform value */
	struct wl_listener destroy_listener;

	struct wl_list process_list;	/** autorotator_process::link */
};

static void
weston_rotate_done(void *data)
{
	struct weston_rotator_ *rotator = data;
	weston_rotator_send_done(rotator->resource, rotator->transform);
}

static void
weston_do_rotate(struct wl_client *client, struct wl_resource *resource,
		 const char *accelerometer_device,
		 struct wl_resource *wl_output, uint32_t transform)
{
	struct weston_rotator_ *rotator = wl_resource_get_user_data(resource);
	struct weston_output *output;

	/* both is not alright, so avoid handle it at all */
	if (wl_output && accelerometer_device) {
		/* send an invalid transform, client should check */
		weston_rotator_send_done(resource, UINT32_MAX);
		return;
	}

	rotator->transform = transform;

	if (wl_output) {
		struct weston_head *head = weston_head_from_resource(wl_output);
		output = weston_head_get_output(head);

		if (output->suspend_repaint) {
			weston_log("Output %s is in rotation process. Refusing"
					"to rotate\n", output->name);
			weston_rotator_send_done(resource, UINT32_MAX);
			return;
		}

		weston_log("Rotator performing rotation %s on output %s\n",
				weston_transform_to_string(transform), output->name);
		weston_rotator_rotate(output, transform,
				      weston_rotate_done, rotator);
		return;
	}

	/* with no output set, search until we find one */
	wl_list_for_each(output, &rotator->compositor->output_list, link) {
		if (output->oac && output->oac->accelerometer &&
		    strcmp(output->oac->accelerometer, accelerometer_device) == 0) {

			if (output->suspend_repaint) {
				weston_log("Output %s is in rotation process. Refusing"
						"to rotate\n", output->name);
				weston_rotator_send_done(resource, UINT32_MAX);
				return;
			}

			weston_log("Rotator performing rotation %s on output %s\n",
					weston_transform_to_string(transform), output->name);
			weston_rotator_rotate(output, transform,
					      weston_rotate_done, rotator);
			break;
		}
	}
}

struct weston_rotator_interface weston_rotator_implementation = {
	weston_do_rotate,
};

static void
weston_rotator_bind_rotator(struct wl_client *client,
			    void *data, uint32_t version, uint32_t id)
{
	struct weston_rotator_ *rotator = data;
	struct autorotator_process *process;
	struct wl_resource *resource;
	bool debug_enabled =
		weston_compositor_is_debug_protocol_enabled(rotator->compositor);
	bool found;

	resource = wl_resource_create(client, &weston_rotator_interface, 1, id);

	if (!resource) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_NO_MEMORY,
				       "weston_rotator bind failed: out-of-memory");
		return;
	}

	found = false;
	wl_list_for_each(process, &rotator->process_list, link)
		if (client == process->client)
			found = true;

	if (!found && !debug_enabled) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "rotator failed: permission denied");
		return;
	}

	rotator->resource = resource;
	wl_resource_set_implementation(resource, &weston_rotator_implementation,
				       data, NULL);
}

static void
weston_rotator_sigchld(struct weston_process *process, int status)
{
	struct autorotator_process *autorotator_process =
		container_of(process, struct autorotator_process, process);

	autorotator_process->client = NULL;
}

static void
weston_rotator_launch_autorotator(struct weston_rotator_ *rotator)
{
	struct autorotator_process *process;
	char *rotator_exe;

	const char *old_device = getenv("WESTON_ACCELEROMETER");
	rotator_exe = wet_get_bindir_path("weston-rotator");

	wl_list_for_each(process, &rotator->process_list, link) {
		setenv("WESTON_ACCELEROMETER", process->accelerometer, 1);
		weston_log("Launching rotator to handle %s\n", process->accelerometer);
		process->client = weston_client_launch(rotator->compositor,
						       &process->process,
						       rotator_exe,
						       weston_rotator_sigchld);
	}

	if (old_device)
		setenv("WESTON_ACCELEROMETER", old_device, 1);
	else
		unsetenv("WESTON_ACCELEROMETER");
}

static void
weston_rotator_destroy(struct wl_listener *listener, void *data)
{
	struct weston_output *output;
	struct autorotator_process *process, *next;
	struct weston_rotator_ *rotator =
		container_of(listener, struct weston_rotator_, destroy_listener);

	wl_list_for_each(output, &rotator->compositor->output_list, link) {
		/* we might not have an output->oac if we're using the cmd line
		 * client */
		if (output->oac) {
			free(output->oac->accelerometer);
		}
		free(output->oac);
	}

	wl_list_for_each_safe(process, next, &rotator->process_list, link) {
		free(process->accelerometer);
		free(process);
	}

	wl_global_destroy(rotator->global);
	free(rotator);
}

static void
get_output_from_rotator(struct weston_rotator_ *rotator, const char *output_name,
			char *accel_name)
{
	struct weston_output *output;

	wl_list_for_each(output, &rotator->compositor->output_list, link) {
		if (output->name && strcmp(output->name, output_name) == 0) {
			struct autorotator_process *ap;

			output->oac = zalloc(sizeof(*output->oac));
			output->oac->accelerometer = accel_name;

			wl_list_for_each(ap, &rotator->process_list, link) {
				if (strcmp(ap->accelerometer, accel_name) == 0) {
					ap->output = output;
					break;
				}
			}

		}
	}
}

static void
weston_rotator_read_configuration(struct weston_rotator_ *rotator)
{
	struct weston_config *config = wet_get_config(rotator->compositor);
	struct weston_config_section *section = NULL;
	struct autorotator_process *process;
	char *output_name, *accelerometer_name;
	const char *section_name;

	while (weston_config_next_section(config, &section, &section_name)) {
		if (strcmp(section_name, "accelerometer") == 0) {
			weston_config_section_get_string(section, "name",
							 &accelerometer_name, NULL);

			if (!accelerometer_name)
				continue;

			process = zalloc(sizeof(*process));
			if (process == NULL)
				return;

			process->accelerometer = accelerometer_name;
			wl_list_insert(&rotator->process_list, &process->link);
		}
	}

	if (wl_list_empty(&rotator->process_list))
		return;

	while (weston_config_next_section(config, &section, &section_name)) {
		if (strcmp(section_name, "output") == 0) {
			weston_config_section_get_string(section, "name",
							 &output_name, NULL);
			if (!output_name)
				continue;

			weston_config_section_get_string(section, "accelerometer",
							 &accelerometer_name, NULL);

			if (!accelerometer_name) {
				free(output_name);
				continue;
			}

			get_output_from_rotator(rotator, output_name,
						accelerometer_name);
			free(output_name);
		}
	}
}

WL_EXPORT void
weston_rotator_create(struct weston_compositor *compositor)
{
	struct weston_rotator_ *rotator;

	rotator = zalloc(sizeof(*rotator));
	if (rotator == NULL)
		return;

	rotator->compositor = compositor;
	wl_list_init(&rotator->process_list);

	weston_rotator_read_configuration(rotator);

	rotator->global = wl_global_create(compositor->wl_display,
					   &weston_rotator_interface, 
					   1, rotator, weston_rotator_bind_rotator);
	if (!rotator->global)
		return;


	rotator->destroy_listener.notify = weston_rotator_destroy;
	wl_signal_add(&compositor->destroy_signal, &rotator->destroy_listener);

	weston_rotator_launch_autorotator(rotator);
}
