// hyprspace - offscreen capture of live window contents.

#pragma once

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <unordered_map>

namespace hyprspace {

    // Renders individual windows into per-window framebuffers using Hyprland's own
    // window renderer, so tiles show real, live content — including windows on
    // workspaces that are not currently visible.
    class CWindowCapture {
      public:
        // Refresh the framebuffer for `window`. Must be called with no monitor
        // render pass in flight (i.e. from the render.pre event).
        void                 capture(PHLWINDOW window, PHLMONITOR monitor);

        SP<Render::ITexture> textureFor(PHLWINDOW window) const;
        Vector2D             sizeFor(PHLWINDOW window) const;

        // Drop framebuffers for windows that were not captured since the last
        // call to beginFrame().
        void                 beginFrame();
        void                 endFrame();

        void                 clear();
        size_t               size() const {
            return m_fbs.size();
        }

      private:
        struct SEntry {
            SP<Render::IFramebuffer> fb;
            Vector2D                 size;
            bool                     seen = false;
        };

        std::unordered_map<Desktop::View::CWindow*, SEntry> m_fbs;
    };

} // namespace hyprspace
