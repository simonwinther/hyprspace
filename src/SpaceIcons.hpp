// Trusted, bundled space symbols; no Hyprland or filesystem dependency.
#pragma once

#include "Raster.hpp"

#include <string_view>

namespace hyprspace {
    inline constexpr int MAX_SPACE_ICON_SIZE = 1024;

    // Physical pixels. Unknown keys use the catalog default; invalid sizes or
    // non-finite colors return an empty image. Output is premultiplied ARGB32.
    SImage renderSpaceIcon(std::string_view key, const SRgba& color, int size);
} // namespace hyprspace
