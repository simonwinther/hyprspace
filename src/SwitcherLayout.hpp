#pragma once

#include "Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hyprspace {

    struct SSwitcherLayoutParams {
        double screenW     = 1920;
        double screenH     = 1080;
        double iconSize    = 96;
        double padding     = 24;
        double gap         = 12;
        double titleWidth  = 0;
        double titleHeight = 34;
    };

    struct SSwitcherLayout {
        SBoxF              panel;
        SBoxF              title;
        SBoxF              pageLabel;
        std::vector<STile> tiles; // keys index the complete MRU list
        double             iconSize = 0;
        double             gap      = 0;
        int                columns  = 0;
        int                capacity = 0;
        int                first    = 0;
        int                page     = 0;
        int                pages    = 0;
    };

    inline SSwitcherLayout switcherLayout(int count, int selected, const SSwitcherLayoutParams& p) {
        SSwitcherLayout out;
        if (count <= 0 || !std::isfinite(p.screenW) || !std::isfinite(p.screenH) || p.screenW <= 0 || p.screenH <= 0 || !std::isfinite(p.iconSize) ||
            !std::isfinite(p.padding) || !std::isfinite(p.gap) || !std::isfinite(p.titleWidth) || !std::isfinite(p.titleHeight))
            return out;

        const double margin     = std::min(40.0, std::min(p.screenW, p.screenH) / 10.0);
        const double availableW = p.screenW - 2 * margin;
        const double availableH = p.screenH - 2 * margin;
        const double pad        = std::clamp(p.padding, 0.0, std::min(availableW, availableH) / 4.0);
        const double innerW     = availableW - 2 * pad;
        const double innerH     = availableH - 2 * pad;
        const double titleH     = std::clamp(p.titleHeight, 0.0, innerH / 4.0);
        const double gap        = std::clamp(p.gap, 0.0, std::min(innerW, innerH) / 4.0);

        double footerH = 0.0;
        int    rows    = 1;
        for (int pass = 0; pass < 2; ++pass) {
            const double gridH = innerH - titleH - footerH;
            out.iconSize       = std::min(std::max(1.0, p.iconSize), std::min(innerW, gridH));
            const double cell  = out.iconSize + gap;
            if (!std::isfinite(cell) || cell <= 0 || !std::isfinite(innerW + gap) || !std::isfinite(gridH + gap))
                return {};
            out.columns          = std::max(1, static_cast<int>(std::min(static_cast<double>(count), std::floor((innerW + gap) / cell))));
            const int neededRows = (count - 1) / out.columns + 1;
            rows                 = std::max(1, static_cast<int>(std::min(static_cast<double>(neededRows), std::floor((gridH + gap) / cell))));
            if (rows == neededRows || pass == 1)
                break;
            footerH = std::min(26.0, innerH / 6.0);
        }

        out.gap                 = gap;
        out.capacity            = static_cast<int>(std::min<int64_t>(count, static_cast<int64_t>(out.columns) * rows));
        out.page                = std::clamp(selected, 0, count - 1) / out.capacity;
        out.pages               = (count - 1) / out.capacity + 1;
        out.first               = out.page * out.capacity;
        const int    visible    = std::min(out.capacity, count - out.first);
        const double gridW      = out.columns * out.iconSize + (out.columns - 1) * gap;
        const double gridH      = rows * out.iconSize + (rows - 1) * gap;
        const double contentW   = std::max(gridW, std::clamp(p.titleWidth, 0.0, std::min(innerW, p.screenW * 0.66)));
        const double panelW     = contentW + 2 * pad;
        const double panelH     = gridH + titleH + footerH + 2 * pad;
        out.panel               = {(p.screenW - panelW) / 2, (p.screenH - panelH) / 2, panelW, panelH};
        const double titleInset = std::min(4.0, titleH);
        out.title               = {out.panel.x + pad, out.panel.y + pad + gridH + titleInset, contentW, titleH - titleInset};
        out.pageLabel           = {out.panel.x + pad, out.title.y + out.title.h, contentW, footerH};

        out.tiles.reserve(visible);
        for (int i = 0; i < visible; ++i) {
            const int    row   = i / out.columns;
            const int    col   = i % out.columns;
            const int    inRow = std::min(out.columns, visible - row * out.columns);
            const double rowW  = inRow * out.iconSize + (inRow - 1) * gap;
            out.tiles.push_back(STile{
                .key   = static_cast<size_t>(out.first + i),
                .box   = {out.panel.x + (panelW - rowW) / 2 + col * (out.iconSize + gap), out.panel.y + pad + row * (out.iconSize + gap), out.iconSize, out.iconSize},
                .row   = row,
                .col   = col,
                .order = static_cast<size_t>(out.first + i),
            });
        }

        return out;
    }

    // Keep vertical navigation aligned with centred, partially filled rows.
    inline int switcherRowStep(int count, int current, int columns, int direction) {
        if (count <= 0)
            return -1;
        current = std::clamp(current, 0, count - 1);
        if (columns <= 0 || direction == 0)
            return current;

        const int    row          = current / columns;
        const int    targetRow    = std::clamp(row + (direction > 0 ? 1 : -1), 0, (count - 1) / columns);
        const int    sourceCount  = std::min(columns, count - row * columns);
        const int    targetCount  = std::min(columns, count - targetRow * columns);
        const double column       = current % columns + (targetCount - sourceCount) / 2.0;
        const int    targetColumn = std::clamp(static_cast<int>(std::floor(column + 0.5)), 0, targetCount - 1);
        return targetRow * columns + targetColumn;
    }

} // namespace hyprspace
