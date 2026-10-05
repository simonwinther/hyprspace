// Read back real overview capture pixels in the private compositor only.
#pragma once

#include "resources.hpp"
#include "../../src/Overview.hpp"
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <array>

namespace {
    auto& captureFor(hyprspace::COverview& view);
    template <auto Member> struct COverviewCaptureAccess {
        friend auto& captureFor(hyprspace::COverview& view) {
            return view.*Member;
        }
    };
    template struct COverviewCaptureAccess<&hyprspace::COverview::m_capture>;

    auto captureFramebufferID();
    template <auto Member> struct CCaptureFramebufferAccess {
        friend auto captureFramebufferID() {
            return Member;
        }
    };
    template struct CCaptureFramebufferAccess<&Render::GL::CGLFramebuffer::m_fb>;

    SDispatchResult captureProbe(std::string address) {
        using namespace hyprspace;
        try {
            size_t     suffix = 0;
            const auto value  = std::stoull(address, &suffix, 16);
            const auto mode   = address.substr(suffix);
            requireResource(mode.empty() || mode == " clipped", "capture probe expects an address and optional clipped mode");
            const auto& windows = Desktop::windowState()->windows();
            const auto  found   = std::ranges::find_if(windows, [value](const auto& window) { return reinterpret_cast<uintptr_t>(window.get()) == value; });
            requireResource(found != windows.end(), "capture probe window no longer exists");
            const auto window = *found;
            const auto view   = std::ranges::find_if(session().views, [&](const auto& candidate) { return candidate->monitor() == window->m_monitor; });
            requireResource(view != session().views.end(), "capture probe needs overview on the window output");
            auto& capture = captureFor(**view);
            if (mode == " clipped") {
                const auto surface  = window->wlSurface();
                const auto previous = surface->m_visibleRegion.copy();
                const auto restore  = Hyprutils::Utils::CScopeGuard([&] { surface->m_visibleRegion.set(previous); });
                const auto buffer   = surface->resource()->m_current.bufferSize;
                // Native visible regions use original surface buffer pixels.
                // Restrict only the parent; the distant child remains visible.
                surface->m_visibleRegion = CRegion{0, 0, std::floor(buffer.x / 2), std::floor(buffer.y / 2)};
                capture.capture(window, (*view)->monitor());
            }
            const auto entry   = captureEntries(capture).find(window.get());
            const auto texture = capture.textureFor(window);
            if (entry == captureEntries(capture).end() || !entry->second.fb || !texture)
                return {.success = false, .error = nlohmann::json{{"ready", false}}.dump()};

            Render::GL::g_pHyprOpenGL->makeEGLCurrent();
            GLint previousRead = 0, previousDraw = 0, previousPack = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPack);
            glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
            glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
            glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
            const auto restore = Hyprutils::Utils::CScopeGuard([=] {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDraw);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPack);
                glPixelStorei(GL_PACK_ALIGNMENT, alignment);
                glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
                glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
                glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
            });
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            const auto framebuffer = dynamicPointerCast<Render::GL::CGLFramebuffer>(entry->second.fb);
            requireResource(framebuffer != nullptr, "capture probe requires an OpenGL framebuffer");
            // CGLFramebuffer::bind sets only the draw binding and also changes
            // the renderer's viewport. Read this capture without either effect.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer.get()->*captureFramebufferID());
            const auto projection = Hyprutils::Math::Mat3x3::outputProjection(texture->m_size, HYPRUTILS_TRANSFORM_NORMAL).getMatrix();
            const auto sample     = [&](double x, double y) {
                // Convert top-left source positions through the same export
                // projection before sampling GL's bottom-left framebuffer space.
                const double sourceX = x * texture->m_size.x, sourceY = y * texture->m_size.y;
                const int    pixelX =
                    std::clamp(static_cast<int>(std::floor((projection[0] * sourceX + projection[1] * sourceY + projection[2] + 1) * texture->m_size.x / 2)), 0,
                               static_cast<int>(texture->m_size.x) - 1);
                const int pixelY =
                    std::clamp(static_cast<int>(std::floor((projection[3] * sourceX + projection[4] * sourceY + projection[5] + 1) * texture->m_size.y / 2)), 0,
                               static_cast<int>(texture->m_size.y) - 1);
                std::array<uint8_t, 4> color{};
                glReadPixels(pixelX, pixelY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
                return color;
            };
            const auto          sourceSize = capture.sizeFor(window);
            const auto          surface    = window->wlSurface()->resource();
            const auto          reported   = window->getReportedSize();
            const auto          visible    = window->wlSurface()->m_visibleRegion.getExtents();
            CSurfacePassElement native(CSurfacePassElement::SRenderData{.pMonitor = (*view)->monitor(),
                                                                        .pos      = (*view)->monitor()->m_position + window->m_floatingOffset,
                                                                        .surface  = surface,
                                                                        .texture  = surface->m_current.texture,
                                                                        .w        = sourceSize.x,
                                                                        .h        = sourceSize.y,
                                                                        .pWindow  = window});
            const auto          nativeBox = native.getTexBox();
            nlohmann::json      result    = {{"ready", true},
                                             {"clipped", mode == " clipped"},
                                             {"logical", {sourceSize.x, sourceSize.y}},
                                             {"texture", {texture->m_size.x, texture->m_size.y}},
                                             {"monitor", (*view)->monitor()->m_name},
                                             {"scale", (*view)->monitor()->m_scale},
                                             {"capture_bytes", static_cast<uint64_t>(texture->m_size.x) * static_cast<uint64_t>(texture->m_size.y) * 4},
                                             {"total_bytes", captureResources().bytes},
                                             {"downsampled", captureResources().downsampled},
                                             {"samples",
                                              {{"top_left", sample(0.04, 0.04)},
                                               {"top_right", sample(0.96, 0.04)},
                                               {"bottom_left", sample(0.04, 0.96)},
                                               {"bottom_right", sample(0.96, 0.96)},
                                               {"child", sample((4352.0 + 160) / 5120, (2304.0 + 90) / 2880)}}}};
            result["surface"]             = {{"size", {surface->m_current.size.x, surface->m_current.size.y}},
                                             {"buffer", {surface->m_current.bufferSize.x, surface->m_current.bufferSize.y}},
                                             {"reported", {reported.x, reported.y}},
                                             {"small", window->wlSurface()->small()},
                                             {"visible", {visible.x, visible.y, visible.w, visible.h}},
                                             {"texture_box", {nativeBox.x, nativeBox.y, nativeBox.w, nativeBox.h}}};
            result["projection"]          = projection;
            result["grid"]                = nlohmann::json::array();
            for (double y : {0.04, 0.2, 0.5, 0.8, 0.96}) {
                nlohmann::json row = nlohmann::json::array();
                for (double x : {0.04, 0.2, 0.5, 0.8, 0.96})
                    row.push_back(sample(x, y));
                result["grid"].push_back(std::move(row));
            }
            result["gl_error"] = glGetError();
            return {.success = false, .error = result.dump()};
        } catch (const std::exception& error) {
            return {.success = false, .error = std::string("capture probe failed: ") + error.what()};
        }
    }
} // namespace
