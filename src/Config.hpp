#pragma once

#include "Geometry.hpp"
#include "globals.hpp"

#include <hyprland/src/helpers/Color.hpp>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace hyprspace::config {

    // Registers every `plugin:hyprspace:*` value. Must run inside pluginInit.
    void registerAll();

    // --- overview ---
    float      overviewBgDim();
    CHyprColor overviewBgColor();
    int        overviewPadding();
    int        overviewGap();
    [[deprecated("compatibility option has no effect")]]
    int        overviewBandGap();
    int        overviewRounding();
    int        overviewBorderSize();
    CHyprColor overviewActiveBorder();
    CHyprColor overviewHoverBorder();
    bool       overviewShowLabels();
    bool       overviewIncludeSpecial();
    bool       overviewExcludesWorkspace(std::string_view name);
    [[deprecated("compatibility option has no effect")]]
    bool         overviewAllWorkspaces();
    bool         overviewAllMonitors();
    CHyprColor   overviewLabelColor();
    CHyprColor   overviewTileBgColor();
    CHyprColor   overviewTileBorderColor();
    CHyprColor   overviewTitleBgColor();
    std::string  overviewFont();
    std::string  overviewPreviewFilter();
    xkb_keysym_t overviewZoomKey();
    xkb_keysym_t overviewEmptyWorkspaceKey();
    xkb_keysym_t overviewCloseWorkspaceKey();
    xkb_keysym_t overviewWindowViewKey();
    xkb_keysym_t overviewFollowKey();
    xkb_keysym_t overviewHelpKey();
    bool         overviewShowFollowHint();
    uint32_t     overviewWorkspaceDragModifiers();
    bool         overviewWheelZoom();

    // Outline and badge marking the window that is fullscreen.
    CHyprColor overviewFullscreenBorder();

    // --- switcher ---
    int         switcherIconSize();
    int         switcherPadding();
    int         switcherGap();
    int         switcherRounding();
    CHyprColor  switcherBgColor();
    CHyprColor  switcherHighlightColor();
    CHyprColor  switcherTextColor();
    bool        switcherShowTitle();
    bool        switcherCurrentWorkspaceOnly();
    std::string switcherFont();

    // --- shared ---
    bool        followMouse();
    bool        warpCursor();
    bool        diagnosticsEnabled();
    bool        spacesEnabled();
    std::string spacesTheme();

} // namespace hyprspace::config
