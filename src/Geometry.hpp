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

    // One window as far as the layout engine is concerned.
    struct STileInput {
        size_t key         = 0;  // caller's opaque handle (index into its own vector)
        double aspect      = 1.0; // w/h of the real window
        long   workspaceId = 0;
    };

    // Result for one window.
    struct STile {
        size_t key      = 0;
        SBoxF  box      = {};
        int    band     = 0; // which workspace band
        int    row      = 0; // visual row inside the band
        int    col      = 0; // visual column inside the row
        size_t order    = 0; // linear (tab) order
    };

    // One workspace band.
    struct SBand {
        long   workspaceId = 0;
        SBoxF  labelBox    = {};
        SBoxF  contentBox  = {};
        size_t firstTile   = 0;
        size_t tileCount   = 0;
    };

    struct SLayoutParams {
        double screenW    = 1920;
        double screenH    = 1080;
        double padding    = 48;  // outer padding
        double gap        = 24;  // gap between tiles
        double bandGap    = 32;  // gap between workspace bands
        double labelGutter = 56; // width of the left gutter holding workspace labels
        bool   showLabels  = true;
    };

    struct SLayoutResult {
        std::vector<STile> tiles;
        std::vector<SBand> bands;
    };

    namespace detail {

        // Lay a set of aspect ratios out in `rows` contiguous rows inside a box of
        // w x h, preserving every aspect ratio exactly. Returns the uniform scale
        // factor (tile height in px) achievable, or 0 if impossible.
        //
        // Every tile in a row shares the same height; the row's width is the sum of
        // height*aspect plus gaps.
        inline double rowScale(const std::vector<double>& aspects, size_t from, size_t count, double w, double gap) {
            if (count == 0)
                return 0.0;

            double aspectSum = 0.0;
            for (size_t i = 0; i < count; ++i)
                aspectSum += std::max(0.05, aspects[from + i]);

            const double gaps = gap * static_cast<double>(count - 1);
            if (aspectSum <= 0.0 || w - gaps <= 0.0)
                return 0.0;

            return (w - gaps) / aspectSum; // height such that the row exactly fills w
        }

        // Split n items into `rows` contiguous groups as evenly as possible.
        inline std::vector<size_t> splitEvenly(size_t n, size_t rows) {
            std::vector<size_t> out(rows, n / rows);
            for (size_t i = 0; i < n % rows; ++i)
                out[i]++;
            return out;
        }

    } // namespace detail

    // Lay out `aspects` inside contentBox, preserving aspect ratios, maximising
    // tile size. Tries every row count and keeps the best.
    //
    // Returns boxes in the same order as the input, plus per-item (row, col).
    inline void layoutBand(const std::vector<double>& aspects, const SBoxF& content, double gap, std::vector<SBoxF>& outBoxes, std::vector<int>& outRow,
                           std::vector<int>& outCol) {
        outBoxes.assign(aspects.size(), SBoxF{});
        outRow.assign(aspects.size(), 0);
        outCol.assign(aspects.size(), 0);

        const size_t n = aspects.size();
        if (n == 0 || content.w <= 0 || content.h <= 0)
            return;

        size_t bestRows  = 1;
        double bestScale = -1.0;

        for (size_t rows = 1; rows <= n; ++rows) {
            const double availH = content.h - gap * static_cast<double>(rows - 1);
            if (availH <= 0)
                break;

            const auto split = detail::splitEvenly(n, rows);

            // The binding constraint is the smallest row scale, further capped by
            // the per-row height budget.
            double scale  = availH / static_cast<double>(rows);
            size_t cursor = 0;
            for (size_t r = 0; r < rows; ++r) {
                const double s = detail::rowScale(aspects, cursor, split[r], content.w, gap);
                scale          = std::min(scale, s);
                cursor += split[r];
            }

            if (scale > bestScale) {
                bestScale = scale;
                bestRows  = rows;
            }
        }

        if (bestScale <= 0.0)
            return;

        // Emit the winning arrangement, centred both ways.
        const auto   split      = detail::splitEvenly(n, bestRows);
        const double tileH      = bestScale;
        const double totalH     = tileH * static_cast<double>(bestRows) + gap * static_cast<double>(bestRows - 1);
        double       y          = content.y + (content.h - totalH) / 2.0;
        size_t       cursor     = 0;

        for (size_t r = 0; r < bestRows; ++r) {
            const size_t count = split[r];

            double rowW = gap * static_cast<double>(count - 1);
            for (size_t i = 0; i < count; ++i)
                rowW += tileH * std::max(0.05, aspects[cursor + i]);

            double x = content.x + (content.w - rowW) / 2.0;

            for (size_t i = 0; i < count; ++i) {
                const double tw            = tileH * std::max(0.05, aspects[cursor + i]);
                outBoxes[cursor + i]       = SBoxF{x, y, tw, tileH};
                outRow[cursor + i]         = static_cast<int>(r);
                outCol[cursor + i]         = static_cast<int>(i);
                x += tw + gap;
            }

            cursor += count;
            y += tileH + gap;
        }
    }

    // Full overview layout: windows grouped into one horizontal band per workspace,
    // bands stacked vertically, each band labelled.
    inline SLayoutResult layout(const std::vector<STileInput>& input, const SLayoutParams& p) {
        SLayoutResult out;
        if (input.empty())
            return out;

        // Stable-group by workspace, preserving the caller's ordering within a group.
        std::vector<long> workspaces;
        for (const auto& t : input) {
            if (std::find(workspaces.begin(), workspaces.end(), t.workspaceId) == workspaces.end())
                workspaces.push_back(t.workspaceId);
        }
        std::sort(workspaces.begin(), workspaces.end());

        const size_t bandCount = workspaces.size();

        // The workspace label lives in a gutter to the left of its band rather
        // than on a line above it: stacking N labels vertically would eat a
        // sizeable share of the screen height, and that height is exactly what
        // limits how large the tiles can be.
        const double gutter = p.showLabels ? p.labelGutter : 0.0;

        const double usableW = p.screenW - 2 * p.padding - gutter;
        const double usableH = p.screenH - 2 * p.padding;
        if (usableW <= 0 || usableH <= 0)
            return out;

        // Group the inputs per band up front; the band heights depend on content.
        std::vector<std::vector<double>> bandAspects(bandCount);
        std::vector<std::vector<size_t>> bandKeys(bandCount);

        for (const auto& t : input) {
            const auto it = std::find(workspaces.begin(), workspaces.end(), t.workspaceId);
            const auto b  = static_cast<size_t>(std::distance(workspaces.begin(), it));
            bandAspects[b].push_back(std::max(0.05, t.aspect));
            bandKeys[b].push_back(t.key);
        }

        // Height available for tiles once the inter-band gaps are removed.
        const double contentH = usableH - p.bandGap * static_cast<double>(bandCount - 1);
        if (contentH <= 0)
            return out;

        // Allocate band heights proportional to sqrt(window count).
        //
        // Sizing a band by the height it needs to fill the width in one row is
        // tempting but backwards: the busiest workspace ends up with the thinnest
        // strip. Proportional-to-count over-corrects the other way and starves
        // single-window bands. The square root sits between the two, so a
        // workspace with several windows gets visibly more room while a workspace
        // with one still gets a usable tile.
        std::vector<double> natural(bandCount, 0.0);
        double              naturalSum = 0.0;

        for (size_t b = 0; b < bandCount; ++b) {
            natural[b] = std::sqrt(static_cast<double>(std::max<size_t>(1, bandAspects[b].size())));
            naturalSum += natural[b];
        }

        if (naturalSum <= 0)
            return out;

        size_t orderCounter = 0;
        double bandY        = p.padding;

        for (size_t b = 0; b < bandCount; ++b) {
            const double bandContentH = natural[b] / naturalSum * contentH;

            SBand band;
            band.workspaceId = workspaces[b];
            band.labelBox    = SBoxF{p.padding, bandY, gutter, bandContentH};
            band.contentBox  = SBoxF{p.padding + gutter, bandY, usableW, bandContentH};
            band.firstTile   = out.tiles.size();

            const std::vector<double>& aspects = bandAspects[b];
            const std::vector<size_t>& keys    = bandKeys[b];

            std::vector<SBoxF> boxes;
            std::vector<int>   rows, cols;
            layoutBand(aspects, band.contentBox, p.gap, boxes, rows, cols);

            for (size_t i = 0; i < keys.size(); ++i) {
                STile tile;
                tile.key   = keys[i];
                tile.box   = boxes[i];
                tile.band  = static_cast<int>(b);
                tile.row   = rows[i];
                tile.col   = cols[i];
                tile.order = orderCounter++;
                out.tiles.push_back(tile);
            }

            band.tileCount = out.tiles.size() - band.firstTile;
            out.bands.push_back(band);

            bandY += bandContentH + p.bandGap;
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
