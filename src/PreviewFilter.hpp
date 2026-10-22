#pragma once

#include "globals.hpp"

#include <hyprland/src/render/Texture.hpp>

#include <cstddef>

namespace hyprspace {
    struct SPreviewFilterResources {
        size_t entries = 0, bytes = 0, peakBytes = 0;
        size_t passes = 0, hits = 0, failures = 0, allocations = 0, animatedBypasses = 0;
    };

    // Resample captured window pixels only. The resulting texture still goes
    // through Hyprland's ordinary alpha, colour, blur and rounding compositor.
    SP<Render::ITexture>           previewFilteredTexture(const SP<Render::ITexture>& source, const Vector2D& destination, bool stableSize = true);
    void                           previewFilterCaptured(const SP<Render::ITexture>& source);
    void                           prunePreviewFilters();
    void                           clearPreviewFilters();
    void                           destroyPreviewFilters();
    const SPreviewFilterResources& previewFilterResources();
} // namespace hyprspace
