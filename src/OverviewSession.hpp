#pragma once

#include "Capture.hpp"
#include "Interaction.hpp"
#include "Input.hpp"

#include <hyprland/src/helpers/AnimatedVariable.hpp>

#include <memory>
#include <optional>
#include <expected>
#include <functional>

namespace hyprspace {
    class COverview;
    class CWindowBoard;

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

    struct SWorkspacePreviewWindow {
        PHLWINDOWREF         window;
        SBoxF                box, clip;
        float                visibility = 1;
        bool                 fullscreen = false, blur = false;
        SP<Render::ITexture> texture;
    };

    struct SWorkspaceDragPreview {
        SBoxF                                cell;
        std::vector<SWorkspacePreviewWindow> windows;
        std::string                          label;
        float                                progress = 1;
    };

    struct SWorkspaceDrag {
        PHLWORKSPACE          workspace;
        SWorkspaceIdentity    identity;
        PHLMONITORREF         sourceMonitor, targetMonitor;
        PHLMONITORREF         focusedMonitor;
        PHLWINDOWREF          focusedWindow;
        Vector2D              pickup, offset;
        SBoxF                 box;
        double                threshold = 0;
        bool                  moved     = false;
        SWorkspaceDragPreview preview;
        std::function<bool()> connectionsValid;
    };

    // Released cards keep a picture, never a button or command target.
    struct SWorkspaceSettle {
        PHLWORKSPACE          workspace;
        SWorkspaceIdentity    identity;
        PHLMONITORREF         monitor;
        SWorkspaceDragPreview from;
    };

    class COverviewSession {
      public:
        COverviewSession();
        ~COverviewSession();
        std::vector<std::unique_ptr<COverview>> views;
        CTargetSelection<SOverviewTarget>       selection;
        SOverviewDrag                           drag;
        std::optional<SWorkspaceDrag>           workspaceDrag;
        std::optional<SWorkspaceSettle>         workspaceSettle;
        // A released resize is still pictured while the native controller
        // flushes its final motion. It no longer owns a pressed button.
        std::optional<SOverviewDrag> pendingResize;

        bool          live() const;
        CWindowBoard* windowBoard() const {
            return m_windowBoard.get();
        }
        bool            windowViewActive() const;
        SDispatchResult windowView(const std::string& args);
        void            commitWindowView();
        void            selectBoardTarget(std::optional<SOverviewTarget> target, bool pointer = false);
        bool            gestureActive() const {
            return drag.active() || workspaceDrag.has_value();
        }
        void                    begin();
        void                    stopInput();
        void                    restoreVisibility();
        void                    reconcileVisibility();
        bool                    covers(PHLMONITOR monitor) const;
        SDispatchResult         emptyWorkspace(PHLMONITOR monitor = nullptr);
        bool                    preparedWorkspace(PHLWORKSPACE workspace) const;
        PHLWORKSPACE            preparedLifetime(PHLWORKSPACE workspace) const;
        void                    hideWindow(PHLWINDOW window);
        void                    pointer(const Vector2D& pos, bool userMotion = false);
        void                    observePointer(const Vector2D& pos);
        void                    refreshPointerTarget(PHLMONITOR renderedMonitor);
        void                    keyboard(COverview& view);
        std::optional<uint64_t> zoomPress();
        void                    zoomRelease(uint64_t token);
        void                    cancelZoom();
        bool                    zoomScroll(const SScrollInput& event, const Vector2D& pos);
        void                    zoomStep(int direction);
        void                    panKey(const Vector2D& delta);
        std::optional<uint64_t> panPress();
        void                    panRelease(uint64_t token);
        uint64_t                zoomGeneration() const {
            return zoomHeld() ? m_zoomGeneration : 0;
        }
        bool keyboardPanHeld() const {
            return m_pan && m_pan->keyboard;
        }
        void releaseMousePan() {
            if (!keyboardPanHeld())
                cancelPan();
        }
        void cancelPan();
        bool panHeld() const {
            return m_pan.has_value();
        }
        bool panning() const {
            return m_pan && m_pan->started;
        }
        bool                                 panAvailable() const;
        bool                                 zoomHeld() const;
        bool                                 zoomLocked() const;
        std::optional<SOverviewTarget>       zoomTarget() const;
        bool                                 zoomEdgeReady(const COverview& view) const;
        std::optional<EDirection>            zoomEdgePending() const;
        void                                 followKeyboardFocus();
        bool                                 button(uint32_t button, bool pressed, uint32_t mods);
        void                                 cancelDrag();
        void                                 cancelWorkspaceDrag();
        void                                 validateWorkspaceDrag();
        void                                 cancelWorkspaceSettle();
        void                                 validateWorkspaceSettle();
        float                                workspaceSettleProgress() const;
        std::optional<SWorkspaceDragPreview> workspaceSettlePreview() const;
        void                                 finishPlacement();
        void                                 monitorRemoved(PHLMONITOR monitor);
        void                                 damage();
        PHLWORKSPACE                         workspace(const SOverviewTarget& target) const;
        Vector2D                             desktopPoint(const SOverviewTarget& target) const;
        void                                 establishTarget();
        void                                 ownCursor(bool own);
        bool                                 cursorOwned() const {
            return m_cursorOwned;
        }
        SP<Render::ITexture>           dragTexture() const;
        COverview*                     keyboardView() const;
        std::optional<SOverviewTarget> hit(const Vector2D& pos) const;

      private:
        bool                                     workspaceDragValid() const;
        PHLMONITOR                               workspaceDropMonitor(const Vector2D& point) const;
        bool                                     beginWorkspaceDrag();
        void                                     finishWorkspaceDrag();
        void                                     beginWorkspaceSettle(SWorkspaceDrag gesture, PHLMONITOR destination);
        void                                     syncSelection();
        void                                     updateZoom();
        void                                     retargetSelection(COverview& view);
        void                                     cancelZoomEdge();
        bool                                     zoomEdgeEnabled(const COverview& view) const;
        COverview*                               zoomView() const;
        void                                     advanceZoomEdge(PHLMONITOR renderedMonitor);
        void                                     updatePan();
        void                                     updateCursor();
        COverview*                               emptyWorkspaceView(PHLMONITOR monitor) const;
        COverview*                               emptyWorkspaceButtonView(const Vector2D& point) const;
        std::expected<PHLWORKSPACE, std::string> resolveEmptyWorkspace(PHLMONITOR monitor, bool isEmpty);
        void                                     retainPreparedWorkspace(PHLWORKSPACE workspace);
        void                                     prunePreparedWorkspaces();
        struct SPreparedWorkspace {
            PHLWORKSPACE  workspace;
            PHLMONITORREF monitor;
        };
        struct SInspectionPan {
            SWorkspaceIdentity workspace;
            PHLMONITORREF      monitor;
            bool               started  = false;
            bool               keyboard = false;
        };
        struct SZoomEdgeIntent {
            SWorkspaceIdentity source, destination;
            PHLMONITORREF      monitor;
        };
        CZoomEdgeHover                                     m_zoomEdgeHover;
        std::unique_ptr<CWindowBoard>                      m_windowBoard;
        std::optional<SZoomEdgeIntent>                     m_zoomEdgeIntent;
        CZoomHoldState                                     m_zoomHolds;
        CZoomHoldState                                     m_panHolds;
        uint64_t                                           m_zoomGeneration = 0;
        std::optional<SOverviewTarget>                     m_zoomTarget;
        std::optional<SInspectionPan>                      m_pan;
        CVisibilityLedger<PHLWINDOWREF>                    m_visibility;
        SP<Render::ITexture>                               m_dragTexture;
        SP<Hyprutils::Animation::SAnimationPropertyConfig> m_workspaceSettleConfig;
        PHLANIMVAR<float>                                  m_workspaceSettleProgress;
        std::vector<SPreparedWorkspace>                    m_preparedWorkspaces;
        bool                                               m_emptyButtonPressed = false;
        bool                                               m_cursorOwned        = false;
        Vector2D                                           m_pointer;
    };

    extern std::unique_ptr<COverviewSession> g_overviewSession;
    COverviewSession&                        session();
} // namespace hyprspace
