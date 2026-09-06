#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "virtual-pointer.h"
#include "virtual-keyboard.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct wl_seat *seat;
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        manager = wl_registry_bind(registry, id, &zwlr_virtual_pointer_manager_v1_interface, 1);
    if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
        keyboard_manager = wl_registry_bind(registry, id, &zwp_virtual_keyboard_manager_v1_interface, 1);
    if (!strcmp(interface, wl_seat_interface.name))
        seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t id) {}
static const struct wl_registry_listener listener = {global, removed};
static uint32_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &listener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !keyboard_manager || !seat) return 2;
    struct zwlr_virtual_pointer_v1 *pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    struct zwp_virtual_keyboard_v1 *keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    struct xkb_state *keyboard_state = xkb_state_new(keymap);
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(text) + 1;
    int fd = memfd_create("hs-test-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) return 4;
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    close(fd);
    free(text);
    wl_display_roundtrip(display);
    puts("ready");
    fflush(stdout);
    uint32_t button, state, axis, source;
    int discrete;
    double delta;
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "key %u %u", &button, &state) == 2) {
            zwp_virtual_keyboard_v1_key(keyboard, milliseconds(), button, state);
            xkb_state_update_key(keyboard_state, button + 8, state ? XKB_KEY_DOWN : XKB_KEY_UP);
            zwp_virtual_keyboard_v1_modifiers(keyboard,
                xkb_state_serialize_mods(keyboard_state, XKB_STATE_MODS_DEPRESSED),
                xkb_state_serialize_mods(keyboard_state, XKB_STATE_MODS_LATCHED),
                xkb_state_serialize_mods(keyboard_state, XKB_STATE_MODS_LOCKED),
                xkb_state_serialize_layout(keyboard_state, XKB_STATE_LAYOUT_EFFECTIVE));
        } else if (sscanf(line, "axis %u %lf %d %u", &axis, &delta, &discrete, &source) == 4) {
            if (source != WL_POINTER_AXIS_SOURCE_WHEEL && delta == 0 && discrete == 0)
                zwlr_virtual_pointer_v1_axis_stop(pointer, milliseconds(), axis);
            else
                zwlr_virtual_pointer_v1_axis_discrete(pointer, milliseconds(), axis, wl_fixed_from_double(delta), discrete);
            // This compositor associates source with the most recent axis.
            zwlr_virtual_pointer_v1_axis_source(pointer, source);
        } else if (sscanf(line, "%u %u", &button, &state) != 2)
            return 3;
        else if (button)
            zwlr_virtual_pointer_v1_button(pointer, milliseconds(), button, state);
        else
            zwlr_virtual_pointer_v1_motion(pointer, milliseconds(), 0, 0);
        zwlr_virtual_pointer_v1_frame(pointer);
        if (wl_display_roundtrip(display) < 0) break;
        puts("ok");
        fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwp_virtual_keyboard_v1_destroy(keyboard);
    wl_display_roundtrip(display);
    xkb_state_unref(keyboard_state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wl_display_disconnect(display);
}
