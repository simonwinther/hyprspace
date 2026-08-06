#include "PassElements.hpp"

#include "Overview.hpp"
#include "Switcher.hpp"

#include <hyprland/src/helpers/Monitor.hpp>

namespace hyprspace {

    // ------------------------------------------------------------- overview --

    std::vector<UP<IPassElement>> COverviewPassElement::draw() {
        if (!m_overview)
            return {};
        return m_overview->buildPass();
    }

    bool COverviewPassElement::needsLiveBlur() {
        return false;
    }

    bool COverviewPassElement::needsPrecomputeBlur() {
        return false;
    }

    std::optional<CBox> COverviewPassElement::boundingBox() {
        const auto MONITOR = m_overview ? m_overview->monitor() : nullptr;
        if (!MONITOR)
            return std::nullopt;

        return CBox{0, 0, MONITOR->m_size.x, MONITOR->m_size.y};
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
