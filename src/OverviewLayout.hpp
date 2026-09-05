#pragma once

#include "Geometry.hpp"

#include <limits>
#include <span>

namespace hyprspace {

    struct SOverviewWindowInput {
        SBoxF desktop;
        bool  fullscreen = false;
    };

    struct SOverviewWindowLayout {
        std::vector<SBoxF> boxes;
        size_t             columns = 0;
        bool               spread  = false;
    };

    // Only the previews are rearranged. A fullscreen layout target can contain
    // the entire work area, so its position cannot recover the obscured layout.
    inline SOverviewWindowLayout layoutOverviewWindows(std::span<const SOverviewWindowInput> windows, const SBoxF& usable, size_t fixedColumns = 0) {
        SOverviewWindowLayout result;
        for (const auto& window : windows)
            result.boxes.push_back(window.desktop);

        if (windows.empty() || !std::isfinite(usable.w) || !std::isfinite(usable.h) || usable.w <= 0 || usable.h <= 0)
            return result;
        if (std::ranges::none_of(windows, [](const auto& window) { return window.fullscreen; }))
            return result;

        const auto aspect = [](const SBoxF& box) {
            const double ratio = box.w / box.h;
            return std::isfinite(ratio) && ratio > 0 ? ratio : 1.0;
        };

        if (windows.size() == 1) {
            result.boxes.front() = fitBox(usable, aspect(windows.front().desktop));
            result.columns       = 1;
            return result;
        }

        result.spread        = true;
        const double padding = std::min(32.0, std::min(usable.w, usable.h) * 0.04);
        const SBoxF  inner{usable.x + padding, usable.y + padding, usable.w - 2 * padding, usable.h - 2 * padding};
        const double gap = std::min(24.0, std::min(inner.w, inner.h) / (2.0 * windows.size()));

        double       bestMinimumArea = -1;
        double       bestTotalArea   = -1;
        const size_t first           = fixedColumns ? std::min(fixedColumns, windows.size()) : 1;
        const size_t last            = fixedColumns ? first : windows.size();

        // Maximize the smallest thumbnail, then the total area. Keeping input
        // order and centering each partial row makes the arrangement stable.
        for (size_t columns = first; columns <= last; ++columns) {
            const size_t rows   = (windows.size() + columns - 1) / columns;
            const double width  = (inner.w - gap * (columns - 1)) / columns;
            const double height = (inner.h - gap * (rows - 1)) / rows;
            if (width <= 0 || height <= 0)
                continue;

            double minimumArea = std::numeric_limits<double>::max();
            double totalArea   = 0;
            for (const auto& window : windows) {
                const auto box = fitBox({0, 0, width, height}, aspect(window.desktop));
                minimumArea    = std::min(minimumArea, box.w * box.h);
                totalArea += box.w * box.h;
            }
            if (minimumArea < bestMinimumArea || (minimumArea == bestMinimumArea && totalArea <= bestTotalArea))
                continue;

            bestMinimumArea = minimumArea;
            bestTotalArea   = totalArea;
            result.columns  = columns;
            for (size_t i = 0; i < windows.size(); ++i) {
                const size_t row   = i / columns;
                const size_t col   = i % columns;
                const size_t count = std::min(columns, windows.size() - row * columns);
                const double left  = inner.x + (inner.w - count * width - (count - 1) * gap) / 2.0;
                result.boxes[i]    = fitBox({left + col * (width + gap), inner.y + row * (height + gap), width, height}, aspect(windows[i].desktop));
            }
        }

        return result;
    }

} // namespace hyprspace
