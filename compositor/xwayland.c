/*
 * Copyright © 2011 Intel Corporation
 * Copyright © 2016 Giulio Camuffo
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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

#include <libweston/libweston.h>
#include "compositor/weston.h"
#include <libweston/xwayland-api.h>
#include "shared/helpers.h"
#include "shared/os-compatibility.h"

#define MAX_XWAYLAND_ARGS_COUNT 15

#ifdef HAVE_XWAYLAND_LISTENFD
#  define LISTEN_STR "-listenfd"
#else
#  define LISTEN_STR "-listen"
#endif

struct wet_xwayland {
	struct weston_compositor *compositor;
	const struct weston_xwayland_api *api;
	struct weston_xwayland *xwayland;
	struct wl_event_source *display_fd_source;
	struct wl_client *client;
	int wm_fd;
	struct weston_process process;
};

static int
handle_display_fd(int fd, uint32_t mask, void *data)
{
	struct wet_xwayland *wxw = data;
	char buf[64];
	ssize_t n;

	/* xwayland exited before being ready, don't finish initialization,
	 * the process watcher will cleanup */
	if (!(mask & WL_EVENT_READABLE))
		goto out;

	/* Xwayland writes to the pipe twice, so if we close it too early
	 * it's possible the second write will fail and Xwayland shuts down.
	 * Make sure we read until end of line marker to avoid this. */
	n = read(fd, buf, sizeof buf);
	if (n < 0 && errno != EAGAIN) {
		weston_log("read from Xwayland display_fd failed: %s\n",
			   strerror(errno));
		goto out;
	}
	/* Returning 1 here means recheck and call us again if required. */
	if (n <= 0 || (n > 0 && buf[n - 1] != '\n'))
		return 1;

	wxw->api->xserver_loaded(wxw->xwayland, wxw->client, wxw->wm_fd);

out:
	wl_event_source_remove(wxw->display_fd_source);
	close(fd);

	return 0;
}

static pid_t
spawn_xserver(void *user_data, const char *display, int abstract_fd, int unix_fd)
{
	struct wet_xwayland *wxw = user_data;
	pid_t pid;
	char s[12], abstract_fd_str[12], unix_fd_str[12], wm_fd_str[12];
	char display_fd_str[12];
	int sv[2], wm[2], fd, display_fd[2];
	char *xserver = NULL;
	bool disable_ac = false;
	struct weston_config *config = wet_get_config(wxw->compositor);
	struct weston_config_section *section;
	struct wl_event_loop *loop;
	const char *argv[MAX_XWAYLAND_ARGS_COUNT];
	int argc = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		weston_log("wl connection socketpair failed\n");
		return 1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0) {
		weston_log("X wm connection socketpair failed\n");
		return 1;
	}

	if (pipe(display_fd) < 0) {
		weston_log("pipe creation for displayfd failed\n");
		return 1;
	}

	if (os_fd_set_cloexec(display_fd[0]) != 0) {
		weston_log("failed setting remaining end of displayfd as cloexec\n");
		return 1;
	}

	pid = fork();
	switch (pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		fd = dup(sv[1]);
		if (fd < 0)
			goto fail;
		snprintf(s, sizeof s, "%d", fd);
		setenv("WAYLAND_SOCKET", s, 1);

		if (abstract_fd >= 0) {
			fd = dup(abstract_fd);
			if (fd < 0)
				goto fail;
			snprintf(abstract_fd_str, sizeof abstract_fd_str, "%d", fd);
		}
		fd = dup(unix_fd);
		if (fd < 0)
			goto fail;
		snprintf(unix_fd_str, sizeof unix_fd_str, "%d", fd);
		fd = dup(wm[1]);
		if (fd < 0)
			goto fail;
		snprintf(wm_fd_str, sizeof wm_fd_str, "%d", fd);
		snprintf(display_fd_str, sizeof display_fd_str, "%d", display_fd[1]);

		section = weston_config_get_section(config,
						    "xwayland", NULL, NULL);
		weston_config_section_get_string(section, "path",
						 &xserver, XSERVER_PATH);
		weston_config_section_get_bool(section, "disable-access-control",
						&disable_ac, false);

		argv[argc++] = xserver;
		argv[argc++] = display;
		argv[argc++] = "-rootless";
		argv[argc++] = "-core";
		argv[argc++] = LISTEN_STR;
		argv[argc++] = unix_fd_str;
		argv[argc++] = "-displayfd";
		argv[argc++] = display_fd_str;
		argv[argc++] = "-wm";
		argv[argc++] = wm_fd_str;
		argv[argc++] = "-terminate";

		if (abstract_fd >= 0) {
			argv[argc++] = LISTEN_STR;
			argv[argc++] = abstract_fd_str;
		} else {
			argv[argc++] = "-nolisten";
			argv[argc++] = "local";
		}

		if (disable_ac) {
			argv[argc++] = "-ac";
		}
		argv[argc] = NULL;

		if (execv(xserver, (char *const *)argv) < 0) {
			int i, e = errno;

			weston_log("Failed to launch Xwayland(");
			for (i = 0; i < argc; i++)
				weston_log_continue("%s ", argv[i]);
			weston_log_continue(") due to %s\n", strerror(e));
		}
	fail:
		_exit(EXIT_FAILURE);

	default:
		close(sv[1]);
		wxw->client = wl_client_create(wxw->compositor->wl_display, sv[0]);

		close(wm[1]);
		wxw->wm_fd = wm[0];

		/* During initialization the X server will round trip
		 * and block on the wayland compositor, so avoid making
		 * blocking requests (like xcb_connect_to_fd) until
		 * it's done with that. */
		close(display_fd[1]);
		loop = wl_display_get_event_loop(wxw->compositor->wl_display);
		wxw->display_fd_source =
			wl_event_loop_add_fd(loop, display_fd[0], WL_EVENT_READABLE,
					handle_display_fd, wxw);

		wxw->process.pid = pid;
		wet_watch_process(wxw->compositor, &wxw->process);
		break;

	case -1:
		weston_log("Failed to fork to spawn xserver process\n");
		break;
	}

	return pid;
}

static void
xserver_cleanup(struct weston_process *process, int status)
{
	struct wet_xwayland *wxw =
		container_of(process, struct wet_xwayland, process);

	wxw->api->xserver_exited(wxw->xwayland, status);
	wxw->client = NULL;
}

int
wet_load_xwayland(struct weston_compositor *comp)
{
	const struct weston_xwayland_api *api;
	struct weston_xwayland *xwayland;
	struct wet_xwayland *wxw;
	struct weston_config *config = wet_get_config(comp);
	struct weston_config_section *section;
	bool disable_abstract_fd;

	if (weston_compositor_load_xwayland(comp) < 0)
		return -1;

	api = weston_xwayland_get_api(comp);
	if (!api) {
		weston_log("Failed to get the xwayland module API.\n");
		return -1;
	}

	xwayland = api->get(comp);
	if (!xwayland) {
		weston_log("Failed to get the xwayland object.\n");
		return -1;
	}

	wxw = zalloc(sizeof *wxw);
	if (!wxw)
		return -1;

	section = weston_config_get_section(config,
					    "xwayland", NULL, NULL);
	weston_config_section_get_bool(section, "disable-abstract-fd",
					&disable_abstract_fd, false);
	wxw->compositor = comp;
	wxw->api = api;
	wxw->xwayland = xwayland;
	wxw->process.cleanup = xserver_cleanup;
	if (api->listen(xwayland, disable_abstract_fd, wxw, spawn_xserver) < 0)
		return -1;

	return 0;
}
