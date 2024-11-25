/*
 * Copyright (C) 2024 Pengutronix, Michael Tretter <entwicklung@pengutronix.de>
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

#include <stdlib.h>
#include <string.h>

#include "libweston/desktop.h"
#include "libweston/zalloc.h"
#include "ivi-layout-export.h"
#include "frontend/weston.h"
#include "lua-controller.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

struct lua_controller {
	struct weston_compositor *compositor;

	struct weston_output *output;
	struct ivi_layout_layer *layer;

	lua_State *lua;

	struct wl_listener destroy_listener;

	struct wl_listener surface_created;
	struct wl_listener surface_configured;
	struct wl_listener surface_removed;

	struct wl_listener desktop_surface_configured;
	int last_surface_id;

	const struct ivi_layout_interface *ivi_layout;
};

static struct ivi_layout_layer *
lua_controller_create_layer(struct lua_controller *controller,
			    struct weston_output *output, uint32_t id)
{
	const struct ivi_layout_interface *ivi_layout = controller->ivi_layout;
	struct ivi_layout_layer *layer;

	layer = ivi_layout->layer_create_with_dimension(id, output->width, output->height);
	if (!layer)
		return NULL;

	ivi_layout->screen_add_layer(output, layer);
	ivi_layout->layer_set_destination_rectangle(layer, 0, 0,
						    output->width, output->height);
	ivi_layout->layer_set_visibility(layer, true);

	return layer;
}

static int
setup_outputs(struct lua_controller *controller)
{
	lua_State* L = controller->lua;
	struct weston_output *output;

	output = wl_container_of(controller->compositor->output_list.prev, output, link);

	lua_newtable(L);
	lua_setglobal(L, "outputs");

	lua_getglobal(L, "outputs");
	lua_pushstring(L, output->name);
	lua_setfield(L, -2, output->name);

	return 0;
}

static int
lua_controller_create_initial_layers(struct lua_controller *controller)
{
	struct weston_output *output;

	if (wl_list_length(&controller->compositor->output_list) == 0) {
		weston_log("no output available\n");
		return -1;
	}

	output = wl_container_of(controller->compositor->output_list.prev, output, link);

	controller->output = output;
	controller->layer = lua_controller_create_layer(controller, output, 1);

	setup_outputs(controller);

	return 0;
}

struct Surface {
	int width;
	int height;
};

static int
surface_get(lua_State *L) {
	struct Surface *a = lua_touserdata(L, 1);
	const char *key = NULL;

	if (!lua_isstring(L, 2)) {
		lua_pushnil(L);
		return 1;
	}

	key = lua_tostring(L, 2);
	if (strcmp(key, "width") == 0) {
		lua_pushnumber(L, a->width);
		return 1;
	} else if (strcmp(key, "height") == 0) {
		lua_pushnumber(L, a->height);
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

static int
surface_set(lua_State *L) {
	struct Surface *a = lua_touserdata(L, 1);
	const char *key = NULL;
	int value;

	if (!lua_isstring(L, 2))
		return 0;

	key = lua_tostring(L, 2);
	value = lua_tointeger(L, 3);

	if (strcmp(key, "width") == 0) {
		a->width = value;
	} else if (strcmp(key, "height") == 0) {
		a->height = value;
	}

	return 0;
}

int
ivi_print(const char *log) {
	return weston_log("%s\n", log);
}

static void
lua_call_configure_surface(lua_State* L, struct weston_surface *surface)
{
	struct Surface *s;
	const char *callback = "configure_surface";

	lua_getglobal(L, callback);
	if (!lua_isfunction(L, -1)) {
		weston_log("%s not available", callback);
		return;
	}

	s = lua_newuserdata(L, sizeof(*s));
	luaL_getmetatable(L, "ivi.surface");
	lua_setmetatable(L, -2);

	s->width = surface->width;
	s->height = surface->height;

	if (lua_pcall(L, 1, 1, 0) != LUA_OK)
		weston_log("%s failed\n", callback);

	lua_pop(L, lua_gettop(L));

	surface->width = s->width;
	surface->height = s->height;
}

static int
luaopen_surface(lua_State* L)
{
	luaL_newmetatable(L, "ivi.surface");

	lua_pushcfunction(L, surface_get);
	lua_setfield(L, -2, "__index");

	lua_pushcfunction(L, surface_set);
	lua_setfield(L, -2, "__newindex");

	return 0;
}

static void
lua_controller_create_surface(struct wl_listener *listener, void *data)
{
	struct lua_controller *controller = wl_container_of(listener, controller, surface_created);
	const struct ivi_layout_interface *ivi_layout = controller->ivi_layout;
	struct ivi_layout_surface *ivisurf = data;
	struct weston_surface *surface;
	struct weston_desktop_surface *desktop_surface;

	surface = ivi_layout->surface_get_weston_surface(ivisurf);

	desktop_surface = weston_surface_get_desktop_surface(surface);
	if (desktop_surface)
		ivi_layout->surface_set_id(ivisurf, ++controller->last_surface_id);

	ivi_layout->layer_add_surface(controller->layer, ivisurf);
}

static void
lua_controller_remove_surface(struct wl_listener *listener, void *data)
{
	struct lua_controller *controller = wl_container_of(listener, controller, surface_removed);
	const struct ivi_layout_interface *ivi_layout = controller->ivi_layout;
	(void)data;
	struct ivi_layout_surface **surfaces = NULL;
	int32_t count = 0;

	ivi_layout->get_surfaces_on_layer(controller->layer, &count, &surfaces);

	if (count > 0) {
		struct weston_seat *seat;
		struct weston_surface *surface;


		surface = ivi_layout->surface_get_weston_surface(surfaces[count - 1]);
		wl_list_for_each(seat, &controller->compositor->seat_list, link) {
			weston_seat_set_keyboard_focus(seat, surface);
		}
	}

	free(surfaces);

	ivi_layout->commit_changes();
}

static void
lua_controller_configure_surface_imp(struct lua_controller *controller,
				     struct ivi_layout_surface *ivisurf)
{
	const struct ivi_layout_interface *ivi_layout = controller->ivi_layout;
	struct weston_surface *surface;

	surface = ivi_layout->surface_get_weston_surface(ivisurf);

	lua_call_configure_surface(controller->lua, surface);

	ivi_layout->surface_set_destination_rectangle(ivisurf, 0, 0,
						      surface->width, surface->height);

	if (weston_surface_has_content(surface)) {
		ivi_layout->surface_set_source_rectangle(ivisurf, 0, 0,
							 surface->width, surface->height);
		ivi_layout->surface_set_visibility(ivisurf, true);
	}

	ivi_layout->commit_changes();
}

static void
lua_controller_configure_surface(struct wl_listener *listener, void *data)
{
	struct lua_controller *controller = wl_container_of(listener, controller,
							    surface_configured);
	struct ivi_layout_surface *ivisurf = data;

	return lua_controller_configure_surface_imp(controller, ivisurf);
}

static void
lua_controller_configure_desktop_surface(struct wl_listener *listener, void *data)
{
	struct lua_controller *controller = wl_container_of(listener, controller,
							    desktop_surface_configured);
	struct ivi_layout_surface *ivisurf = data;

	return lua_controller_configure_surface_imp(controller, ivisurf);
}

static void
lua_controller_destroy(struct wl_listener *listener, void *data)
{
	struct lua_controller *controller = wl_container_of(listener, controller, destroy_listener);
	(void)data;

	lua_close(controller->lua);

	free(controller);
}

static lua_State *
lua_controller_init_lua(struct lua_controller *controller)
{
	lua_State *L;
	struct weston_compositor *compositor = controller->compositor;
	struct weston_config *config;
	struct weston_config_section *section = NULL;
	char *script;

	config = wet_get_config(compositor);
	section = weston_config_get_section(config, "shell", NULL, NULL);
	weston_config_section_get_string(section, "script", &script, NULL);
	if (!script) {
		weston_log("Missing script for Lua controller\n");
		return NULL;
	}

	L = luaL_newstate();

	luaL_openlibs(L);
	luaopen_surface(L);
	luaopen_ivi(L);

	weston_log("Loading script %s\n", script);
	if (luaL_dofile(L, script) != LUA_OK) {
		/* FIXME Handle error */
		weston_log("%s: Error\n", script);
		free(script);
		return NULL;
	}
	free(script);

	lua_pop(L, lua_gettop(L));

	return L;
}

WL_EXPORT int
wet_module_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	struct lua_controller *controller;
	const struct ivi_layout_interface *ivi_layout;

	controller = zalloc(sizeof(*controller));
	if (!controller)
		goto err;

	ivi_layout = ivi_layout_get_api(ec);
	if (!ivi_layout) {
		weston_log("ivi_layout_interface not available\n");
		goto err;
	}
	controller->ivi_layout = ivi_layout;

	controller->compositor = ec;

	controller->lua = lua_controller_init_lua(controller);
	if (!controller->lua)
		goto err;

	if (lua_controller_create_initial_layers(controller) < 0)
		goto err;

	controller->surface_created.notify = lua_controller_create_surface;
	ivi_layout->add_listener_create_surface(&controller->surface_created);

	controller->surface_removed.notify = lua_controller_remove_surface;
	ivi_layout->add_listener_remove_surface(&controller->surface_removed);

	controller->surface_configured.notify = lua_controller_configure_surface;
	ivi_layout->add_listener_configure_surface(&controller->surface_configured);

	controller->desktop_surface_configured.notify = lua_controller_configure_desktop_surface;
	ivi_layout->add_listener_configure_desktop_surface(&controller->desktop_surface_configured);

	controller->destroy_listener.notify = lua_controller_destroy;
	wl_signal_add(&controller->compositor->destroy_signal, &controller->destroy_listener);

	ivi_layout->commit_changes();

	return 0;

err:
	lua_close(controller->lua);

	free(controller);
	return -1;
}
