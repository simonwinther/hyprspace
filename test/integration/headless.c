// Private render host for the unmodified Hyprland integration compositor.
// There is no DRM, Wayland or libinput backend: it cannot open desktop windows
// or claim physical outputs and input devices.
#define WLR_USE_UNSTABLE
#include "xdg-shell-protocol.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

struct host {
    struct wl_display*       display;
    struct wlr_scene*        scene;
    struct wlr_scene_output* output;
    struct wlr_xdg_shell*    shell;
    struct wl_event_source*  heartbeat;
    struct wl_listener       frame;
    struct wl_listener       new_toplevel;
    unsigned                 occupied;
};

struct toplevel {
    struct wlr_xdg_toplevel* xdg;
    struct host*             host;
    unsigned                 slot;
    struct wl_listener       commit;
    struct wl_listener       destroy;
};

static void commit(struct wl_listener* listener, void* data) {
    (void)data;
    struct toplevel* top = wl_container_of(listener, top, commit);
    if (top->xdg->base->initial_commit) {
        wlr_xdg_toplevel_set_size(top->xdg, 960, 600);
        wlr_xdg_toplevel_set_activated(top->xdg, true);
        // Hyprland 0.56.2 / Aquamarine 0.14 submits its bootstrap buffer before
        // acknowledging this configure. Accept that first buffer for its output
        // surfaces, as a Hyprland parent does. Protocol tests still run inside
        // the unmodified child compositor, not against this render host.
        if (top->xdg->app_id && strcmp(top->xdg->app_id, "aquamarine") == 0)
            top->xdg->base->configured = true;
    }
}

static void destroy(struct wl_listener* listener, void* data) {
    (void)data;
    struct toplevel* top = wl_container_of(listener, top, destroy);
    wl_list_remove(&top->commit.link);
    wl_list_remove(&top->destroy.link);
    top->host->occupied &= ~(1u << top->slot);
    free(top);
}

static void new_toplevel(struct wl_listener* listener, void* data) {
    struct host*     host = wl_container_of(listener, host, new_toplevel);
    struct toplevel* top  = calloc(1, sizeof(*top));
    if (!top) {
        wl_display_terminate(host->display);
        return;
    }
    top->xdg  = data;
    top->host = host;
    while (top->slot < 3 && (host->occupied & (1u << top->slot)))
        ++top->slot;
    struct wlr_scene_tree* tree = wlr_scene_xdg_surface_create(&host->scene->tree, top->xdg->base);
    if (top->slot == 3 || !tree) {
        free(top);
        wl_display_terminate(host->display);
        return;
    }
    host->occupied |= 1u << top->slot;
    // All three outputs remain unoccluded in memory, so frame callbacks keep
    // running even while the physical desktop is busy or on another workspace.
    wlr_scene_node_set_position(&tree->node, (int)top->slot * 960, 0);
    top->commit.notify = commit;
    wl_signal_add(&top->xdg->base->surface->events.commit, &top->commit);
    top->destroy.notify = destroy;
    wl_signal_add(&top->xdg->events.destroy, &top->destroy);
    // wlroots emits new_toplevel during the initial surface commit. A listener
    // added during that emission may not observe the commit already in flight.
    if (top->xdg->base->initialized)
        commit(&top->commit, NULL);
}

static void frame(struct wl_listener* listener, void* data) {
    (void)data;
    struct host* host = wl_container_of(listener, host, frame);
    if (!wlr_scene_output_commit(host->output, NULL)) {
        wlr_log(WLR_ERROR, "Headless host frame failed");
        wl_display_terminate(host->display);
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(host->output, &now);
}

static int terminate(int signum, void* data) {
    (void)signum;
    wl_display_terminate(data);
    return 0;
}

static int heartbeat(void* data) {
    struct host* host = data;
    // Aquamarine 0.14 can leave an output's initial requests buffered until
    // another parent event arrives. Protocol pings provide that traffic without
    // inventing input events or changing the compositor under test.
    struct wlr_xdg_client* client;
    wl_list_for_each(client, &host->shell->clients, link) xdg_wm_base_send_ping(client->resource, wl_display_next_serial(host->display));
    wl_event_source_timer_update(host->heartbeat, 100);
    return 0;
}

int main(void) {
    wlr_log_init(WLR_INFO, NULL);
    struct host host = {0};
    host.display     = wl_display_create();
    if (!host.display)
        return 1;
    struct wl_event_loop* loop    = wl_display_get_event_loop(host.display);
    struct wlr_backend*   backend = wlr_headless_backend_create(loop);
    if (!backend)
        return 1;
    // The renderer opens a render node, never a KMS device or input backend.
    setenv("WLR_RENDERER", "gles2", 1);
    struct wlr_renderer* renderer = wlr_renderer_autocreate(backend);
    if (!renderer || wlr_renderer_get_drm_fd(renderer) < 0)
        return 1;
    struct wlr_allocator* allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator || !wlr_renderer_init_wl_shm(renderer, host.display))
        return 1;
    if (!wlr_compositor_create(host.display, 6, renderer) || !wlr_subcompositor_create(host.display) || !wlr_data_device_manager_create(host.display) ||
        !wlr_linux_dmabuf_v1_create_with_renderer(host.display, 5, renderer) || !wlr_seat_create(host.display, "headless"))
        return 1;
    struct wlr_xdg_shell* shell = wlr_xdg_shell_create(host.display, 6);
    host.shell                  = shell;
    host.scene                  = wlr_scene_create();
    if (!shell || !host.scene)
        return 1;
    host.new_toplevel.notify = new_toplevel;
    wl_signal_add(&shell->events.new_toplevel, &host.new_toplevel);
    if (!wlr_backend_start(backend))
        return 1;
    struct wlr_output* output = wlr_headless_add_output(backend, 2880, 600);
    if (!output || !wlr_output_init_render(output, allocator, renderer))
        return 1;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, 2880, 600, 60000);
    bool enabled = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    if (!enabled)
        return 1;
    wlr_output_create_global(output, host.display);
    host.output = wlr_scene_output_create(host.scene, output);
    if (!host.output)
        return 1;
    host.frame.notify = frame;
    wl_signal_add(&output->events.frame, &host.frame);
    struct wl_event_source* interrupt = wl_event_loop_add_signal(loop, SIGINT, terminate, host.display);
    struct wl_event_source* term      = wl_event_loop_add_signal(loop, SIGTERM, terminate, host.display);
    host.heartbeat                    = wl_event_loop_add_timer(loop, heartbeat, &host);
    if (!interrupt || !term || !host.heartbeat || wl_display_add_socket(host.display, "background") < 0)
        return 1;
    wl_event_source_timer_update(host.heartbeat, 100);
    wl_display_run(host.display);
    wl_display_destroy_clients(host.display);
    wl_list_remove(&host.frame.link);
    wl_list_remove(&host.new_toplevel.link);
    wlr_scene_node_destroy(&host.scene->tree.node);
    wlr_backend_destroy(backend);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wl_event_source_remove(interrupt);
    wl_event_source_remove(term);
    wl_event_source_remove(host.heartbeat);
    wl_display_destroy(host.display);
    return 0;
}
