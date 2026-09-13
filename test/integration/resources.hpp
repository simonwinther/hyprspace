// Resource probes run only in the private compositor's fixture plugin.
#pragma once

#include "../../src/Capture.hpp"
#include "../../src/Texture.hpp"
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
    auto& captureEntries(hyprspace::CWindowCapture& capture);
    template <auto Member> struct CCaptureEntryAccess {
        friend auto& captureEntries(hyprspace::CWindowCapture& capture) {
            return capture.*Member;
        }
    };
    template struct CCaptureEntryAccess<&hyprspace::CWindowCapture::m_fbs>;

    void requireResource(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    SDispatchResult resourceProbe(std::string mode) {
        using namespace hyprspace;
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        nlohmann::json result;
        try {
            if (mode == "churn") {
                const auto    before = CTextureCache::resources();
                CTextureCache cache;
                const auto    color = CHyprColor(1., 1., 1., 1.);
                auto          hot   = cache.text("frequently reused", "Sans 12", color, 300);
                requireResource(hot != nullptr, "initial text upload failed");
                for (int i = 0; i < 700; ++i) {
                    requireResource(cache.text("Unique title " + std::to_string(i), "Sans 12", color, 300) != nullptr, "title upload failed");
                    requireResource(cache.text("frequently reused", "Sans 12", color, 300) == hot, "LRU evicted a frequently reused title");
                }
                for (int width = 1; width <= 700; ++width) {
                    requireResource(cache.text("Animated width variants stay bounded", "Sans 12", color, width) != nullptr, "width upload failed");
                    requireResource(cache.text("frequently reused", "Sans 12", color, 300) == hot, "width churn evicted the hot title");
                }
                requireResource(cache.size() == CTextureCache::SResources::MAX_ENTRIES, "entry limit was not exercised");
                result["entries_at_limit"] = cache.size();
                cache.clear();
                requireResource(hot->ok() && CTextureCache::resources().bytes > before.bytes, "eviction destroyed a frame's shared texture");
                hot.reset();
                requireResource(CTextureCache::resources().bytes == before.bytes, "clear retained unreferenced textures");

                std::vector<SP<Render::ITexture>> leases;
                for (int i = 0; i < 12; ++i) {
                    auto texture = cache.text(std::string(256, 'M') + std::to_string(i), "Sans 128", color, 4096);
                    if (texture)
                        leases.push_back(texture);
                }
                requireResource(CTextureCache::resources().failures > before.failures, "live texture byte limit was not exercised");
                requireResource(CTextureCache::resources().bytes <= CTextureCache::SResources::MAX_BYTES, "live textures exceed byte limit");
                result["retained_frame_textures"] = leases.size();
                leases.clear();
                cache.clear();
                requireResource(CTextureCache::resources().bytes == before.bytes, "frame completion retained textures");
                auto reopened = cache.text("frequently reused", "Sans 12", color, 300);
                requireResource(reopened != nullptr, "cache did not recover after clear");
                const auto& counts = CTextureCache::resources();
                result.update({{"peak_bytes", counts.peakBytes},
                               {"peak_entries", counts.peakEntries},
                               {"evictions", counts.evictions - before.evictions},
                               {"hits", counts.hits - before.hits},
                               {"misses", counts.misses - before.misses},
                               {"failures", counts.failures - before.failures}});
            } else if (mode == "allocation") {
                const auto before = captureResources();
                // Invalid GL dimensions force an incomplete FBO without consuming
                // GPU memory. The native allocator would assert here.
                GLint maximum = 0;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
                auto rejected = createCaptureFramebuffer();
                requireResource(!rejected->alloc(maximum + 1, 1, DRM_FORMAT_ABGR8888), "oversized framebuffer was accepted");
                requireResource(!rejected->isAllocated() && !rejected->getTexture(), "failed allocation exposed an incomplete framebuffer");
                requireResource(glGetError() == GL_NO_ERROR, "failed allocation left a GL error behind");

                class CFailedFramebuffer : public Render::GL::CGLFramebuffer {
                  public:
                    bool alloc(int, int, DRMFormat) override {
                        return false;
                    }
                };
                bool           fail = true;
                CWindowCapture capture([&]() -> SP<Render::IFramebuffer> { return fail ? makeShared<CFailedFramebuffer>() : createCaptureFramebuffer(); });
                const auto     window  = Desktop::focusState()->window();
                const auto     monitor = window ? window->m_monitor.lock() : nullptr;
                requireResource(window && monitor, "probe needs a focused fixture window");
                auto now = CWindowCapture::Clock::now();
                for (int i = 0; i < 120; ++i)
                    capture.capture(window, monitor, now);
                requireResource(!capture.textureFor(window), "failed first capture has a texture");
                requireResource(captureResources().attempts == before.attempts + 1, "failure retried on every frame");
                fail = false;
                now += std::chrono::seconds(2);
                capture.capture(window, monitor, now);
                auto previous = capture.textureFor(window);
                requireResource(previous != nullptr, "capture did not recover after backoff");
                {
                    const auto scale = monitor->m_scale;
                    struct SRestoreScale {
                        PHLMONITOR monitor;
                        double     scale;
                        ~SRestoreScale() {
                            monitor->m_scale = scale;
                        }
                    } restore{monitor, scale};
                    monitor->m_scale *= 0.75;
                    fail = true;
                    capture.capture(window, monitor, now);
                    requireResource(capture.textureFor(window) == previous, "failed resize discarded previous capture");
                }
                captureEntries(capture).at(window.get()).window = {};
                requireResource(!capture.textureFor(window), "a reused window address exposed another window's pixels");
                capture.capture(window, monitor, now);
                requireResource(!capture.textureFor(window), "a new window inherited a previous capture after allocation failure");
                capture.clear();
                requireResource(previous->ok(), "capture clear destroyed a retained frame texture");
                previous.reset();
                requireResource(captureResources().bytes == before.bytes, "capture teardown leaked bytes");
                const auto& counts = captureResources();
                result             = {
                    {"attempts", counts.attempts - before.attempts}, {"failures", counts.failures - before.failures}, {"fallbacks", counts.fallbacks - before.fallbacks},
                    {"previous", counts.previous - before.previous}, {"omitted", counts.omitted - before.omitted},    {"peak_bytes", counts.peakBytes},
                    {"invalid_gl_allocation_rejected", true}};
            } else {
                throw std::runtime_error("unknown resource probe");
            }
            // hyprctl prints the error field verbatim; the Python fixture reads
            // the JSON instead of treating this as a production dispatcher.
            return {.success = false, .error = result.dump()};
        } catch (const std::exception& error) {
            return {.success = false, .error = std::string("resource probe failed: ") + error.what()};
        }
    }
} // namespace
