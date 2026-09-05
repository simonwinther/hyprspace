// hyprspace - committing a selection to the compositor.

#pragma once

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

namespace hyprspace {

    // Switch to the window's workspace (if needed) and focus it.
    //
    // Focusing alone is not enough: with input:follow_mouse enabled — Hyprland's
    // default, and Omarchy's — the pointer sitting over some other window steals
    // the focus straight back as soon as the overlay closes, so the selection
    // silently does nothing. Hyprland's own window-cycling actions solve this by
    // warping the cursor onto the new window and replaying a synthetic motion
    // with m_forcedFocus pinned; hyprspace does the same.
    // An explicit overview click may reveal a window that the compositor's
    // fullscreen focus policy would otherwise redirect away from.
    void focusSelection(PHLWINDOW window, bool warpCursor, bool revealFullscreenBlocked = false);

} // namespace hyprspace
