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
        bool onMouseButton(uint32_t button, bool pressed, uint32_t mods);
        void onScroll(double delta);

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

            // eFullscreenMode this window was in when the overview opened;
            // 0 is FSMODE_NONE. Non-zero means the overview took it out of
            // fullscreen and owes it the state back on close.
            uint8_t      savedFullscreen = 0;
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

        // Super + drag: move a window between workspaces, or resize it in place.
        enum class EDrag : uint8_t {
            NONE = 0,
            MOVE,
            RESIZE,
        };

        struct SDrag {
            EDrag        mode       = EDrag::NONE;
            PHLWINDOWREF window;
            int          sourceTile = -1; // tile the window was picked up from
            int          targetTile = -1; // tile currently under the pointer
            Vector2D     grabOffset = {}; // pointer minus the window's top-left
            Vector2D     lastPos    = {}; // previous pointer position, overview-local
            SBoxF        box        = {}; // where the lifted window is drawn
            bool         moved      = false;

            bool         active() const {
                return mode != EDrag::NONE;
            }
        };

        void      collect();
        void      computeLayout();

        // Which tile the open/close zoom starts full-screen from; -1 for none.
        void      anchorAnimation(int entryIdx);
        int       committedEntry() const;

        void      hideRealWindows();
        void      restoreRealWindows();

        // Fullscreen is undone for as long as the overview is up, and put back
        // when it closes.
        void      suspendFullscreen();
        void      restoreFullscreen();
        void      selectIndex(int idx);
        void      commit();
        void      commitSelection();
        void      settleWorkspaceAnimations() const;

        // Where a window is drawn; fullscreen windows use their restored box.
        SBoxF     boxFor(const PHLWINDOW& w) const;

        SBoxF     interpolate(const SEntry& e) const;

        // Where a window at monitor-local logical `r` lands inside cell `cell`.
        SBoxF     windowBoxInCell(const SBoxF& r, const SBoxF& cell) const;

        // Overview px -> monitor logical px, for a window living in tile `idx`.
        double    tileScale(int idx) const;

        // Hit test: which tile, and which window inside it.
        int       tileAtLocal(const Vector2D& local) const;
        PHLWINDOW windowAtLocal(const Vector2D& local) const;

        bool      beginDrag(EDrag mode, const Vector2D& local);
        void      updateDrag(const Vector2D& local);
        void      finishDrag();

        // The workspace a tile stands for, created if the last window was
        // dragged off it and Hyprland reaped it.
        PHLWORKSPACE workspaceForEntry(const SEntry& e) const;

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

        SDrag               m_drag;

        PHLWINDOWREF        m_originalFocus;
        PHLWORKSPACEREF     m_originalWorkspace;
        PHLWINDOWREF        m_clickedWindow;

        // Set when a number key names a workspace that has no tile; commit()
        // switches to it (creating it) instead of using the selection.
        long                m_gotoWorkspace = 0;

        PHLANIMVAR<float>   m_progress; // 0 = desktop, 1 = overview

        friend class COverviewPassElement;
    };

} // namespace hyprspace
