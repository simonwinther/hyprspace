#include "Capture.hpp"

#include "Access.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>

#include <algorithm>
#include <drm_fourcc.h>

namespace hyprspace {

    namespace {
        SCaptureResources resources;

        // Hyprland 0.56.2 asserts inside CGLFramebuffer::internalAlloc on an
        // incomplete framebuffer. Keep its type/bind/release implementation,
        // but give our single-colour captures a non-fatal allocation path.
        // Explicit instantiation uses the same pinned-ABI access idiom as Access.hpp.
        auto framebufferID();
        auto temporaryBuffer();
        template <auto Member> struct CFramebufferID {
            friend auto framebufferID() {
                return Member;
            }
        };
        template <auto Member> struct CTemporaryBuffer {
            friend auto temporaryBuffer() {
                return Member;
            }
        };
        template struct CFramebufferID<&Render::GL::CGLFramebuffer::m_fb>;
        template struct CTemporaryBuffer<&Render::GL::CGLFramebuffer::m_tempBuf>;

        struct SResidentCapture {
            WP<Render::ITexture> texture;
            size_t               bytes;
        };
        std::vector<SResidentCapture> residents;
        void                          pruneCaptures() {
            std::erase_if(residents, [](const auto& entry) { return entry.texture.expired(); });
            resources.bytes = 0;
            for (const auto& entry : residents)
                resources.bytes += entry.bytes;
        }

        class CCaptureFramebuffer final : public Render::GL::CGLFramebuffer {
          protected:
            bool internalAlloc(int w, int h, DRMFormat format) override {
                if (m_fbAllocated) {
                    release();
                    m_size = {w, h};
                }
                if (format != DRM_FORMAT_ABGR8888 || w <= 0 || h <= 0 || static_cast<size_t>(w) > SCaptureResources::MAX_CAPTURE_BYTES / 4 / static_cast<size_t>(h))
                    return false;
                const size_t bytes = static_cast<size_t>(w) * h * 4;
                pruneCaptures();
                if (residents.size() >= SCaptureResources::MAX_CAPTURES || resources.bytes > SCaptureResources::MAX_BYTES - bytes)
                    return false;

                Render::GL::g_pHyprOpenGL->makeEGLCurrent();
                GLint draw = 0, read = 0, texture = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                m_tex = g_pHyprRenderer->createTexture();
                if (!m_tex)
                    return false;
                m_tex->allocate({w, h}, format);
                m_tex->bind();
                m_tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                m_tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                m_tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                m_tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                auto& id = this->*framebufferID();
                id       = 0;
                glGenFramebuffers(1, &id);
                m_fbAllocated            = id != 0;
                this->*temporaryBuffer() = false;
                if (m_fbAllocated) {
                    glBindFramebuffer(GL_FRAMEBUFFER, id);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex->m_texID, 0);
                }
                const bool complete = m_fbAllocated && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                const auto error    = glGetError();
                if (!complete || error != GL_NO_ERROR)
                    release();
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
                glBindTexture(GL_TEXTURE_2D, texture);
                if (!complete || error != GL_NO_ERROR)
                    return false;
                residents.push_back({m_tex, bytes});
                pruneCaptures();
                resources.peakBytes = std::max(resources.peakBytes, resources.bytes);
                return true;
            }
        };
    } // namespace

    const SCaptureResources& captureResources() {
        pruneCaptures();
        return resources;
    }

    SP<Render::IFramebuffer> createCaptureFramebuffer() {
        return makeShared<CCaptureFramebuffer>();
    }

    void CWindowCapture::beginFrame() {
        for (auto& [_, e] : m_fbs)
            e.seen = false;
    }

    void CWindowCapture::endFrame() {
        std::erase_if(m_fbs, [](const auto& kv) { return !kv.second.seen; });
    }

    void CWindowCapture::capture(PHLWINDOW window, PHLMONITOR monitor, Clock::time_point now) {
        if (!window || !monitor || !window->m_isMapped)
            return;

        const auto LOGICAL = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (LOGICAL.x < 1.0 || LOGICAL.y < 1.0)
            return;

        // Capture at physical resolution so tiles stay sharp on HiDPI outputs.
        const auto PIXELS = (LOGICAL * monitor->m_scale).floor();
        auto&      entry  = m_fbs[window.get()];
        // An allocator can reuse a destroyed window's address before the next
        // pruning pass. Its pixels are never a fallback for a different window.
        if (entry.window.lock() != window) {
            entry        = {};
            entry.window = window;
        }
        entry.seen          = true;
        const auto fallback = [&] {
            ++resources.fallbacks;
            if (entry.valid)
                ++resources.previous;
            else
                ++resources.omitted;
        };
        // Check floating-point dimensions before converting to int. Captures
        // above the per-window budget use the previous frame or tile backing.
        const bool bounded =
            std::isfinite(PIXELS.x) && std::isfinite(PIXELS.y) && PIXELS.x >= 1 && PIXELS.y >= 1 && PIXELS.x * PIXELS.y <= SCaptureResources::MAX_CAPTURE_BYTES / 4;
        if (now < entry.retryAfter || !bounded) {
            fallback();
            return;
        }
        const int w = static_cast<int>(PIXELS.x), h = static_cast<int>(PIXELS.y);
        auto      fb = entry.fb;
        if (!fb || fb->m_size != Vector2D{w, h}) {
            ++resources.attempts;
            try {
                fb = m_allocator();
                if (!fb || !fb->alloc(w, h, DRM_FORMAT_ABGR8888) || !fb->isAllocated())
                    fb.reset();
            } catch (const std::bad_alloc&) {
                fb.reset();
            }
            if (!fb) {
                ++resources.failures;
                entry.retryAfter = now + std::chrono::seconds(1);
                fallback();
                return;
            }
        }
        fb->setImageDescription(monitor->workBufferImageDescription());

        CRegion fakeDamage{0, 0, w, h};

        if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, fb)) {
            entry.retryAfter = now + std::chrono::seconds(1);
            fallback();
            return;
        }

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
        entry.fb                                 = std::move(fb);
        entry.size                               = LOGICAL;
        entry.valid                              = true;

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
        if (it == m_fbs.end() || it->second.window.lock() != window || !it->second.valid || !it->second.fb || !it->second.fb->isAllocated())
            return nullptr;

        return it->second.fb->getTexture();
    }

    Vector2D CWindowCapture::sizeFor(PHLWINDOW window) const {
        if (!window)
            return {};

        auto it = m_fbs.find(window.get());
        if (it == m_fbs.end() || it->second.window.lock() != window)
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
