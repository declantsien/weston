/*
 * Copyright © 2012 Intel Corporation
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include <linux/input.h>

#include "window.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "shared/helpers.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

enum compose_state {
	state_normal,
	state_compose
};

struct compose_seq {
	uint32_t keys[4];

	const char *text;
};

struct simple_im;

typedef void (*keyboard_input_key_handler_t)(struct simple_im *keyboard,
					     uint32_t serial,
					     uint32_t time, uint32_t key, uint32_t unicode,
					     enum wl_keyboard_key_state state);

struct simple_im {
	struct zwp_input_method_manager_v2 *input_method_manager;
	struct zwp_input_method_v2 *input_method;
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwp_input_method_keyboard_grab_v2 *keyboard_grab;
  struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
  struct zwp_virtual_keyboard_v1 *virtual_keyboard;
	enum compose_state compose_state;
	struct compose_seq compose_seq;

	struct xkb_context *xkb_context;

	uint32_t modifiers;

	struct xkb_keymap *keymap;
	struct xkb_state *state;
	xkb_mod_mask_t control_mask;
	xkb_mod_mask_t alt_mask;
	xkb_mod_mask_t shift_mask;

	keyboard_input_key_handler_t key_handler;

	uint32_t serial;
};

static const struct compose_seq compose_seqs[] = {
	{ { XKB_KEY_quotedbl, XKB_KEY_A, 0 },  "Ä" },
	{ { XKB_KEY_quotedbl, XKB_KEY_O, 0 },  "Ö" },
	{ { XKB_KEY_quotedbl, XKB_KEY_U, 0 },  "Ü" },
	{ { XKB_KEY_quotedbl, XKB_KEY_a, 0 },  "ä" },
	{ { XKB_KEY_quotedbl, XKB_KEY_o, 0 },  "ö" },
	{ { XKB_KEY_quotedbl, XKB_KEY_u, 0 },  "ü" },
	{ { XKB_KEY_apostrophe, XKB_KEY_A, 0 },  "Á" },
	{ { XKB_KEY_apostrophe, XKB_KEY_a, 0 },  "á" },
	{ { XKB_KEY_slash, XKB_KEY_O, 0 },     "Ø" },
	{ { XKB_KEY_slash, XKB_KEY_o, 0 },     "ø" },
	{ { XKB_KEY_less, XKB_KEY_3, 0 },  "♥" },
	{ { XKB_KEY_A, XKB_KEY_A, 0 },  "Å" },
	{ { XKB_KEY_A, XKB_KEY_E, 0 },  "Æ" },
	{ { XKB_KEY_O, XKB_KEY_C, 0 },  "©" },
	{ { XKB_KEY_O, XKB_KEY_R, 0 },  "®" },
	{ { XKB_KEY_s, XKB_KEY_s, 0 },  "ß" },
	{ { XKB_KEY_a, XKB_KEY_e, 0 },  "æ" },
	{ { XKB_KEY_a, XKB_KEY_a, 0 },  "å" },
};

static const uint32_t ignore_keys_on_compose[] = {
	XKB_KEY_Shift_L,
	XKB_KEY_Shift_R
};

static void
handle_surrounding_text(void *data,
			struct zwp_input_method_v2 *input_method,
			const char *text,
			uint32_t cursor,
			uint32_t anchor)
{
	fprintf(stderr, "Surrounding text updated: %s\n", text);
}

static void
handle_content_type(void *data,
		    struct zwp_input_method_v2 *input_method,
		    uint32_t hint,
		    uint32_t purpose)
{
	fprintf(stderr, "Content type event: hint: %d, purpose: %d\n", hint,
			purpose);
}

static void
handle_done_event(void *data,
		    struct zwp_input_method_v2 *input_method)
{
	fprintf(stderr, "Done event received.\n");
}

static void
input_method_keyboard_grab_keymap(void *data,
			     struct zwp_input_method_keyboard_grab_v2 *grab,
			     uint32_t format,
			     int32_t fd,
			     uint32_t size)
{
	struct simple_im *simple_im = data;
	char *map_str;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}

	simple_im->keymap =
		xkb_keymap_new_from_string(simple_im->xkb_context,
					   map_str,
					   XKB_KEYMAP_FORMAT_TEXT_V1,
					   XKB_KEYMAP_COMPILE_NO_FLAGS);

	munmap(map_str, size);
	close(fd);

	if (!simple_im->keymap) {
		fprintf(stderr, "Failed to compile keymap\n");
		return;
	}

	simple_im->state = xkb_state_new(simple_im->keymap);
	if (!simple_im->state) {
		fprintf(stderr, "Failed to create XKB state\n");
		xkb_keymap_unref(simple_im->keymap);
		return;
	}

	simple_im->control_mask =
		1 << xkb_keymap_mod_get_index(simple_im->keymap, "Control");
	simple_im->alt_mask =
		1 << xkb_keymap_mod_get_index(simple_im->keymap, "Mod1");
	simple_im->shift_mask =
		1 << xkb_keymap_mod_get_index(simple_im->keymap, "Shift");

	zwp_virtual_keyboard_v1_keymap(simple_im->virtual_keyboard,
			format, fd, size);
}

static void
input_method_keyboard_grab_key(void *data,
			  struct zwp_input_method_keyboard_grab_v2 *grab,
			  uint32_t serial,
			  uint32_t time,
			  uint32_t key,
			  uint32_t state_w)
{
	struct simple_im *simple_im = data;
	uint32_t code;
	uint32_t num_syms;
	const xkb_keysym_t *syms;
	xkb_keysym_t sym;
	enum wl_keyboard_key_state state = state_w;

	if (!simple_im->state)
		return;

	code = key + 8;
	num_syms = xkb_state_key_get_syms(simple_im->state, code, &syms);

	sym = XKB_KEY_NoSymbol;
	if (num_syms == 1)
		sym = syms[0];

	if (!simple_im->key_handler)
	 return;

	simple_im->key_handler(simple_im, serial, time, key, sym, state);
}

static void
input_method_keyboard_grab_modifiers(void *data,
				struct zwp_input_method_keyboard_grab_v2 * grab,
				uint32_t serial,
				uint32_t mods_depressed,
				uint32_t mods_latched,
				uint32_t mods_locked,
				uint32_t group)
{
	struct simple_im *simple_im = data;
	xkb_mod_mask_t mask;

	xkb_state_update_mask(simple_im->state, mods_depressed,
			      mods_latched, mods_locked, 0, 0, group);
	mask = xkb_state_serialize_mods(simple_im->state,
					XKB_STATE_MODS_DEPRESSED |
					XKB_STATE_MODS_LATCHED);

	simple_im->modifiers = 0;
	if (mask & simple_im->control_mask)
		simple_im->modifiers |= MOD_CONTROL_MASK;
	if (mask & simple_im->alt_mask)
		simple_im->modifiers |= MOD_ALT_MASK;
	if (mask & simple_im->shift_mask)
		simple_im->modifiers |= MOD_SHIFT_MASK;

  zwp_virtual_keyboard_v1_modifiers(simple_im->virtual_keyboard,
		mods_depressed, mods_latched, mods_locked, group);
}

static void input_method_keyboard_grab_repeat_info(void *data,
    struct zwp_input_method_keyboard_grab_v2 *grab,
    int32_t rate, int32_t delay)
{
	// TODO
}

static const struct zwp_input_method_keyboard_grab_v2_listener input_method_keyboard_grab_listener = {
	.keymap = input_method_keyboard_grab_keymap,
	.key = input_method_keyboard_grab_key,
	.modifiers = input_method_keyboard_grab_modifiers,
	.repeat_info = input_method_keyboard_grab_repeat_info,
};

static void
handle_input_method_activate(void *data,
		      struct zwp_input_method_v2 *input_method)
{
	struct simple_im *simple_im = data;

	simple_im->compose_state = state_normal;

	simple_im->serial = 0;

	simple_im->keyboard_grab = zwp_input_method_v2_grab_keyboard(
			simple_im->input_method);
	zwp_input_method_keyboard_grab_v2_add_listener(simple_im->keyboard_grab,
				 &input_method_keyboard_grab_listener, simple_im);
}

static void
handle_input_method_deactivate(void *data,
			struct zwp_input_method_v2 *input_method)
{
	struct simple_im *simple_im = data;
	zwp_input_method_keyboard_grab_v2_release(simple_im->keyboard_grab);
	zwp_input_method_v2_destroy(simple_im->input_method);
}

static void
handle_text_change_cause(void *data,
			struct zwp_input_method_v2 *input_method,
			uint32_t cause)
{
	fprintf(stderr, "Text change cause event: %d\n", cause);
}

static void
handle_input_method_unavailable(void *data,
			struct zwp_input_method_v2 *input_method)
{
	// TODO: clean-up everything
}

static const struct zwp_input_method_v2_listener input_method_listener = {
	.activate = handle_input_method_activate,
	.deactivate = handle_input_method_deactivate,
	.surrounding_text = handle_surrounding_text,
	.text_change_cause = handle_text_change_cause,
	.content_type = handle_content_type,
	.done = handle_done_event,
	.unavailable = handle_input_method_unavailable,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
	struct simple_im *simple_im = data;

	if (!strcmp(interface, zwp_input_method_manager_v2_interface.name)) {
		simple_im->input_method_manager =
			wl_registry_bind(registry, name,
					 &zwp_input_method_manager_v2_interface, 1);
		if (!simple_im->input_method_manager) {
			fprintf(stderr, "No input_method_manager global\n");
			exit(-1);
		}	

		simple_im->input_method =
			zwp_input_method_manager_v2_get_input_method(
				simple_im->input_method_manager, simple_im->seat);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		simple_im->seat = wl_registry_bind(registry, name,
					 &wl_seat_interface, 7);
	} else if (!strcmp(interface,
				zwp_virtual_keyboard_manager_v1_interface.name)) {
		simple_im->virtual_keyboard_manager = wl_registry_bind(registry, name,
					 &zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static int
compare_compose_keys(const void *c1, const void *c2)
{
	const struct compose_seq *cs1 = c1;
	const struct compose_seq *cs2 = c2;
	int i;

	for (i = 0; cs1->keys[i] != 0 && cs2->keys[i] != 0; i++) {
		if (cs1->keys[i] != cs2->keys[i])
			return cs1->keys[i] - cs2->keys[i];
	}

	if (cs1->keys[i] == cs2->keys[i]
	    || cs1->keys[i] == 0)
		return 0;

	return cs1->keys[i] - cs2->keys[i];
}

static void
simple_im_key_handler(struct simple_im *simple_im,
		      uint32_t serial, uint32_t time, uint32_t key, uint32_t sym,
		      enum wl_keyboard_key_state state)
{
	if (sym == XKB_KEY_Insert &&
	    state == WL_KEYBOARD_KEY_STATE_RELEASED &&
	    simple_im->compose_state == state_normal) {
		simple_im->compose_state = state_compose;
		memset(&simple_im->compose_seq, 0, sizeof(struct compose_seq));
		return;
	}

	if (simple_im->compose_state == state_normal) {
		zwp_virtual_keyboard_v1_key(simple_im->virtual_keyboard,
			time, key, state);
		return;
	}

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	uint32_t i = 0;
	for (i = 0; i < ARRAY_LENGTH(ignore_keys_on_compose); i++) {
		if (sym == ignore_keys_on_compose[i]) {
			return;
		}
	}

	for (i = 0; simple_im->compose_seq.keys[i] != 0; i++);

	simple_im->compose_seq.keys[i] = sym;

	struct compose_seq *cs;
	cs = bsearch(&simple_im->compose_seq, compose_seqs,
					ARRAY_LENGTH(compose_seqs),
					sizeof(compose_seqs[0]), compare_compose_keys);

	if (cs && (cs->keys[i + 1] == 0)) {
		// no more composing keys so the sequence is now done
		zwp_input_method_v2_set_preedit_string(simple_im->input_method,
				"", 0, 0);
		zwp_input_method_v2_commit_string(simple_im->input_method, cs->text);

		simple_im->compose_state = state_normal;
		zwp_input_method_v2_commit(simple_im->input_method, serial);
		return;
	}

	uint32_t j = 0, idx = 0;
	char text[64] = {0};
	for (; j <= i; j++) {
		idx += xkb_keysym_to_utf8(simple_im->compose_seq.keys[j], text + idx,
				sizeof(text) - idx);
	}

	if (cs) {
		// we are still composing and not done. Set preedit string to indicate that
		zwp_input_method_v2_set_preedit_string(simple_im->input_method,
								 text,
								 0,
								 strlen(text));
	} else {
		// the key was not what we expected to get so we just
		// enter what we had in preedit before and give up
		zwp_input_method_v2_set_preedit_string(simple_im->input_method,
				"", 0, 0);
		zwp_input_method_v2_commit_string(simple_im->input_method, text);

		simple_im->compose_state = state_normal;
	}

	zwp_input_method_v2_commit(simple_im->input_method, serial);
}

static void
initialise_simple_im(struct simple_im *simple_im)
{
	zwp_input_method_v2_add_listener(simple_im->input_method,
		&input_method_listener, simple_im);

	simple_im->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (simple_im->xkb_context == NULL) {
		fprintf(stderr, "Failed to create XKB context\n");
		exit(-1);
	}

	simple_im->key_handler = simple_im_key_handler;

	simple_im->virtual_keyboard =
	zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
				simple_im->virtual_keyboard_manager, simple_im->seat);
	if (simple_im->virtual_keyboard == NULL) {
		fprintf(stderr, "Failed to create virtual keyboard\n");
		exit(-1);
	}
}

static void
destroy_simple_im(struct simple_im *simple_im)
{
	if (simple_im->keyboard_grab) {
		zwp_input_method_keyboard_grab_v2_release(simple_im->keyboard_grab);
		zwp_input_method_keyboard_grab_v2_destroy(simple_im->keyboard_grab);
	}

	if (simple_im->input_method)
		zwp_input_method_v2_destroy(simple_im->input_method);
}

int
main(int argc, char *argv[])
{
	struct simple_im simple_im;
	int ret = 0;

	memset(&simple_im, 0, sizeof(simple_im));

	simple_im.display = wl_display_connect(NULL);
	if (simple_im.display == NULL) {
		fprintf(stderr, "Failed to connect to server: %s\n",
			strerror(errno));
		return -1;
	}

	simple_im.registry = wl_display_get_registry(simple_im.display);
	wl_registry_add_listener(simple_im.registry,
				 &registry_listener, &simple_im);
	wl_display_roundtrip(simple_im.display);
	if (simple_im.input_method == NULL) {
		fprintf(stderr, "No input_method global\n");
		return -1;
	}

	initialise_simple_im(&simple_im);

	while (ret != -1)
		ret = wl_display_dispatch(simple_im.display);

	destroy_simple_im(&simple_im);

	if (ret == -1) {
		fprintf(stderr, "Dispatch error: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}
