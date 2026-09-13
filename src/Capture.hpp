// hyprspace - offscreen capture of live window contents.

#pragma once

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <unordered_map>
#include <chrono>
#include <functional>

namespace hyprspace {

    struct SCaptureResources {
        static constexpr size_t MAX_BYTES         = 256 * 1024 * 1024;
        static constexpr size_t MAX_CAPTURES      = 256;
        static constexpr size_t MAX_CAPTURE_BYTES = 32 * 1024 * 1024;
        size_t                  bytes = 0, peakBytes = 0, attempts = 0, failures = 0, fallbacks = 0, previous = 0, omitted = 0;
    };

    const SCaptureResources& captureResources();
    SP<Render::IFramebuffer> createCaptureFramebuffer();

    // Renders individual windows into per-window framebuffers using Hyprland's own
    // window renderer, so tiles show real, live content — including windows on
    // workspaces that are not currently visible.
    class CWindowCapture {
      public:
        using Clock = std::chrono::steady_clock;
        explicit CWindowCapture(std::function<SP<Render::IFramebuffer>()> allocator = createCaptureFramebuffer) : m_allocator(std::move(allocator)) {}
        // Refresh the framebuffer for `window`. Must be called with no monitor
        // render pass in flight (i.e. from the render.pre event).
        void capture(PHLWINDOW window, PHLMONITOR monitor, Clock::time_point now = Clock::now());

        SP<Render::ITexture> textureFor(PHLWINDOW window) const;
        Vector2D             sizeFor(PHLWINDOW window) const;

        // Drop framebuffers for windows that were not captured since the last
        // call to beginFrame().
        void beginFrame();
        void endFrame();

        void   clear();
        size_t size() const {
            return m_fbs.size();
        }

      private:
        struct SEntry {
            PHLWINDOWREF             window;
            SP<Render::IFramebuffer> fb;
            Vector2D                 size;
            bool                     seen  = false;
            bool                     valid = false;
            Clock::time_point        retryAfter{};
        };

        std::unordered_map<Desktop::View::CWindow*, SEntry> m_fbs;
        std::function<SP<Render::IFramebuffer>()>           m_allocator;
    };

} // namespace hyprspace
