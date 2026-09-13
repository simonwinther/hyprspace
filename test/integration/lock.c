#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "session-lock.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct ext_session_lock_manager_v1 *manager;
static struct ext_session_lock_v1 *lock;
static bool locked;
static unsigned keys, buttons, output_count;
static struct output {
    struct wl_output *output;
    struct wl_surface *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
    struct wl_buffer *buffer;
} outputs[16];

static void keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) { close(fd); }
static void key_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *pressed) {}
static void key_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {}
static void key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) { ++keys; }
static void modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked_mods, uint32_t group) {}
static void repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener keyboard_listener = {keymap, key_enter, key_leave, key, modifiers, repeat};
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}
static void motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) { ++buttons; }
static void axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {}
static const struct wl_pointer_listener pointer_listener = {pointer_enter, pointer_leave, motion, button, axis};
static void capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {}
static const struct wl_seat_listener seat_listener = {.capabilities = capabilities};
static void geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t width, int32_t height, int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static void mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {}
static void output_done(void *data, struct wl_output *output) {}
static void output_scale(void *data, struct wl_output *output, int32_t scale) {}
static void output_name(void *data, struct wl_output *output, const char *name) {}
static void output_description(void *data, struct wl_output *output, const char *description) {}
static const struct wl_output_listener output_listener = {geometry, mode, output_done, output_scale, output_name, output_description};
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    if (!strcmp(interface, wl_compositor_interface.name)) compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    if (!strcmp(interface, wl_shm_interface.name)) shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    if (!strcmp(interface, ext_session_lock_manager_v1_interface.name)) manager = wl_registry_bind(registry, id, &ext_session_lock_manager_v1_interface, 1);
    if (!strcmp(interface, wl_seat_interface.name)) {
        seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
    if (!strcmp(interface, wl_output_interface.name)) {
        if (output_count == 16) exit(2);
        struct output *output = &outputs[output_count++];
        output->output = wl_registry_bind(registry, id, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(output->output, &output_listener, NULL);
    }
}
static void removed(void *data, struct wl_registry *registry, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void configure(void *data, struct ext_session_lock_surface_v1 *surface, uint32_t serial, uint32_t width, uint32_t height) {
    struct output *output = data;
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    if (!width || !height || width > 8192 || height > 8192) exit(3);
    const size_t size = (size_t)width * height * 4;
    int fd = memfd_create("hs-lock", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) exit(3);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) exit(3);
    for (size_t i = 0; i < size / 4; ++i) pixels[i] = 0xff204060;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    if (output->buffer) wl_buffer_destroy(output->buffer);
    output->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(pixels, size);
    wl_surface_attach(output->surface, output->buffer, 0, 0);
    wl_surface_damage(output->surface, 0, 0, width, height);
    wl_surface_commit(output->surface);
}
static const struct ext_session_lock_surface_v1_listener surface_listener = {configure};
static void on_locked(void *data, struct ext_session_lock_v1 *lock) { locked = true; }
static void finished(void *data, struct ext_session_lock_v1 *lock) { exit(4); }
static const struct ext_session_lock_v1_listener lock_listener = {on_locked, finished};
static void command(const char *line) {
    if (!strcmp(line, "lock\n") && !lock) {
        lock = ext_session_lock_manager_v1_lock(manager);
        ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
        for (unsigned i = 0; i < output_count; ++i) {
            struct output *output = &outputs[i];
            output->surface = wl_compositor_create_surface(compositor);
            output->lock_surface = ext_session_lock_v1_get_lock_surface(lock, output->surface, output->output);
            ext_session_lock_surface_v1_add_listener(output->lock_surface, &surface_listener, output);
        }
    } else if (!strcmp(line, "unlock\n") && locked) {
        ext_session_lock_v1_unlock_and_destroy(lock);
        lock = NULL;
        locked = false;
        for (unsigned i = 0; i < output_count; ++i) {
            struct output *output = &outputs[i];
            ext_session_lock_surface_v1_destroy(output->lock_surface);
            wl_surface_destroy(output->surface);
            wl_buffer_destroy(output->buffer);
            output->surface = NULL;
            output->lock_surface = NULL;
            output->buffer = NULL;
        }
    } else if (strcmp(line, "counts\n")) exit(5);
    wl_display_roundtrip(display);
    printf("{\"locked\":%s,\"keys\":%u,\"buttons\":%u}\n", locked ? "true" : "false", keys, buttons);
    fflush(stdout);
}
int main(void) {
    display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !seat || !manager || !output_count) return 2;
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, NULL);
    wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, NULL);
    wl_display_roundtrip(display);
    puts("ready");
    fflush(stdout);
    struct pollfd fds[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    for (;;) {
        while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(fds, 2, -1) < 0) { wl_display_cancel_read(display); break; }
        if (fds[0].revents & POLLIN) wl_display_read_events(display);
        else wl_display_cancel_read(display);
        if (wl_display_dispatch_pending(display) < 0) break;
        if (fds[1].revents & POLLIN) {
            char line[80];
            if (!fgets(line, sizeof(line), stdin)) break;
            command(line);
        }
        if (fds[1].revents & POLLHUP) break;
    }
    wl_display_disconnect(display);
}
