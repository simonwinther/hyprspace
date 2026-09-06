#include <wayland-client.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "input-method.h"

static struct zwp_input_method_manager_v2 *manager;
static struct wl_seat *seat;
static unsigned keys, modifiers;
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    if (!strcmp(interface, zwp_input_method_manager_v2_interface.name))
        manager = wl_registry_bind(registry, id, &zwp_input_method_manager_v2_interface, 1);
    if (!strcmp(interface, wl_seat_interface.name))
        seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void empty(void *data, struct zwp_input_method_v2 *ime) {}
static void surrounding(void *data, struct zwp_input_method_v2 *ime, const char *text, uint32_t cursor, uint32_t anchor) {}
static void cause(void *data, struct zwp_input_method_v2 *ime, uint32_t cause) {}
static void content(void *data, struct zwp_input_method_v2 *ime, uint32_t hint, uint32_t purpose) {}
static const struct zwp_input_method_v2_listener ime_listener = {
    .activate = empty, .deactivate = empty, .surrounding_text = surrounding,
    .text_change_cause = cause, .content_type = content, .done = empty, .unavailable = empty,
};
static void keymap(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t format, int32_t fd, uint32_t size) { close(fd); }
static void key(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) { ++keys; }
static void mods(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) { ++modifiers; }
static void repeat(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, int32_t rate, int32_t delay) {}
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {keymap, key, mods, repeat};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !seat) return 2;
    struct zwp_input_method_v2 *ime = zwp_input_method_manager_v2_get_input_method(manager, seat);
    zwp_input_method_v2_add_listener(ime, &ime_listener, NULL);
    struct zwp_input_method_keyboard_grab_v2 *grab = zwp_input_method_v2_grab_keyboard(ime);
    zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
    wl_display_roundtrip(display);
    puts("ready");
    fflush(stdout);
    struct pollfd fds[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    char line[80];
    while (1) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(fds, 2, -1) < 0) break;
        if (fds[0].revents & POLLIN && wl_display_dispatch(display) < 0) break;
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (!fgets(line, sizeof(line), stdin)) break;
            wl_display_roundtrip(display);
            if (!strcmp(line, "reset\n")) {
                keys = modifiers = 0;
                puts("ok");
            } else if (!strcmp(line, "counts\n"))
                printf("%u %u\n", keys, modifiers);
            else return 3;
            fflush(stdout);
        }
    }
    zwp_input_method_keyboard_grab_v2_release(grab);
    zwp_input_method_v2_destroy(ime);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
}
