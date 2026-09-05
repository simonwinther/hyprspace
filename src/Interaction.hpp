#pragma once

#include "Geometry.hpp"

#include <chrono>
#include <optional>
#include <unordered_map>

namespace hyprspace {

    struct SWorkspaceIdentity {
        int64_t     id = 0;
        std::string name;
        bool        operator==(const SWorkspaceIdentity&) const = default;
        explicit    operator bool() const {
            return id != 0;
        }
    };

    struct SPoint {
        double x = 0, y = 0;
    };

    // All input geometry is logical, including transformed portrait outputs.
    // Monitor scale is applied only when rendering. A separated fullscreen
    // preview supplies its own desktop box instead of the workspace work area.
    inline std::optional<SPoint> mapPreviewPoint(SPoint point, const SBoxF& preview, const SBoxF& desktop) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(preview.w) || !std::isfinite(preview.h) || !std::isfinite(preview.x) ||
            !std::isfinite(preview.y) || !std::isfinite(desktop.x) || !std::isfinite(desktop.y) || !std::isfinite(desktop.w) || !std::isfinite(desktop.h) ||
            preview.w <= 0 || preview.h <= 0 || desktop.w <= 0 || desktop.h <= 0)
            return std::nullopt;
        return SPoint{desktop.x + (point.x - preview.x) * desktop.w / preview.w, desktop.y + (point.y - preview.y) * desktop.h / preview.h};
    }

    // A missing hit retains the command destination but is never a valid drop.
    template <typename Target> class CTargetSelection {
      public:
        void pointer(std::optional<Target> hit) {
            m_hit = hit;
            if (hit)
                m_selected = hit;
        }
        void keyboard(Target target) {
            m_selected = std::move(target);
        }
        const std::optional<Target>& command() const {
            return m_selected;
        }
        const std::optional<Target>& drop() const {
            return m_hit;
        }
        void clear() {
            m_hit.reset();
            m_selected.reset();
        }

      private:
        std::optional<Target> m_hit, m_selected;
    };

    template <typename WeakWindow> class CVisibilityLedger {
      public:
        template <typename Read, typename Hide> void hide(WeakWindow window, Read read, Hide hideWindow) {
            auto w = window.lock();
            if (!w)
                return;
            if (std::ranges::none_of(m_saved, [&](const auto& saved) { return saved.first.lock() == w; }))
                m_saved.emplace_back(window, read(w));
            hideWindow(w);
        }
        template <typename Restore> void restore(Restore restoreWindow) {
            for (const auto& [window, alpha] : m_saved)
                if (auto w = window.lock())
                    restoreWindow(w, alpha);
            m_saved.clear();
        }
        size_t size() const {
            return m_saved.size();
        }

      private:
        std::vector<std::pair<WeakWindow, float>> m_saved;
    };

    // Per-request, bounded and expiring. Capture and consumption are separate:
    // moving the pointer while a launcher processes a result cannot retarget it.
    template <typename Context> class CLaunchContexts {
      public:
        using Clock                   = std::chrono::steady_clock;
        static constexpr size_t LIMIT = 256;
        void                    prune(Clock::time_point now) {
            std::erase_if(m_contexts, [&](const auto& item) { return item.second.expires <= now; });
        }
        bool capture(std::string token, Context context, Clock::time_point now) {
            prune(now);
            if (token.empty() || m_contexts.size() >= LIMIT)
                return false;
            return m_contexts.emplace(std::move(token), SEntry{std::move(context), now + std::chrono::minutes(2), false}).second;
        }
        std::optional<Context> consume(const std::string& token, Clock::time_point now) {
            prune(now);
            const auto it = m_contexts.find(token);
            if (it == m_contexts.end() || it->second.consumed)
                return std::nullopt;
            it->second.consumed = true;
            return it->second.context;
        }
        void clear() {
            m_contexts.clear();
        }

      private:
        struct SEntry {
            Context           context;
            Clock::time_point expires;
            bool              consumed;
        };
        std::unordered_map<std::string, SEntry> m_contexts;
    };
} // namespace hyprspace
