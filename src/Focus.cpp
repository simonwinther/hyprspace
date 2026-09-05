#include "Focus.hpp"

#include "Config.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

namespace hyprspace {

    void focusSelection(PHLWINDOW window, bool warpCursor, bool revealFullscreenBlocked) {
        if (!window || !window->m_isMapped || window->isHidden())
            return;

        if (const auto WS = window->m_workspace; WS && !WS->isVisible()) {
            // Switch the workspace on the monitor that owns it, not on whichever
            // one Hyprland currently calls "current". With an overview up on
            // every output the pointer is over the monitor being picked from
            // while focus is still on the one it started on, so the
            // current-monitor actions would rearrange the wrong screen.
            const auto MONITOR = WS->m_monitor.lock();

            if (!MONITOR) {
                if (WS->m_isSpecialWorkspace)
                    (void)Config::Actions::toggleSpecial(WS);
                else
                    (void)Config::Actions::changeWorkspaceOnCurrentMonitor(WS);
            } else if (WS->m_isSpecialWorkspace)
                MONITOR->setSpecialWorkspace(WS);
            else
                MONITOR->changeWorkspace(WS);
        }

        (void)Config::Actions::focus(window);

        // Respect normal fullscreen cycling first. With on_focus_under_fullscreen=0
        // the action redirects back to the covering window; an explicit preview
        // click should reveal the chosen window instead of silently selecting another.
        if (revealFullscreenBlocked && Desktop::focusState()->window() != window) {
            const auto COVERING = Fullscreen::controller()->getFullscreenWindow(window->m_workspace);
            if (COVERING && COVERING != window && Desktop::focusState()->window() == COVERING &&
                Fullscreen::controller()->getFullscreenModes(COVERING).internal != Fullscreen::FSMODE_NONE && !Fullscreen::controller()->layoutManagedFS(COVERING)) {
                Fullscreen::controller()->setFullscreenMode(COVERING, Fullscreen::FSMODE_NONE);
                (void)Config::Actions::focus(window);
            }
        }

        if (!warpCursor || Desktop::focusState()->window() != window)
            return;

        window->warpCursor();

        // Pin the focus across the synthetic motion, otherwise follow_mouse
        // resolves the pointer's new position and can land somewhere else.
        g_pInputManager->m_forcedFocus = window;
        g_pInputManager->simulateMouseMovement();
        g_pInputManager->m_forcedFocus.reset();
    }

} // namespace hyprspace
