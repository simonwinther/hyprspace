#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace hyprspace {

    struct SScrollInput {
        double   delta    = 0.0;
        int32_t  value120 = 0;
        uint32_t timeMs   = 0;
        bool     wheel    = true;
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
