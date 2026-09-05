#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xdg-shell.h"
#include "xdg-activation.h"
#include "session-lock.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static struct xdg_activation_v1 *activation;
static struct ext_session_lock_manager_v1 *locker;
static struct wl_display *display;
struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *top;
    struct wl_buffer *buffer;
    int width, height;
    char after_token[128];
};
static struct window windows[32];
static unsigned count;
static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    if (!strcmp(interface, wl_compositor_interface.name)) compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    if (!strcmp(interface, wl_shm_interface.name)) shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    if (!strcmp(interface, xdg_wm_base_interface.name)) {
        shell = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    }
    if (!strcmp(interface, xdg_activation_v1_interface.name)) activation = wl_registry_bind(registry, id, &xdg_activation_v1_interface, 1);
    if (!strcmp(interface, ext_session_lock_manager_v1_interface.name)) locker = wl_registry_bind(registry, id, &ext_session_lock_manager_v1_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct window *window = data;
    xdg_surface_ack_configure(surface, serial);
    if (window->buffer) wl_buffer_destroy(window->buffer);
    const size_t size = window->width * window->height * 4;
    int fd = memfd_create("hs-activation", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) exit(3);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) exit(4);
    for (size_t i = 0; i < size / 4; ++i) pixels[i] = 0xff406080;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    window->buffer = wl_shm_pool_create_buffer(pool, 0, window->width, window->height, window->width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(pixels, size);
    wl_surface_attach(window->surface, window->buffer, 0, 0);
    wl_surface_damage(window->surface, 0, 0, window->width, window->height);
    wl_surface_commit(window->surface);
    if (window->after_token[0]) {
        xdg_activation_v1_activate(activation, window->after_token, window->surface);
        window->after_token[0] = 0;
    }
}
static const struct xdg_surface_listener surface_listener = {configure};
static void top_configure(void *data, struct xdg_toplevel *top, int32_t width, int32_t height, struct wl_array *states) {
    struct window *window = data;
    if (width > 0) window->width = width;
    if (height > 0) window->height = height;
}
static void top_close(void *data, struct xdg_toplevel *top) { exit(0); }
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close};
static void locked(void *data, struct ext_session_lock_v1 *lock) { puts("locked"); fflush(stdout); }
static void finished(void *data, struct ext_session_lock_v1 *lock) { puts("finished"); fflush(stdout); }
static const struct ext_session_lock_v1_listener lock_listener = {locked, finished};
static void command(char *line) {
    char action[16], title[80], token[128];
    int fields = sscanf(line, "%15s %79s %127s", action, title, token);
    if (fields == 1 && !strcmp(action, "lock")) {
        struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(locker);
        ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
    } else if (fields == 3 && !strcmp(action, "activate")) {
        unsigned index = strtoul(title, NULL, 10);
        if (index < count) xdg_activation_v1_activate(activation, token, windows[index].surface);
    } else if (fields == 3 && (!strcmp(action, "before") || !strcmp(action, "after")) && count < 32) {
        struct window *window = &windows[count++];
        window->width = 320;
        window->height = 240;
        window->surface = wl_compositor_create_surface(compositor);
        window->xdg = xdg_wm_base_get_xdg_surface(shell, window->surface);
        xdg_surface_add_listener(window->xdg, &surface_listener, window);
        window->top = xdg_surface_get_toplevel(window->xdg);
        xdg_toplevel_add_listener(window->top, &top_listener, window);
        xdg_toplevel_set_app_id(window->top, "hs-activation-fixture");
        xdg_toplevel_set_title(window->top, title);
        if (strcmp(token, "-")) {
            if (!strcmp(action, "before")) xdg_activation_v1_activate(activation, token, window->surface);
            else strcpy(window->after_token, token);
        }
        wl_surface_commit(window->surface);
    } else exit(5);
    wl_display_roundtrip(display);
    puts("ok");
    fflush(stdout);
}
int main(void) {
    display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !shell || !activation || !locker) return 2;
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
            char line[256];
            if (!fgets(line, sizeof(line), stdin)) break;
            command(line);
        }
        if (fds[1].revents & POLLHUP) break;
    }
    wl_display_disconnect(display);
}
