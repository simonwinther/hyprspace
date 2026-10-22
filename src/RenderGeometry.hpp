#pragma once

#include "Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <limits>

namespace hyprspace::renderGeometry {
    // Native border damage is an integer Pixman region. Fractional origins and
    // sizes truncate separately there, which can clip the last row or column
    // of a one-pixel outline. Snap both endpoints before queuing paired shapes.
    inline SBoxF physicalBox(const SBoxF& logical, double scale) {
        const double left = std::round(logical.x * scale), top = std::round(logical.y * scale);
        const double right = std::round((logical.x + logical.w) * scale), bottom = std::round((logical.y + logical.h) * scale);
        return {left, top, std::max(0.0, right - left), std::max(0.0, bottom - top)};
    }

    inline int physicalRadius(double logical, double scale) {
        return std::max(0, static_cast<int>(std::round(logical * scale)));
    }

    // Hyprland scales borderSize itself; boxes and corner radii are physical.
    // Keep outerRound unspecified so it derives from the actual scaled width.
    inline int logicalBorderWidth(double logical) {
        return logical <= 0 ? 0 : std::max(1, static_cast<int>(std::round(logical)));
    }

    struct SShape {
        SBoxF box;
        int   radius = 0, borderWidth = 0;
    };

    // One host-testable contract for native solid fills and outlines. Texture
    // sampling intentionally keeps its separate fractional coordinate path.
    inline std::optional<SShape> physicalShape(const SBoxF& logical, double scale, double radius = 0, double borderWidth = 0) {
        if (!(scale > 0) || !(logical.w > 0) || !(logical.h > 0) || !std::isfinite(scale) || !std::isfinite(logical.x) || !std::isfinite(logical.y) ||
            !std::isfinite(logical.w) || !std::isfinite(logical.h) || !std::isfinite(radius) || !std::isfinite(borderWidth))
            return std::nullopt;
        const auto       box     = physicalBox(logical, scale);
        constexpr double maximum = std::numeric_limits<int>::max();
        if (box.w <= 0 || box.h <= 0 || !std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.w) || !std::isfinite(box.h) || std::abs(box.x) > maximum ||
            std::abs(box.y) > maximum || box.x + box.w > maximum || box.y + box.h > maximum || std::max(0.0, radius) * scale > maximum || borderWidth > maximum)
            return std::nullopt;
        return SShape{box, physicalRadius(std::max(0.0, radius), scale), logicalBorderWidth(borderWidth)};
    }
} // namespace hyprspace::renderGeometry
