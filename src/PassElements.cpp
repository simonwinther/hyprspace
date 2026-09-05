#include "PassElements.hpp"

#include "Switcher.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <hyprutils/utils/ScopeGuard.hpp>

namespace hyprspace {

    CWindowPreviewPassElement::CWindowPreviewPassElement(SRenderData data, PHLWINDOW window) : CTexPassElement(std::move(data)), m_window(window) {}

    bool CWindowPreviewPassElement::needsPrecomputeBlur() {
        const auto WINDOW = m_window.lock();
        return m_data.blur && WINDOW && g_pHyprRenderer->shouldUseNewBlurOptimizations(nullptr, WINDOW);
    }

    bool CWindowPreviewPassElement::needsLiveBlur() {
        return m_data.blur && !needsPrecomputeBlur();
    }

    std::vector<UP<IPassElement>> CWindowPreviewPassElement::draw() {
        std::vector<UP<IPassElement>> out;
        const auto                    WINDOW = m_window.lock();
        if (!WINDOW || !WINDOW->m_isMapped || !m_data.tex || !m_data.tex->ok())
            return out;

        auto plainTexture = [&] {
            auto data = m_data;
            data.blur = false;
            out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
        };

        // These weak handles refer to compositor-owned unique objects; they
        // can be borrowed during the render pass, but not promoted with lock().
        const auto GL = g_pHyprRenderer->glBackend();
        if (!m_data.blur || !GL) {
            plainTexture();
            return out;
        }

        auto&   renderData = g_pHyprRenderer->m_renderData;
        CRegion damage     = renderData.damage.copy().intersect(m_data.box);
        if (!m_data.clipBox.empty())
            damage.intersect(m_data.clipBox);
        if (damage.empty())
            return out;

        const bool           PRECOMPUTED = needsPrecomputeBlur();
        SP<Render::ITexture> BACKGROUND;
        if (PRECOMPUTED) {
            if (const auto RESOURCES = renderData.pMonitor->resources(); RESOURCES && RESOURCES->m_blurFB)
                BACKGROUND = RESOURCES->m_blurFB->getTexture();
        } else
            BACKGROUND = g_pHyprRenderer->blurMainFramebuffer(m_data.a, &damage);
        if (!BACKGROUND || !BACKGROUND->ok()) {
            plainTexture();
            return out;
        }

        // Use the existing texture/blur compositor with the chosen background.
        // Passing this through the ordinary texture element would lose the
        // window's cached-blur policy; setting currentWindow instead would apply
        // dimming/tint a second time to pixels that already contain those effects.
        const auto oldClip    = renderData.clipBox;
        const auto oldWindow  = renderData.currentWindow;
        const auto oldSurface = renderData.surface;
        const auto oldUVStart = renderData.primarySurfaceUVTopLeft;
        const auto oldUVEnd   = renderData.primarySurfaceUVBottomRight;
        renderData.clipBox    = m_data.clipBox;
        renderData.currentWindow.reset();
        renderData.surface.reset();
        renderData.primarySurfaceUVTopLeft     = {-1, -1};
        renderData.primarySurfaceUVBottomRight = {-1, -1};
        g_pHyprRenderer->pushMonitorTransformEnabled(false);
        const Hyprutils::Utils::CScopeGuard restore{[&] {
            g_pHyprRenderer->popMonitorTransformEnabled();
            renderData.clipBox                     = oldClip;
            renderData.currentWindow               = oldWindow;
            renderData.surface                     = oldSurface;
            renderData.primarySurfaceUVTopLeft     = oldUVStart;
            renderData.primarySurfaceUVBottomRight = oldUVEnd;
        }};

        GL->renderTexture(m_data.tex, m_data.box,
                          Render::GL::CHyprOpenGLImpl::STextureRenderData{
                              .blur                  = true,
                              .blurA                 = m_data.blurA,
                              .blockBlurOptimization = !PRECOMPUTED,
                              .blurredBG             = BACKGROUND,
                              .damage                = &damage,
                              .a                     = m_data.a,
                              .round                 = m_data.round,
                              .roundingPower         = m_data.roundingPower,
                              .allowDim              = false,
                          });
        return out;
    }

    // ------------------------------------------------------------- switcher --

    std::vector<UP<IPassElement>> CSwitcherPassElement::draw() {
        if (!m_switcher)
            return {};
        return m_switcher->buildPass();
    }

    bool CSwitcherPassElement::needsLiveBlur() {
        return false;
    }

    bool CSwitcherPassElement::needsPrecomputeBlur() {
        return false;
    }

    std::optional<CBox> CSwitcherPassElement::boundingBox() {
        const auto MONITOR = m_switcher ? m_switcher->monitor() : nullptr;
        if (!MONITOR)
            return std::nullopt;

        return CBox{0, 0, MONITOR->m_size.x, MONITOR->m_size.y};
    }

} // namespace hyprspace
