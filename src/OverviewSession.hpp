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
        Vector2D        scale      = {1, 1};
        bool            resizeLeft = false, resizeTop = false;
        bool            moved = false;
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

        bool         live() const;
        void         begin();
        void         stopInput();
        void         restoreVisibility();
        void         reconcileVisibility();
        bool         covers(PHLMONITOR monitor) const;
        void         hideWindow(PHLWINDOW window);
        void         pointer(const Vector2D& pos);
        void         refreshPointerTarget();
        void         keyboard(COverview& view);
        void         followKeyboardFocus();
        bool         button(uint32_t button, bool pressed, uint32_t mods);
        void         cancelDrag();
        void         monitorRemoved(PHLMONITOR monitor);
        void         damage();
        PHLWORKSPACE workspace(const SOverviewTarget& target) const;
        Vector2D     desktopPoint(const SOverviewTarget& target) const;
        void         establishTarget();
        void         ownCursor(bool own);
        bool         cursorOwned() const {
            return m_cursorOwned;
        }
        SP<Render::ITexture>           dragTexture() const;
        COverview*                     keyboardView() const;
        std::optional<SOverviewTarget> hit(const Vector2D& pos) const;

      private:
        CVisibilityLedger<PHLWINDOWREF> m_visibility;
        SP<Render::ITexture>            m_dragTexture;
        bool                            m_cursorOwned = false;
        Vector2D                        m_pointer;
    };

    extern std::unique_ptr<COverviewSession> g_overviewSession;
    COverviewSession&                        session();
} // namespace hyprspace
