// hyprspace - the full-screen workspace overview.

#pragma once

#include "Capture.hpp"
#include "Geometry.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <xkbcommon/xkbcommon.h>

#include <string>
#include <vector>

namespace hyprspace {

    class COverview {
      public:
        COverview(PHLMONITOR monitor);
        ~COverview();

        // Begin the closing animation; the instance is reaped once it finishes.
        void close(bool commitSelection);
        bool finished() const {
            return m_closing && m_progress->value() < 0.01F;
        }
        bool closing() const {
            return m_closing;
        }

        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }

        // --- input ---
        // Returns true when the key was consumed.
        bool onKey(xkb_keysym_t sym, uint32_t mods, bool pressed);
        void onMouseMove(const Vector2D& globalPos);
        bool onMouseButton(uint32_t button, bool pressed);

        // --- render lifecycle ---
        void                          prepareFrame(); // capture live window contents
        std::vector<UP<IPassElement>> buildPass();    // called from the pass element
        void                          damage();



      private:
        struct SEntry {
            PHLWINDOWREF window;
            SBoxF        target;    // final tile box, monitor-local logical px
            SBoxF        start;     // where it animates from
            long         workspace = 0;
            std::string  title;
            bool         fromVisibleWorkspace = false;
            float        savedAlpha           = 1.F;
            bool         alphaHidden          = false;
        };

        void collectWindows();
        void hideRealWindows();
        void restoreRealWindows();
        void computeLayout();
        void selectIndex(int idx);
        void commit();

        SBoxF interpolate(const SEntry& e) const;

        PHLMONITORREF        m_monitor;
        std::vector<SEntry>  m_entries;
        std::vector<STile>   m_tiles;
        std::vector<SBand>   m_bands;

        CWindowCapture       m_capture;

        int                  m_selected = -1;
        int                  m_hovered  = -1;
        bool                 m_closing  = false;
        bool                 m_commit   = false;

        PHLWINDOWREF         m_originalFocus;
        PHLWORKSPACEREF      m_originalWorkspace;

        PHLANIMVAR<float>    m_progress; // 0 = desktop, 1 = overview

        friend class COverviewPassElement;
    };

} // namespace hyprspace
