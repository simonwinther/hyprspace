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
            m_consumed = true;
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
        bool                            m_consumed = false;
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
        bool consume(uint32_t button, bool pressed, bool ownsInput) {
            if (!pressed)
                return m_buttons.erase(button) > 0;

            if (!ownsInput)
                return m_buttons.contains(button);

            m_buttons.insert(button);
            return true;
        }

        void clear() {
            m_buttons.clear();
        }

      private:
        std::unordered_set<uint32_t> m_buttons;
    };

} // namespace hyprspace
