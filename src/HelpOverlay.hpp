#pragma once

#include "HelpModel.hpp"
#include "OverviewSession.hpp"
#include "SearchInput.hpp"
#include "Raster.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <xkbcommon/xkbcommon.h>
#include <limits>

namespace hyprspace {
    class CHelpOverlay {
      public:
        explicit CHelpOverlay(COverviewSession& owner) : m_session(owner) {}
        ~CHelpOverlay();
        void open(PHLMONITOR monitor);
        void close();
        void refresh(bool force = false);
        void syncKeyboardOwnership(bool owned);
        bool active() const {
            return m_active;
        }
        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }
        uint64_t generation() const {
            return m_generation;
        }
        bool searchFocused() const {
            return m_searchFocused;
        }
        const CSearchInput& search() const {
            return m_search;
        }
        const CHelpBrowser& browser() const {
            return m_browser;
        }
        const SHelpLayout& layout() const {
            return m_layout;
        }
        double scrollOffset() const {
            return m_scroll;
        }
        bool                          overSearch(const Vector2D& point) const;
        bool                          reservesKey(xkb_keysym_t sym, uint32_t mods) const;
        bool                          oneShot(xkb_keysym_t sym, const std::string& text) const;
        void                          onKey(xkb_keysym_t sym, uint32_t mods, const std::string& text);
        void                          pointer(const Vector2D& point);
        void                          button(uint32_t button, bool pressed, uint32_t mods, const Vector2D& point);
        void                          scroll(const SScrollInput& input);
        std::vector<UP<IPassElement>> buildPass(PHLMONITOR monitor) const;

      private:
        struct SSettings {
            xkb_keysym_t zoom, empty, windows, follow, help;
            uint32_t     workspaceMods;
            bool         wheelZoom, followEnabled, followMouse, warpCursor, showFollowHint, allMonitors;
            bool         operator==(const SSettings&) const = default;
        };
        SSettings               settings() const;
        std::vector<SHelpEntry> catalog(const SSettings& settings) const;
        void                    focusSearch(bool selectAll = true);
        void                    blurSearch();
        void                    setTopic(size_t topic);
        void                    ensureVisible();
        void                    queryChanged();
        void                    resetCaret();
        void                    stopCaret();
        SBoxF                   topicBox(size_t topic) const;
        SBoxF                   rowBox(size_t row) const;
        SBoxF                   searchContent() const;
        const STextCursor&      searchMetrics() const;

        COverviewSession&        m_session;
        PHLMONITORREF            m_monitor;
        bool                     m_active = false, m_searchFocused = false, m_caretVisible = false;
        bool                     m_keyboardOwned = false;
        uint64_t                 m_generation    = 0;
        CSearchInput             m_search;
        CHelpBrowser             m_browser;
        SHelpLayout              m_layout;
        std::optional<SSettings> m_settings;
        std::string              m_font;
        double                   m_scale = 0, m_scroll = 0, m_lineHeight = 22;
        Vector2D                 m_pointer;
        SP<CEventLoopTimer>      m_caretTimer;
        mutable std::string      m_measuredText;
        mutable size_t           m_measuredCursor = std::numeric_limits<size_t>::max();
        mutable STextCursor      m_searchMetrics;
        mutable double           m_searchOffset = 0;
    };
} // namespace hyprspace
