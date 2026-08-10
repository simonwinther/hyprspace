// hyprspace - GNOME-style Alt+Tab window switcher.

#pragma once

#include "Geometry.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <xkbcommon/xkbcommon.h>

#include <string>
#include <vector>

namespace hyprspace {

    class CSwitcher {
      public:
        // `forward` picks the initial step direction (Alt+Tab vs Alt+Shift+Tab).
        CSwitcher(PHLMONITOR monitor, bool forward);
        ~CSwitcher();

        // Advance the selection; called for each further Alt+Tab press.
        void advance(bool forward);

        void close(bool commitSelection);
        bool finished() const {
            return m_closing && m_alpha->value() < 0.01F;
        }
        bool closing() const {
            return m_closing;
        }
        bool empty() const {
            return m_entries.empty();
        }

        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }

        // --- input ---
        bool onKey(xkb_keysym_t sym, uint32_t mods, bool pressed);
        void onModifiersChanged(uint32_t mods);
        void onMouseMove(const Vector2D& globalPos);
        bool onMouseButton(uint32_t button, bool pressed);
        void onScroll(double delta);

        // --- render ---
        std::vector<UP<IPassElement>> buildPass();
        void                          damage();

      private:
        struct SEntry {
            PHLWINDOWREF window;
            std::string  title;
            std::string  appClass;
            SBoxF        box; // icon box, monitor-local logical px
        };

        void collectWindows(bool forward);
        void layoutPanel();
        void commit();
        void closeSelection();

        PHLMONITORREF       m_monitor;
        std::vector<SEntry> m_entries;

        SBoxF m_panel     = {};
        SBoxF m_titleArea = {};
        int   m_selected  = 0;
        int   m_hovered   = -1;
        bool  m_closing   = false;

        PHLANIMVAR<float> m_alpha;

        friend class CSwitcherPassElement;
    };

} // namespace hyprspace
