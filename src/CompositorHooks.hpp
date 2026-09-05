#pragma once

#include "OverviewSession.hpp"

#include <functional>
#include <hyprland/src/plugins/HookSystem.hpp>

namespace hyprspace::hooks {
    void           install(std::function<bool()> ownsKeyboard, std::function<bool()> launchEnabled);
    void           uninstall();
    void           ownCursor(bool own);
    void           renderPanels(PHLMONITOR monitor);
    bool           keyboardOwned();
    CFunctionHook* attach(const std::string& name, const std::string& signature, void* callback);

    // Desktop coordinates are scoped to one synchronous native operation.
    // The hardware pointer never warps into a miniature's desktop position.
    void atDesktopPoint(const Vector2D& point, const std::function<void()>& action);
    bool place(PHLWINDOW window, const SOverviewTarget& source, const SOverviewTarget& destination, bool resize = false);
    void cancelPlacement();
} // namespace hyprspace::hooks
