#pragma once

#include "globals.hpp"

namespace hyprspace {

    // Readable without loading or executing the plugin. Keep the full header
    // versions here; check-abi.sh applies Hyprland's patch-version stripping.
    [[gnu::used, gnu::section(".hyprspace.abi")]]
    static const char BUILD_ABI[] =
        GIT_COMMIT_HASH "_aq_" AQUAMARINE_VERSION "_hu_" HYPRUTILS_VERSION "_hg_" HYPRGRAPHICS_VERSION "_hc_" HYPRCURSOR_VERSION "_hlg_" HYPRLANG_VERSION;

} // namespace hyprspace
