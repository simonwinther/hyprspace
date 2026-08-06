#include "Focus.hpp"

#include "Config.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace hyprspace {

    void focusSelection(PHLWINDOW window, bool warpCursor) {
        if (!window)
            return;

        if (const auto WS = window->m_workspace; WS && !WS->isVisible()) {
            if (WS->m_isSpecialWorkspace)
                (void)Config::Actions::toggleSpecial(WS);
            else
                (void)Config::Actions::changeWorkspaceOnCurrentMonitor(WS);
        }

        (void)Config::Actions::focus(window);

        if (!warpCursor)
            return;

        window->warpCursor();

        // Pin the focus across the synthetic motion, otherwise follow_mouse
        // resolves the pointer's new position and can land somewhere else.
        g_pInputManager->m_forcedFocus = window;
        g_pInputManager->simulateMouseMovement();
        g_pInputManager->m_forcedFocus.reset();
    }

} // namespace hyprspace
