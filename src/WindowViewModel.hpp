#pragma once

#include "Geometry.hpp"

#include <glib.h>

#include <span>
#include <tuple>
#include <limits>
#include <utility>

namespace hyprspace {

    enum class EWindowGrouping { FLAT, APP, WORKSPACE, MONITOR };

    inline const char* windowGroupingName(EWindowGrouping grouping) {
        switch (grouping) {
        case EWindowGrouping::APP:
            return "app";
        case EWindowGrouping::WORKSPACE:
            return "workspace";
        case EWindowGrouping::MONITOR:
            return "monitor";
        default:
            return "flat";
        }
    }

    inline std::string foldWindowSearch(const std::string& text) {
        auto*       valid  = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
        auto*       folded = g_utf8_casefold(valid, -1);
        std::string result{folded};
        g_free(folded);
        g_free(valid);
        return result;
    }

    inline void windowSearchBackspace(std::string& text) {
        if (!text.empty()) {
            const auto* previous = g_utf8_find_prev_char(text.data(), text.data() + text.size());
            text.resize(previous ? static_cast<size_t>(previous - text.data()) : 0);
        }
    }

    struct SWindowViewInput {
        uint64_t    key     = 0;
        int64_t     monitor = 0, workspace = 0;
        std::string monitorName, workspaceName, appId, appName, appClass, title;
        size_t      recent                                    = std::numeric_limits<size_t>::max();
        bool        operator==(const SWindowViewInput&) const = default;
    };

    struct SWindowViewGroup {
        std::string         key, label;
        std::vector<size_t> windows;
    };

    inline std::vector<SWindowViewGroup> groupWindowViews(std::span<const SWindowViewInput> input, EWindowGrouping grouping, bool recent, const std::string& query) {
        std::vector<std::string> terms;
        const auto               folded = foldWindowSearch(query);
        std::string              term;
        for (const char* p = folded.c_str(); *p; p = g_utf8_next_char(p)) {
            if (g_unichar_isspace(g_utf8_get_char(p))) {
                if (!term.empty())
                    terms.push_back(std::exchange(term, {}));
            } else
                term.append(p, g_utf8_next_char(p) - p);
        }
        if (!term.empty())
            terms.push_back(term);

        auto                location = [&](size_t i) { return std::tuple{input[i].monitor, input[i].workspace, input[i].key}; };
        std::vector<size_t> ordered;
        for (size_t i = 0; i < input.size(); ++i) {
            const auto& window   = input[i];
            const auto  haystack = foldWindowSearch(window.title + " " + window.appName + " " + window.appClass + " " + window.workspaceName + " " +
                                                    std::to_string(window.workspace) + " " + window.monitorName);
            if (std::ranges::all_of(terms, [&](const auto& word) { return haystack.find(word) != std::string::npos; }))
                ordered.push_back(i);
        }
        std::ranges::sort(ordered, [&](size_t a, size_t b) {
            if (recent && input[a].recent != input[b].recent)
                return input[a].recent < input[b].recent;
            return location(a) < location(b);
        });

        std::vector<SWindowViewGroup> result;
        for (size_t i : ordered) {
            const auto& window = input[i];
            std::string key, label;
            switch (grouping) {
            case EWindowGrouping::APP:
                key   = window.appId.empty() ? "unknown:" + std::to_string(window.key) : window.appId;
                label = window.appName.empty() ? (window.appClass.empty() ? "Unknown app" : window.appClass) : window.appName;
                break;
            case EWindowGrouping::WORKSPACE:
                key   = std::to_string(window.monitor) + ":" + std::to_string(window.workspace);
                label = workspaceLabel(window.workspace, window.workspaceName) + " · " + window.monitorName;
                break;
            case EWindowGrouping::MONITOR:
                key   = std::to_string(window.monitor);
                label = window.monitorName;
                break;
            default:
                break;
            }
            auto found = std::ranges::find(result, key, &SWindowViewGroup::key);
            if (found == result.end()) {
                result.push_back({key, label, {}});
                found = result.end() - 1;
            }
            found->windows.push_back(i);
        }
        std::ranges::sort(result, [&](const auto& a, const auto& b) {
            if (recent && input[a.windows.front()].recent != input[b.windows.front()].recent)
                return input[a.windows.front()].recent < input[b.windows.front()].recent;
            if (grouping == EWindowGrouping::APP)
                return std::tuple{foldWindowSearch(a.label), a.key} < std::tuple{foldWindowSearch(b.label), b.key};
            return location(a.windows.front()) < location(b.windows.front());
        });
        return result;
    }

    struct SWindowViewCard {
        uint64_t key;
        SBoxF    box;
    };
    struct SWindowViewHeading {
        std::string label;
        size_t      count;
        SBoxF       box;
    };
    struct SWindowViewLayout {
        std::vector<SWindowViewCard>    cards;
        std::vector<SWindowViewHeading> headings;
        double                          height = 0;
    };

    // Fixed card proportions keep scanning and navigation predictable. Each
    // window is fitted independently inside its preview, preserving its aspect.
    inline SWindowViewLayout layoutWindowViews(std::span<const SWindowViewInput> input, const std::vector<SWindowViewGroup>& groups, const SBoxF& viewport, double gap,
                                               double minimumWidth = 220) {
        SWindowViewLayout result;
        if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.w) || !std::isfinite(viewport.h) || viewport.w <= 0 || viewport.h <= 0 ||
            !std::isfinite(gap) || !std::isfinite(minimumWidth) || minimumWidth <= 0)
            return result;
        gap          = std::clamp(gap, 0.0, 80.0);
        size_t count = 0;
        for (const auto& group : groups)
            count += group.windows.size();
        if (!count)
            return result;
        // On wide outputs, give groups their own columns. A handful of small
        // groups should use the available width rather than squeezing into a
        // narrow vertical strip just to fit every heading on screen.
        const size_t sectionColumns =
            std::min<size_t>(groups.size(), static_cast<size_t>(std::clamp(std::floor((viewport.w + gap) / (minimumWidth * 1.7 + gap)), 1.0, 3.0)));
        if (sectionColumns > 1) {
            const double        sectionWidth = (viewport.w - gap * (sectionColumns - 1)) / sectionColumns;
            std::vector<double> heights(sectionColumns, 0);
            for (const auto& group : groups) {
                const size_t column = std::ranges::min_element(heights) - heights.begin();
                const SBoxF  section{viewport.x + column * (sectionWidth + gap), viewport.y + heights[column], sectionWidth, viewport.h};
                auto         layout = layoutWindowViews(input, {group}, section, gap, minimumWidth);
                result.cards.insert(result.cards.end(), layout.cards.begin(), layout.cards.end());
                result.headings.insert(result.headings.end(), layout.headings.begin(), layout.headings.end());
                heights[column] += layout.height + gap;
            }
            result.height = *std::ranges::max_element(heights) - gap;
            return result;
        }
        constexpr double CAPTION = 52, ASPECT = 1.6, HEADING = 34;
        size_t           columns = 1;
        double           width   = 0;
        // Fit all cards when that gives readable previews. Otherwise retain a
        // minimum width and let the board scroll instead of shrinking forever.
        for (size_t candidate = 1; candidate <= std::min<size_t>(count, 64); ++candidate) {
            size_t rows  = 0;
            double fixed = gap * (groups.size() - 1);
            for (const auto& group : groups) {
                const size_t groupRows = (group.windows.size() + candidate - 1) / candidate;
                rows += groupRows;
                fixed += (group.label.empty() ? 0 : HEADING) + gap * (groupRows - 1);
            }
            const double allowed = std::min({640.0, (viewport.w - gap * (candidate - 1)) / candidate, ((viewport.h - fixed) / rows - CAPTION) * ASPECT});
            if (allowed >= minimumWidth && allowed > width) {
                width   = allowed;
                columns = candidate;
            }
        }
        if (width <= 0) {
            columns = std::max<size_t>(1, static_cast<size_t>(std::min<double>(count, std::floor((viewport.w + gap) / (minimumWidth + gap)))));
            width   = std::max(1.0, (viewport.w - gap * (columns - 1)) / columns);
        }
        const double cardHeight = width / ASPECT + CAPTION;
        const double left       = viewport.x + (viewport.w - (columns * width + gap * (columns - 1))) / 2;
        double       y          = viewport.y;
        for (const auto& group : groups) {
            if (!group.label.empty()) {
                result.headings.push_back({group.label, group.windows.size(), {left, y, columns * width + gap * (columns - 1), HEADING}});
                y += HEADING;
            }
            for (size_t i = 0; i < group.windows.size(); ++i) {
                const auto& window = input[group.windows[i]];
                result.cards.push_back({window.key, {left + (i % columns) * (width + gap), y + (i / columns) * (cardHeight + gap), width, cardHeight}});
            }
            y += ((group.windows.size() + columns - 1) / columns) * (cardHeight + gap);
        }
        result.height = y - viewport.y - gap;
        return result;
    }

} // namespace hyprspace
