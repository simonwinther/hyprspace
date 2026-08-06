// hyprspace - pure geometry / layout math.
//
// This header deliberately has NO Hyprland dependencies so that it can be
// compiled and unit-tested on the host (see test/test_hyprspace.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>

namespace hyprspace {

    struct SBoxF {
        double x = 0, y = 0, w = 0, h = 0;

        double cx() const {
            return x + w / 2.0;
        }
        double cy() const {
            return y + h / 2.0;
        }
        bool contains(double px, double py) const {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    // One workspace as far as the layout engine is concerned.
    struct STileInput {
        size_t key         = 0; // caller's opaque handle
        long   workspaceId = 0;
    };

    struct STile {
        size_t key   = 0;
        SBoxF  box   = {};
        int    row   = 0;
        int    col   = 0;
        size_t order = 0; // linear (tab) order
    };

    struct SLayoutParams {
        double screenW = 1920;
        double screenH = 1080;
        double padding = 56; // outer padding
        double gap     = 28; // gap between cells
        double aspect  = 16.0 / 9.0;
    };

    struct SLayoutResult {
        std::vector<STile> tiles;
        int                rows = 0;
        int                cols = 0;
    };

    // Lay out `n` equally sized cells, every one at the monitor's aspect ratio,
    // filling as much of the screen as possible.
    //
    // Every column count is tried and the one producing the largest cell wins,
    // so the grid stays close to square: 5 workspaces become 3+2, 10 become 4+3+3
    // (or 5+5, whichever is larger), and a single workspace fills the screen.
    inline SLayoutResult layout(const std::vector<STileInput>& input, const SLayoutParams& p) {
        SLayoutResult out;

        const size_t n = input.size();
        if (n == 0)
            return out;

        const double usableW = p.screenW - 2 * p.padding;
        const double usableH = p.screenH - 2 * p.padding;
        const double aspect  = p.aspect > 0.01 ? p.aspect : 16.0 / 9.0;

        if (usableW <= 0 || usableH <= 0)
            return out;

        size_t bestCols = 1;
        double bestW    = -1.0;

        for (size_t cols = 1; cols <= n; ++cols) {
            const size_t rows = (n + cols - 1) / cols;

            // Width-limited and height-limited candidates; the cell must satisfy
            // both, so take the smaller.
            const double byW = (usableW - p.gap * static_cast<double>(cols - 1)) / static_cast<double>(cols);
            const double byH = ((usableH - p.gap * static_cast<double>(rows - 1)) / static_cast<double>(rows)) * aspect;

            const double cellW = std::min(byW, byH);
            if (cellW > bestW) {
                bestW    = cellW;
                bestCols = cols;
            }
        }

        if (bestW <= 0)
            return out;

        const size_t cols   = bestCols;
        const size_t rows   = (n + cols - 1) / cols;
        const double cellW  = bestW;
        const double cellH  = cellW / aspect;
        const double gridH  = cellH * static_cast<double>(rows) + p.gap * static_cast<double>(rows - 1);
        const double startY = p.padding + (usableH - gridH) / 2.0;

        out.rows = static_cast<int>(rows);
        out.cols = static_cast<int>(cols);

        for (size_t i = 0; i < n; ++i) {
            const size_t row     = i / cols;
            const size_t col     = i % cols;
            const size_t inRow   = std::min(cols, n - row * cols);
            const double rowW    = cellW * static_cast<double>(inRow) + p.gap * static_cast<double>(inRow - 1);
            const double startX  = p.padding + (usableW - rowW) / 2.0; // last row stays centred

            STile tile;
            tile.key   = input[i].key;
            tile.box   = SBoxF{startX + static_cast<double>(col) * (cellW + p.gap), startY + static_cast<double>(row) * (cellH + p.gap), cellW, cellH};
            tile.row   = static_cast<int>(row);
            tile.col   = static_cast<int>(col);
            tile.order = i;
            out.tiles.push_back(tile);
        }

        return out;
    }

    // ---- keyboard navigation over a finished layout -------------------------

    enum class EDirection { LEFT, RIGHT, UP, DOWN };

    // Directional move: pick the closest tile in the requested direction, scoring
    // primarily on the axis of travel and secondarily on perpendicular offset.
    inline int navigate(const std::vector<STile>& tiles, int current, EDirection dir) {
        if (tiles.empty())
            return -1;
        if (current < 0 || current >= static_cast<int>(tiles.size()))
            return 0;

        const auto& from = tiles[current].box;
        int         best = -1;
        double      bestScore = 0;

        for (size_t i = 0; i < tiles.size(); ++i) {
            if (static_cast<int>(i) == current)
                continue;

            const auto&  to = tiles[i].box;
            const double dx = to.cx() - from.cx();
            const double dy = to.cy() - from.cy();

            double along = 0, perp = 0;
            switch (dir) {
                case EDirection::LEFT:
                    along = -dx;
                    perp  = std::abs(dy);
                    break;
                case EDirection::RIGHT:
                    along = dx;
                    perp  = std::abs(dy);
                    break;
                case EDirection::UP:
                    along = -dy;
                    perp  = std::abs(dx);
                    break;
                case EDirection::DOWN:
                    along = dy;
                    perp  = std::abs(dx);
                    break;
            }

            if (along <= 1.0) // must actually move that way
                continue;

            // Prefer near in the direction of travel, heavily penalise drifting sideways.
            const double score = along + perp * 2.0;
            if (best == -1 || score < bestScore) {
                best      = static_cast<int>(i);
                bestScore = score;
            }
        }

        return best == -1 ? current : best;
    }

    inline int tileAt(const std::vector<STile>& tiles, double x, double y) {
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i].box.contains(x, y))
                return static_cast<int>(i);
        }
        return -1;
    }

} // namespace hyprspace
