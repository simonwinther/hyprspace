// hyprspace - the full-screen workspace overview.
//
// One tile per non-empty workspace, every tile at the monitor's aspect ratio,
// each showing that workspace's real live windows laid out exactly as they sit
// on screen.

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
        bool empty() const {
            return m_entries.empty();
        }

        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }

        // --- input ---
        bool onKey(xkb_keysym_t sym, uint32_t mods, bool pressed);
        void onMouseMove(const Vector2D& globalPos);
        bool onMouseButton(uint32_t button, bool pressed);

        // --- render lifecycle ---
        void                          prepareFrame(); // capture live window contents
        std::vector<UP<IPassElement>> buildPass();    // called from the pass element
        void                          damage();

      private:
        // A window as it sits inside its workspace, in monitor-local logical px.
        struct SWindowSlot {
            PHLWINDOWREF window;
            SBoxF        rect;
            float        savedAlpha  = 1.F;
            bool         alphaHidden = false;
        };

        // One workspace tile.
        struct SEntry {
            long                     workspaceId = 0;
            std::string              name;
            std::vector<SWindowSlot> windows;
            bool                     isActive = false;
            SBoxF                    target   = {}; // final cell
            SBoxF                    start    = {}; // where it animates from
        };

        void      collect();
        void      computeLayout();
        void      hideRealWindows();
        void      restoreRealWindows();
        void      selectIndex(int idx);
        void      commit();

        SBoxF     interpolate(const SEntry& e) const;

        // Where a window at monitor-local logical `r` lands inside cell `cell`.
        SBoxF     windowBoxInCell(const SBoxF& r, const SBoxF& cell) const;

        // Hit test: which tile, and which window inside it.
        int       tileAtLocal(const Vector2D& local) const;
        PHLWINDOW windowAtLocal(const Vector2D& local) const;

        PHLMONITORREF       m_monitor;

        // The part of the monitor windows actually live in — the full output
        // minus whatever layer surfaces reserved (the bar). Tiles map this, not
        // the whole output, so no tile carries an empty strip where the bar sits.
        SBoxF               m_usable;
        std::vector<SEntry> m_entries;
        std::vector<STile>  m_tiles;

        CWindowCapture      m_capture;

        int                 m_selected = -1;
        int                 m_hovered  = -1;
        bool                m_closing  = false;

        PHLWINDOWREF        m_originalFocus;
        PHLWORKSPACEREF     m_originalWorkspace;
        PHLWINDOWREF        m_clickedWindow;

        PHLANIMVAR<float>   m_progress; // 0 = desktop, 1 = overview

        friend class COverviewPassElement;
    };

} // namespace hyprspace
