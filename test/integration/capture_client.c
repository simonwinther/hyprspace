// Immutable SHM content with four corners and a far-offset real subsurface.
#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xdg-shell.h"

enum { WIDTH = 5120, HEIGHT = 2880, CHILD_WIDTH = 320, CHILD_HEIGHT = 180, CHILD_X = 4352, CHILD_Y = 2304 };
static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_subcompositor* subcompositor;
static struct wl_shm* shm;
static struct xdg_wm_base* shell;
static struct wl_surface *parent_surface, *child_surface;
static struct wl_buffer *parent_buffer, *child_buffers[2];
static unsigned frames, phase;
static int configured;

static void ping(void* data, struct xdg_wm_base* base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};

static void global(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    (void)data;
    if (!strcmp(interface, wl_compositor_interface.name))
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, version < 4 ? version : 4);
    if (!strcmp(interface, wl_subcompositor_interface.name))
        subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    if (!strcmp(interface, wl_shm_interface.name))
        shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    if (!strcmp(interface, xdg_wm_base_interface.name)) {
        shell = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    }
}
static void removed(void* data, struct wl_registry* registry, uint32_t id) {
    (void)data;
    (void)registry;
    (void)id;
}
static const struct wl_registry_listener registry_listener = {.global = global, .global_remove = removed};

static struct wl_buffer* buffer(int width, int height, uint32_t color, int corners) {
    const size_t bytes = (size_t)width * height * 4;
    int fd = memfd_create("hs-capture", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, bytes))
        exit(3);
    uint32_t* pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        exit(4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uint32_t ink = color;
            if (corners) {
                if (x < width / 10 && y < height / 10)
                    ink = 0xffe62e2e;
                else if (x >= width * 9 / 10 && y < height / 10)
                    ink = 0xff2ee62e;
                else if (x < width / 10 && y >= height * 9 / 10)
                    ink = 0xff2e2ee6;
                else if (x >= width * 9 / 10 && y >= height * 9 / 10)
                    ink = 0xffe6e62e;
            }
            pixels[(size_t)y * width + x] = ink;
        }
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, bytes);
    struct wl_buffer* result = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, bytes);
    close(fd);
    return result;
}

static void request_frame(void);
static void frame_done(void* data, struct wl_callback* callback, uint32_t time) {
    (void)data;
    (void)time;
    wl_callback_destroy(callback);
    ++frames;
    if (!(frames % 8)) {
        phase ^= 1;
        wl_surface_attach(child_surface, child_buffers[phase], 0, 0);
        wl_surface_damage(child_surface, 0, 0, CHILD_WIDTH, CHILD_HEIGHT);
        wl_surface_commit(child_surface);
    }
    printf("frame %u %u\n", frames, phase);
    fflush(stdout);
    request_frame();
    wl_surface_commit(parent_surface);
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};
static void request_frame(void) {
    wl_callback_add_listener(wl_surface_frame(parent_surface), &frame_listener, NULL);
}

static void configure(void* data, struct xdg_surface* surface, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    if (!configured) {
        parent_buffer = buffer(WIDTH, HEIGHT, 0xff304050, 1);
        child_buffers[0] = buffer(CHILD_WIDTH, CHILD_HEIGHT, 0xff25cddd, 0);
        child_buffers[1] = buffer(CHILD_WIDTH, CHILD_HEIGHT, 0xffdd25cd, 0);
        wl_surface_attach(child_surface, child_buffers[0], 0, 0);
        wl_surface_damage(child_surface, 0, 0, CHILD_WIDTH, CHILD_HEIGHT);
        wl_surface_commit(child_surface);
        wl_surface_attach(parent_surface, parent_buffer, 0, 0);
        wl_surface_damage(parent_surface, 0, 0, WIDTH, HEIGHT);
        request_frame();
        configured = 1;
        puts("ready");
        fflush(stdout);
    }
    wl_surface_commit(parent_surface);
}
static const struct xdg_surface_listener surface_listener = {.configure = configure};
static void top_configure(void* data, struct xdg_toplevel* top, int32_t width, int32_t height, struct wl_array* states) {
    (void)data;
    (void)top;
    (void)width;
    (void)height;
    (void)states;
}
static void top_close(void* data, struct xdg_toplevel* top) {
    (void)data;
    (void)top;
    exit(0);
}
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close};

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    display = wl_display_connect(NULL);
    if (!display)
        return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !subcompositor || !shm || !shell)
        return 2;
    parent_surface = wl_compositor_create_surface(compositor);
    child_surface = wl_compositor_create_surface(compositor);
    struct wl_subsurface* child = wl_subcompositor_get_subsurface(subcompositor, child_surface, parent_surface);
    wl_subsurface_set_position(child, CHILD_X, CHILD_Y);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(shell, parent_surface);
    xdg_surface_add_listener(xdg, &surface_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_app_id(top, "hs-capture-fixture");
    xdg_toplevel_set_title(top, argv[1]);
    xdg_toplevel_set_min_size(top, WIDTH, HEIGHT);
    xdg_toplevel_set_max_size(top, WIDTH, HEIGHT);
    wl_surface_commit(parent_surface);
    while (wl_display_dispatch(display) != -1) {}
    wl_display_disconnect(display);
    return 0;
}
