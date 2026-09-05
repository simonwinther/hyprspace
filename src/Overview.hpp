// hyprspace - the full-screen workspace overview.
//
// One tile per non-empty workspace, every tile at the monitor's aspect ratio,
// each showing live windows. Fullscreen workspaces spread their previews apart.

#pragma once

#include "Capture.hpp"
#include "Geometry.hpp"
#include "Input.hpp"
#include "OverviewSession.hpp"
#include "PreviewStyle.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
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

        // A Super+drag in flight. The gesture can wander onto another output,
        // so the overview that started it keeps the pointer until it ends.
        bool dragging() const {
            return session().drag.active();
        }

        bool needsBlur() const {
            return m_needsBlur;
        }

        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }

        // --- input ---
        bool onKey(xkb_keysym_t sym, uint32_t mods, bool pressed);
        void onMouseMove(const Vector2D& globalPos);
        bool onMouseButton(uint32_t button, bool pressed, uint32_t mods);
        void onScroll(const SScrollInput& event);

        // --- render lifecycle ---
        void                           prepareFrame(); // capture live window contents
        std::vector<UP<IPassElement>>  buildPass();    // ordinary elements appended to Hyprland's pass
        void                           damage();
        std::optional<SOverviewTarget> targetAt(const Vector2D& globalPos) const;
        std::optional<SOverviewTarget> selectedTarget() const;
        void                           selectTarget(const SOverviewTarget& target);
        std::vector<SOverviewTarget>   inspectTargets() const;
        SP<Render::ITexture>           textureFor(PHLWINDOW window) const {
            return m_capture.textureFor(window);
        }

      private:
        // Desktop and preview bounds are both in monitor-local logical pixels.
        struct SWindowSlot {
            PHLWINDOWREF                window;
            SBoxF                       desktopRect;
            SBoxF                       previewRect;
            Fullscreen::eFullscreenMode fullscreen  = Fullscreen::FSMODE_NONE;
            int                         renderLayer = 0;
            bool                        blur        = false;
        };

        // One workspace tile.
        struct SEntry {
            long                     workspaceId = 0;
            std::string              name;
            std::string              workspaceName;
            std::vector<SWindowSlot> windows;
            std::vector<size_t>      drawOrder;
            size_t                   previewColumns = 0;
            bool                     spread         = false;
            bool                     isActive       = false;
            SBoxF                    target         = {}; // final cell
            SBoxF                    start          = {}; // where it animates from
        };

        void collect();
        void refreshWindows();
        void computeLayout();
        void updateWindowLayout(SEntry& entry);

        // Which tile the open/close zoom starts full-screen from; -1 for none.
        void anchorAnimation(int entryIdx);
        int  committedEntry() const;

        void selectIndex(int idx);
        void commit();
        void commitSelection();
        void settleWorkspaceAnimations() const;

        // Actual desktop bounds, independent of its overview placement.
        SBoxF                  boxFor(const PHLWINDOW& w) const;
        SWindowPreviewGeometry geometryFor(const SEntry& entry, const SWindowSlot& slot) const;

        SBoxF interpolate(const SEntry& e) const;

        // Where a window at monitor-local logical `r` lands inside cell `cell`.
        SBoxF windowBoxInCell(const SBoxF& r, const SBoxF& cell) const;

        // Hit test: which tile, and which window inside it.
        int       tileAtLocal(const Vector2D& local) const;
        PHLWINDOW windowAtLocal(const Vector2D& local) const;

        // The workspace a tile stands for, created if the last window was
        // dragged off it and Hyprland reaped it.
        PHLWORKSPACE workspaceForEntry(const SEntry& e) const;

        PHLMONITORREF m_monitor;

        // The part of the monitor windows actually live in — the full output
        // minus whatever layer surfaces reserved (the bar). Tiles map this, not
        // the whole output, so no tile carries an empty strip where the bar sits.
        SBoxF               m_usable;
        std::vector<SEntry> m_entries;
        std::vector<STile>  m_tiles;

        CWindowCapture m_capture;

        int  m_selected        = -1;
        int  m_hovered         = -1;
        bool m_closing         = false;
        bool m_needsBlur       = false;
        int  m_animationAnchor = -1;

        CScrollAccumulator m_scroll;

        PHLWINDOWREF    m_originalFocus;
        PHLWORKSPACEREF m_originalWorkspace;
        PHLWINDOWREF    m_clickedWindow;
        PHLWINDOWREF    m_hoveredWindow;

        // Set when a number key names a workspace that has no tile; commit()
        // switches to it (creating it) instead of using the selection.
        long m_gotoWorkspace = 0;

        PHLANIMVAR<float> m_progress; // 0 = desktop, 1 = overview
    };

} // namespace hyprspace
