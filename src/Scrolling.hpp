#pragma once

#include "Geometry.hpp"
#include "Input.hpp"

namespace hyprspace {
    struct SScrollViewport {
        bool horizontal = true;
        bool previous = false, next = false;
    };

    struct SScrollControls {
        SBoxF previous, next;
    };

    inline SScrollControls scrollControls(const SBoxF& cell, bool horizontal) {
        const double size = std::min(34.0, std::min(cell.w, cell.h) / 4.0);
        const double pad  = std::min(8.0, size / 3.0);
        if (horizontal)
            return {{cell.x + pad, cell.cy() - size / 2, size, size}, {cell.x + cell.w - pad - size, cell.cy() - size / 2, size, size}};
        return {{cell.cx() - size / 2, cell.y + pad, size, size}, {cell.cx() - size / 2, cell.y + cell.h - pad - size, size, size}};
    }

    // Wheels move a quarter viewport per detent. Finger input moves content
    // by the same logical distance inside the preview, without quantization.
    inline double scrollDistance(const SScrollInput& event, double previewExtent) {
        if (!std::isfinite(event.delta) || !std::isfinite(previewExtent) || previewExtent <= 0)
            return 0;
        const double distance = event.wheel ? (event.value120 ? event.value120 / 120.0 : event.delta / 15.0) * 0.25 : event.delta / previewExtent;
        return std::clamp(distance, -1.0, 1.0);
    }
} // namespace hyprspace
