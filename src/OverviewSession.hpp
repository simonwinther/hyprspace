#pragma once

#include "Capture.hpp"
#include "Interaction.hpp"
#include "Input.hpp"

#include <memory>
#include <optional>

namespace hyprspace {
    class COverview;

    struct SOverviewTarget {
        SWorkspaceIdentity workspace;
        PHLMONITORREF      monitor;
        PHLWINDOWREF       window;
        Vector2D           desktop;
        SBoxF              preview, desktopBox, monitorBox, previewClip;
    };

    struct SOverviewDrag {
        enum EMode { NONE, MOVE, RESIZE } mode = NONE;
        PHLWINDOWREF    window;
        SOverviewTarget source;
        Vector2D        pickup, offset, desktopOffset;
        SBoxF           box;
        SBoxF           sourceCell; // monitor-local displayed workspace at pickup
        Vector2D        scale      = {1, 1};
        bool            resizeLeft = false, resizeTop = false;
        double          threshold = 0;
        bool            moved     = false;
        bool            active() const {
            return mode != NONE;
        }
    };

    class COverviewSession {
      public:
        COverviewSession();
        ~COverviewSession();
        std::vector<std::unique_ptr<COverview>> views;
        CTargetSelection<SOverviewTarget>       selection;
        SOverviewDrag                           drag;
        // A released resize is still pictured while the native controller
        // flushes its final motion. It no longer owns a pressed button.
        std::optional<SOverviewDrag> pendingResize;

        bool                    live() const;
        void                    begin();
        void                    stopInput();
        void                    restoreVisibility();
        void                    reconcileVisibility();
        bool                    covers(PHLMONITOR monitor) const;
        void                    hideWindow(PHLWINDOW window);
        void                    pointer(const Vector2D& pos, bool userMotion = false);
        void                    observePointer(const Vector2D& pos);
        void                    refreshPointerTarget(PHLMONITOR renderedMonitor);
        void                    keyboard(COverview& view);
        std::optional<uint64_t> zoomPress();
        void                    zoomRelease(uint64_t token);
        void                    cancelZoom();
        bool                    zoomScroll(const SScrollInput& event, const Vector2D& pos);
        void                    cancelPan();
        bool                    panHeld() const {
            return m_pan.has_value();
        }
        bool panning() const {
            return m_pan && m_pan->started;
        }
        bool                           panAvailable() const;
        bool                           zoomHeld() const;
        bool                           zoomLocked() const;
        std::optional<SOverviewTarget> zoomTarget() const;
        bool                           zoomEdgeReady(const COverview& view) const;
        std::optional<EDirection>      zoomEdgePending() const;
        void                           followKeyboardFocus();
        bool                           button(uint32_t button, bool pressed, uint32_t mods);
        void                           cancelDrag();
        void                           finishPlacement();
        void                           monitorRemoved(PHLMONITOR monitor);
        void                           damage();
        PHLWORKSPACE                   workspace(const SOverviewTarget& target) const;
        Vector2D                       desktopPoint(const SOverviewTarget& target) const;
        void                           establishTarget();
        void                           ownCursor(bool own);
        bool                           cursorOwned() const {
            return m_cursorOwned;
        }
        SP<Render::ITexture>           dragTexture() const;
        COverview*                     keyboardView() const;
        std::optional<SOverviewTarget> hit(const Vector2D& pos) const;

      private:
        void       syncSelection();
        void       updateZoom();
        void       retargetSelection(COverview& view);
        void       cancelZoomEdge();
        bool       zoomEdgeEnabled(const COverview& view) const;
        COverview* zoomView() const;
        void       advanceZoomEdge(PHLMONITOR renderedMonitor);
        void       updatePan();
        void       updateCursor();
        struct SInspectionPan {
            SWorkspaceIdentity workspace;
            PHLMONITORREF      monitor;
            bool               started = false;
        };
        struct SZoomEdgeIntent {
            SWorkspaceIdentity source, destination;
            PHLMONITORREF      monitor;
        };
        CZoomEdgeHover                  m_zoomEdgeHover;
        std::optional<SZoomEdgeIntent>  m_zoomEdgeIntent;
        CZoomHoldState                  m_zoomHolds;
        std::optional<SOverviewTarget>  m_zoomTarget;
        std::optional<SInspectionPan>   m_pan;
        CVisibilityLedger<PHLWINDOWREF> m_visibility;
        SP<Render::ITexture>            m_dragTexture;
        bool                            m_cursorOwned = false;
        Vector2D                        m_pointer;
    };

    extern std::unique_ptr<COverviewSession> g_overviewSession;
    COverviewSession&                        session();
} // namespace hyprspace
