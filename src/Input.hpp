#pragma once

#include "Geometry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace hyprspace {

    // Only user motion arms a dwell. A completed visit stays consumed until
    // the pointer leaves; an unfinished visit can recover on later motion.
    class CZoomEdgeHover {
      public:
        using Clock = std::chrono::steady_clock;

        bool motion(std::optional<EDirection> edge, bool eligible, Clock::time_point now, bool ready = true) {
            if (edge != m_edge)
                sync(edge);
            if (!eligible) {
                cancel();
                return false;
            }
            if (!edge || m_pending || m_consumed)
                return false;
            m_pending = edge;
            if (ready)
                m_started = now;
            return true;
        }

        std::optional<EDirection> advance(std::optional<EDirection> edge, bool ready, Clock::time_point now) {
            if (edge != m_pending) {
                cancel();
                return std::nullopt;
            }
            if (!m_pending)
                return std::nullopt;
            if (!ready) {
                m_started.reset();
                return std::nullopt;
            }
            if (!m_started)
                m_started = now;
            if (now - *m_started < std::chrono::milliseconds(250))
                return std::nullopt;
            const auto result = m_pending;
            m_consumed        = true;
            cancel();
            return result;
        }

        void sync(std::optional<EDirection> edge) {
            if (edge != m_edge)
                m_consumed = false;
            m_edge = edge;
            cancel();
        }

        void cancel() {
            m_pending.reset();
            m_started.reset();
        }

        std::optional<EDirection> pending() const {
            return m_pending;
        }

      private:
        std::optional<EDirection>        m_edge, m_pending;
        std::optional<Clock::time_point> m_started;
        bool                             m_consumed = false;
    };

    // Independent keyboard holds share one zoom. Cancellation drops their
    // tokens without reusing them, so a late release cannot end a newer hold.
    class CZoomHoldState {
      public:
        uint64_t press() {
            if (m_nextToken == std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("zoom hold tokens exhausted");
            const auto token = ++m_nextToken;
            m_tokens.insert(token);
            return token;
        }

        bool release(uint64_t token) {
            return m_tokens.erase(token) > 0;
        }

        void cancel() {
            m_tokens.clear();
        }

        bool held() const {
            return !m_tokens.empty();
        }

      private:
        uint64_t                     m_nextToken = 0;
        std::unordered_set<uint64_t> m_tokens;
    };

    struct SScrollInput {
        double   delta      = 0.0;
        int32_t  value120   = 0;
        uint32_t timeMs     = 0;
        bool     wheel      = true;
        bool     horizontal = false;
        bool     wheelTilt  = false;
    };

    // The lens acts on baseline-camera output coordinates. Its two affine
    // endpoints share one progress value, preserving anchors during animation.
    class CInspectionZoom {
      public:
        SOverviewCamera current(double progress) const {
            return interpolateOverviewCamera(m_from, m_goal, progress);
        }

        const SOverviewCamera& goal() const {
            return m_goal;
        }

        bool goalFits(const SBoxF& footprint) const {
            return fits(m_goal, footprint);
        }

        bool canPan(const SBoxF& footprint, double progress) const {
            const auto displayed = current(progress);
            return std::isfinite(progress) && displayed.scale > 1 && fits(displayed, footprint);
        }

        bool beginPan(const SBoxF& footprint, double progress) {
            if (!canPan(footprint, progress))
                return false;
            freeze(progress);
            return true;
        }

        bool pan(const SBoxF& footprint, double dx, double dy) {
            if (!std::isfinite(dx) || !std::isfinite(dy) || !canPan(footprint, 1))
                return false;
            auto next = m_goal;
            next.x    = std::clamp(next.x + dx, (footprint.x + footprint.w) * (1 - next.scale), footprint.x * (1 - next.scale));
            next.y    = std::clamp(next.y + dy, (footprint.y + footprint.h) * (1 - next.scale), footprint.y * (1 - next.scale));
            if (next == m_goal)
                return false;
            m_from = m_goal = next;
            m_direction     = 0;
            return true;
        }

      private:
        static bool fits(const SOverviewCamera& camera, const SBoxF& footprint) {
            if (!std::isfinite(camera.scale) || camera.scale < 1 || camera.scale > 4 || !std::isfinite(camera.x) || !std::isfinite(camera.y) ||
                !std::isfinite(footprint.x) || !std::isfinite(footprint.y) || !std::isfinite(footprint.w) || !std::isfinite(footprint.h) || footprint.w <= 0 ||
                footprint.h <= 0)
                return false;
            if (camera.scale == 1)
                return camera == SOverviewCamera{};
            const double minimumX = (footprint.x + footprint.w) * (1 - camera.scale), maximumX = footprint.x * (1 - camera.scale);
            const double minimumY = (footprint.y + footprint.h) * (1 - camera.scale), maximumY = footprint.y * (1 - camera.scale);
            if (!std::isfinite(minimumX) || !std::isfinite(maximumX) || !std::isfinite(minimumY) || !std::isfinite(maximumY))
                return false;
            // Freezing an interpolated lens can introduce a few rounding ULPs
            // at a bound. Preserve a valid inspection while rejecting a fit pan.
            const double tolerance =
                32 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, std::abs(minimumX), std::abs(maximumX), std::abs(minimumY), std::abs(maximumY), std::abs(camera.x), std::abs(camera.y)});
            return camera.x >= minimumX - tolerance && camera.x <= maximumX + tolerance && camera.y >= minimumY - tolerance && camera.y <= maximumY + tolerance;
        }

      public:
        bool beginFitTransition(const SOverviewCamera& displayed, const SOverviewCamera& fit) {
            const auto lens = relativeOverviewCamera(displayed, fit);
            if (!lens)
                return false;
            m_from      = *lens;
            m_goal      = {};
            m_direction = 0;
            return true;
        }

        bool scroll(const SScrollInput& event, double scrollFactor, const SBoxF& footprint, double pointerX, double pointerY, double progress,
                    std::optional<SBoxF> displayedFootprint = std::nullopt) {
            if (!event.wheel || event.horizontal || event.wheelTilt || !std::isfinite(event.delta) || !std::isfinite(scrollFactor) || scrollFactor == 0 ||
                !std::isfinite(pointerX) || !std::isfinite(pointerY) || !std::isfinite(progress) || !std::isfinite(footprint.x) || !std::isfinite(footprint.y) ||
                !std::isfinite(footprint.w) || !std::isfinite(footprint.h) || footprint.w <= 0 || footprint.h <= 0)
                return false;
            const double right = footprint.x + footprint.w, bottom = footprint.y + footprint.h;
            if (!std::isfinite(right) || !std::isfinite(bottom))
                return false;
            const double detents = event.value120 != 0 ? event.value120 / 120.0 : event.delta / 15.0;
            if (detents == 0)
                return false;

            // Compare magnitudes before multiplying: a finite device factor
            // can still overflow when applied to an unusually large event.
            constexpr double MAX_DETENTS = 32;
            const double     amount = std::abs(detents), factor = std::abs(scrollFactor);
            const double     effective = factor >= MAX_DETENTS / amount ? MAX_DETENTS : amount * factor;
            if (effective == 0)
                return false;
            const int  direction = std::signbit(detents) != std::signbit(scrollFactor) ? -1 : 1;
            const auto displayed = current(progress);
            // A fit handoff can start below 1 or above 4. Its first tick uses
            // the new hold's requested fit; reversals use bounded display scale.
            const double source = m_direction == 0 || direction == m_direction ? m_goal.scale : std::clamp(displayed.scale, 1.0, 4.0);
            double       scale  = std::clamp(source * std::exp(-direction * effective * std::log(1.15)), 1.0, 4.0);
            if (scale <= 1.0 + 1e-9)
                scale = 1;
            else if (scale >= 4.0 - 1e-9)
                scale = 4;
            if (scale == m_goal.scale)
                return false; // Limits neither retain input nor replace an anchor.

            SOverviewCamera next;
            if (scale != 1) {
                const auto box = displayedFootprint.value_or(displayed.apply(footprint));
                if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.w) || !std::isfinite(box.h) || box.w <= 0 || box.h <= 0)
                    return false;
                // During opening, the selected cell also morphs. Preserve its
                // normalized pointer position in the final fitted footprint.
                const double anchorX  = footprint.x + (pointerX - box.x) / box.w * footprint.w;
                const double anchorY  = footprint.y + (pointerY - box.y) / box.h * footprint.h;
                const double minimumX = right * (1 - scale), maximumX = footprint.x * (1 - scale);
                const double minimumY = bottom * (1 - scale), maximumY = footprint.y * (1 - scale);
                const double x = pointerX - anchorX * scale, y = pointerY - anchorY * scale;
                if (!std::isfinite(anchorX) || !std::isfinite(anchorY) || !std::isfinite(minimumX) || !std::isfinite(maximumX) || !std::isfinite(minimumY) ||
                    !std::isfinite(maximumY) || !std::isfinite(x) || !std::isfinite(y))
                    return false;
                next = {scale, std::clamp(x, minimumX, maximumX), std::clamp(y, minimumY, maximumY)};
            }
            m_from      = displayed;
            m_goal      = next;
            m_direction = direction;
            return true;
        }

        void reset() {
            m_from = m_goal = {};
            m_direction     = 0;
        }

        void freeze(double progress) {
            m_from = m_goal = current(progress);
            m_direction     = 0;
        }

      private:
        SOverviewCamera m_from, m_goal;
        int             m_direction = 0;
    };

    class CScrollAccumulator {
      public:
        int steps(const SScrollInput& event) {
            // Wheels report fractions of a detent in units of 120. Finger and
            // continuous scroll use logical pixels, with 40 px per selection.
            const double amount = event.wheel ? (event.value120 != 0 ? event.value120 / 120.0 : event.delta / 15.0) : event.delta / 40.0;
            if (!std::isfinite(amount)) {
                reset();
                return 0;
            }
            if (amount == 0.0) {
                if (!event.wheel)
                    reset(); // finger/continuous axis-stop ends a gesture
                return 0;
            }

            if (!m_hasTime || event.timeMs - m_lastTime > 300 || event.wheel != m_wheel || (m_remainder != 0.0 && std::signbit(amount) != std::signbit(m_remainder)))
                m_remainder = 0.0;

            m_hasTime  = true;
            m_lastTime = event.timeMs;
            m_wheel    = event.wheel;
            m_remainder += std::clamp(amount, -32.0, 32.0);

            const int result = static_cast<int>(m_remainder + std::copysign(1e-9, m_remainder));
            m_remainder -= result;
            if (std::abs(m_remainder) < 1e-9)
                m_remainder = 0.0;
            return result;
        }

        void reset() {
            m_remainder = 0.0;
            m_hasTime   = false;
        }

      private:
        double   m_remainder = 0.0;
        uint32_t m_lastTime  = 0;
        bool     m_hasTime   = false;
        bool     m_wheel     = true;
    };

    // Button releases follow their presses even if an overlay opened, closed,
    // or yielded to another UI between the two events.
    class CButtonCapture {
      public:
        bool captured(uint32_t button) const {
            return m_buttons.contains(button);
        }

        void orphan(uint32_t button) {
            if (captured(button))
                m_orphaned.insert(button);
        }

        void preparePress(uint32_t button) {
            if (m_orphaned.erase(button) > 0)
                m_buttons.erase(button);
        }

        bool consume(uint32_t button, bool pressed, bool ownsInput) {
            if (!pressed) {
                m_orphaned.erase(button);
                return m_buttons.erase(button) > 0;
            }

            preparePress(button);

            if (!ownsInput)
                return m_buttons.contains(button);

            m_buttons.insert(button);
            return true;
        }

        void clear() {
            m_buttons.clear();
            m_orphaned.clear();
        }

      private:
        std::unordered_set<uint32_t> m_buttons;
        std::unordered_set<uint32_t> m_orphaned;
    };

} // namespace hyprspace
