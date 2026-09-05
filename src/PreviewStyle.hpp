#pragma once

#include "Geometry.hpp"

#include <algorithm>
#include <cmath>

namespace hyprspace {

    inline float previewUnit(float value, float fallback = 0.F) {
        return std::isfinite(value) ? std::clamp(value, 0.F, 1.F) : fallback;
    }

    struct SWorkspacePreviewStyle {
        float visibility;
        float windowVisibility;
        float plateVisibility;
    };

    inline SWorkspacePreviewStyle workspacePreviewStyle(bool anchor, bool selected, float progress) {
        const float p          = previewUnit(progress);
        const float visibility = anchor ? 1.F : p;
        return {visibility, visibility * (1.F - (selected ? 0.F : 0.1F) * p), visibility * p};
    }

    // Capture omits compositor opacity. Apply it once, independently of the
    // overview's animation; the temporary alpha used to hide real windows must
    // never enter this calculation.
    inline float windowPreviewOpacity(float opacity, bool forceOpaque, float visibility) {
        return (forceOpaque ? 1.F : previewUnit(opacity, 1.F)) * previewUnit(visibility);
    }

    struct SWindowPreviewGeometry {
        SBoxF box;
        SBoxF clip;
    };

    inline bool previewContains(const SWindowPreviewGeometry& geometry, double x, double y) {
        return geometry.box.contains(x, y) && geometry.clip.contains(x, y);
    }

    // Windows obscured by fullscreen become visible as their previews spread
    // apart, and reach their compositor opacity again at the desktop endpoint.
    inline float overviewWindowVisibility(float desktopVisibility, bool anchor, float progress) {
        return anchor ? std::lerp(previewUnit(desktopVisibility), 1.F, previewUnit(progress)) : 1.F;
    }

    // Every window in the anchor workspace travels back to its own desktop
    // bounds. Fullscreen clipping must reach the entire output, including panels.
    inline SWindowPreviewGeometry overviewWindowGeometry(const SWindowPreviewGeometry& overview, const SWindowPreviewGeometry& desktop, bool anchor, float progress) {
        if (!anchor)
            return overview;

        const double p           = previewUnit(progress);
        const auto   interpolate = [p](const SBoxF& from, const SBoxF& to) {
            return SBoxF{std::lerp(from.x, to.x, p), std::lerp(from.y, to.y, p), std::lerp(from.w, to.w, p), std::lerp(from.h, to.h, p)};
        };
        return {interpolate(desktop.box, overview.box), interpolate(desktop.clip, overview.clip)};
    }

} // namespace hyprspace
