#pragma once

#include "OverviewSession.hpp"
#include "Scrolling.hpp"

#include <functional>
#include <hyprland/src/plugins/HookSystem.hpp>

namespace hyprspace::hooks {
    void           install(std::function<bool()> ownsKeyboard, std::function<bool()> launchEnabled, std::function<bool(PHLMONITOR)> promotePanels);
    void           uninstall();
    void           ownCursor(bool own);
    void           renderPanels(PHLMONITOR monitor);
    bool           keyboardOwned();
    double         scrollFactor();
    void           syncKeyboardFocus();
    CFunctionHook* attach(const std::string& name, const std::string& signature, void* callback);

    // Desktop coordinates are scoped to one synchronous native operation.
    // The hardware pointer never warps into a miniature's desktop position.
    void                           atDesktopPoint(const Vector2D& point, const std::function<void()>& action);
    bool                           place(PHLWINDOW window, const SOverviewTarget& source, const SOverviewTarget& destination, bool resize = false);
    void                           cancelPlacement();
    std::optional<SScrollViewport> scrollingViewport(const SOverviewTarget& target);
    bool                           panWorkspace(const SOverviewTarget& target, double distance);
    bool                           stepWorkspace(const SOverviewTarget& target, int direction, bool fromSelection);
} // namespace hyprspace::hooks
