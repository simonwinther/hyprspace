#pragma once

#include "SpaceAppearance.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace hyprspace::spaces {
    // Catalog words and aliases are English. Matching folds ASCII without a
    // locale and requires every whitespace-separated query term to match.
    inline std::vector<const SSpaceIcon*> filterSpaceIcons(std::string_view query) {
        const auto lower = [](std::string_view value) {
            std::string result(value);
            for (auto& c : result)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return result;
        };
        const auto                     needle = lower(query);
        std::vector<const SSpaceIcon*> result;
        for (const auto& icon : SPACE_ICON_CATALOG) {
            const auto haystack = lower(std::string(icon.id) + " " + std::string(icon.label) + " " + std::string(icon.keywords));
            bool       matches  = true;
            for (size_t from = 0; from < needle.size();) {
                while (from < needle.size() && std::isspace(static_cast<unsigned char>(needle[from])))
                    ++from;
                auto to = from;
                while (to < needle.size() && !std::isspace(static_cast<unsigned char>(needle[to])))
                    ++to;
                if (to > from && haystack.find(needle.substr(from, to - from)) == std::string::npos) {
                    matches = false;
                    break;
                }
                from = to;
            }
            if (matches)
                result.push_back(&icon);
        }
        return result;
    }

    struct SSpaceIconViewport {
        size_t first = 0, count = 0, columns = 1, rows = 1;
        bool   previous = false, next = false;
    };

    inline SSpaceIconViewport spaceIconViewport(size_t total, size_t selected, size_t columns, size_t rows) {
        columns                = std::max<size_t>(1, columns);
        rows                   = std::max<size_t>(1, rows);
        const size_t totalRows = total / columns + (total % columns != 0);
        const size_t chosenRow = total ? std::min(selected, total - 1) / columns : 0;
        const size_t firstRow  = std::min(chosenRow > rows / 2 ? chosenRow - rows / 2 : 0, totalRows > rows ? totalRows - rows : 0);
        const size_t first     = firstRow * columns;
        const size_t count     = std::min(total - first, rows * columns);
        return {first, count, columns, rows, first > 0, first + count < total};
    }
} // namespace hyprspace::spaces
