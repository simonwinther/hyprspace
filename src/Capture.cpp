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

        const auto LOGICAL = window->m_realSize->value();
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
