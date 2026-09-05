#include "Capture.hpp"

#include "Access.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>

#include <algorithm>
#include <drm_fourcc.h>

namespace hyprspace {

    void CWindowCapture::beginFrame() {
        for (auto& [_, e] : m_fbs)
            e.seen = false;
    }

    void CWindowCapture::endFrame() {
        std::erase_if(m_fbs, [](const auto& kv) { return !kv.second.seen; });
    }

    void CWindowCapture::capture(PHLWINDOW window, PHLMONITOR monitor) {
        if (!window || !monitor || !window->m_isMapped)
            return;

        const auto LOGICAL = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (LOGICAL.x < 1.0 || LOGICAL.y < 1.0)
            return;

        // Capture at physical resolution so tiles stay sharp on HiDPI outputs.
        const auto PIXELS = (LOGICAL * monitor->m_scale).floor();
        const int  w      = std::max(1, static_cast<int>(PIXELS.x));
        const int  h      = std::max(1, static_cast<int>(PIXELS.y));

        auto& entry = m_fbs[window.get()];
        entry.seen  = true;
        entry.size  = LOGICAL;

        if (!entry.fb)
            entry.fb = g_pHyprRenderer->createFB("hyprspace window");

        if (!entry.fb)
            return;

        if (entry.fb->m_size.x != w || entry.fb->m_size.y != h) {
            entry.fb->alloc(w, h, DRM_FORMAT_ABGR8888);
            entry.fb->setImageDescription(monitor->workBufferImageDescription());
        }

        CRegion fakeDamage{0, 0, w, h};

        if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, entry.fb))
            return;

        // Render into this framebuffer on its own terms, not the monitor's.
        //
        // beginFullFakeRender sets both the projection and the viewport up for
        // the output: the projection folds in the panel's transform, and the
        // viewport is its pixel size. Neither fits a capture. This framebuffer
        // is one window big and holds that window upright, so on a rotated
        // output the transform lays it on its side, and a landscape viewport
        // over a portrait framebuffer clips whatever survived.
        //
        // RPT_EXPORT is the projection Hyprland uses for its own renders into a
        // framebuffer that is not a monitor: identity, with boxes measured in
        // that framebuffer's own pixels. Paired with a matching viewport the
        // window's box lands exactly on the framebuffer at any scale and any
        // transform. On an unrotated output this works out to precisely what
        // the monitor projection was already doing, which is why those tiles
        // looked right before and still do.
        g_pHyprRenderer->m_renderData.fbSize          = Vector2D{static_cast<double>(w), static_cast<double>(h)};
        g_pHyprRenderer->m_renderData.transformDamage = false;
        g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
        g_pHyprRenderer->setViewport(0, 0, w, h);

        // This capture drives frame callbacks for every window it draws.
        //
        // Deferring to the normal pass for windows on the visible workspace does
        // not work while the overview is up: the overview parks those windows at
        // zero alpha precisely so the normal pass skips them, so they would get
        // no callbacks from anywhere and stop repainting. A client that is not
        // repainting never commits a buffer at its new size, which is why a
        // window resized inside the overview kept showing a stale buffer with
        // empty space where it grew.
        const bool prevBlock                     = g_pHyprRenderer->m_bBlockSurfaceFeedback;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor(0, 0, 0, 0)});
        g_pHyprRenderer->startRenderPass();

        hidden::renderWindowStandalone(window, monitor, Time::steadyNow());

        // Never run the user's screen shader over an offscreen tile capture.
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        g_pHyprRenderer->m_bBlockSurfaceFeedback = prevBlock;

        // Nothing above is restored, and nothing may be: the render is over.
        //
        // setProjectionType() recomputes a matrix from render data that endRender
        // has already finished with, and calling it here aborts the compositor
        // outright. It is also pointless. Every render re-establishes all of this
        // at its start — beginRender sets the projection, and both
        // beginRenderInternal and beginFullFakeRenderInternal set the viewport
        // and transformDamage — so the next capture and the monitor's own frame
        // each begin from a clean slate regardless of what is left here.
    }

    SP<Render::ITexture> CWindowCapture::textureFor(PHLWINDOW window) const {
        if (!window)
            return nullptr;

        auto it = m_fbs.find(window.get());
        if (it == m_fbs.end() || !it->second.fb)
            return nullptr;

        return it->second.fb->getTexture();
    }

    Vector2D CWindowCapture::sizeFor(PHLWINDOW window) const {
        if (!window)
            return {};

        auto it = m_fbs.find(window.get());
        if (it == m_fbs.end())
            return {};

        return it->second.size;
    }

    void CWindowCapture::clear() {
        if (!m_fbs.empty())
            Render::GL::g_pHyprOpenGL->makeEGLCurrent();

        for (auto& [_, e] : m_fbs) {
            if (e.fb)
                e.fb->release();
        }

        m_fbs.clear();
    }

} // namespace hyprspace
