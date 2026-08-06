#pragma once

#include "globals.hpp"

#include <hyprland/src/helpers/Color.hpp>

#include <string>

namespace hyprspace::config {

    // Registers every `plugin:hyprspace:*` value. Must run inside pluginInit.
    void        registerAll();

    // --- overview ---
    float       overviewBgDim();
    CHyprColor  overviewBgColor();
    int         overviewPadding();
    int         overviewGap();
    int         overviewBandGap();
    int         overviewRounding();
    int         overviewBorderSize();
    CHyprColor  overviewActiveBorder();
    CHyprColor  overviewHoverBorder();
    bool        overviewShowLabels();
    bool        overviewIncludeSpecial();
    bool        overviewAllWorkspaces();
    CHyprColor  overviewLabelColor();
    CHyprColor  overviewTileBgColor();
    CHyprColor  overviewTileBorderColor();
    CHyprColor  overviewTitleBgColor();
    std::string overviewFont();

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

} // namespace hyprspace::config
