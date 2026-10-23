#pragma once

#include "../src/WindowViewModel.hpp"
#include "test_search_input.hpp"

static void testWindowViews() {
    testSearchInput();
    section("window views: identities, Unicode search, stable grouping and frozen recency");
    std::vector<SWindowViewInput> input{
        {.key           = 1,
         .monitor       = 0,
         .workspace     = 2,
         .monitorName   = "eDP-1",
         .workspaceName = "2",
         .appId         = "firefox",
         .appName       = "Firefox",
         .appClass      = "firefox",
         .title         = "Æble research",
         .recent        = 2},
        {.key           = 2,
         .monitor       = 1,
         .workspace     = 3,
         .monitorName   = "DP-1",
         .workspaceName = "Research",
         .appId         = "firefox",
         .appName       = "Firefox",
         .appClass      = "Firefox",
         .title         = "Øvelse",
         .recent        = 0},
        {.key           = 3,
         .monitor       = 0,
         .workspace     = 1,
         .monitorName   = "eDP-1",
         .workspaceName = "1",
         .appId         = "ghostty",
         .appName       = "Terminal",
         .appClass      = "com.mitchellh.ghostty",
         .title         = "hyprspace",
         .recent        = 1},
        {.key           = 4,
         .monitor       = 0,
         .workspace     = 2,
         .monitorName   = "eDP-1",
         .workspaceName = "2",
         .appId         = "another-terminal",
         .appName       = "Terminal",
         .appClass      = "kitty",
         .title         = "notes",
         .recent        = 3},
    };
    auto flat = groupWindowViews(input, EWindowGrouping::FLAT, false, "");
    CHECK(flat.size() == 1 && flat[0].windows == std::vector<size_t>({2, 0, 3, 1}));
    auto recent = groupWindowViews(input, EWindowGrouping::FLAT, true, "");
    CHECK(recent[0].windows == std::vector<size_t>({1, 2, 0, 3}));
    auto apps = groupWindowViews(input, EWindowGrouping::APP, false, "");
    CHECK(apps.size() == 3); // Equal human names must not merge unrelated apps.
    CHECK(apps[0].key == "firefox" && apps[0].windows.size() == 2);
    CHECK(groupWindowViews(input, EWindowGrouping::WORKSPACE, false, "").size() == 3);
    auto monitors = groupWindowViews(input, EWindowGrouping::MONITOR, false, "");
    CHECK(monitors.size() == 2 && monitors[0].windows.size() == 3);
    auto matched = groupWindowViews(input, EWindowGrouping::FLAT, false, "FIREFOX æBLE 2");
    CHECK(matched.size() == 1 && matched[0].windows == std::vector<size_t>({0}));
    matched = groupWindowViews(input, EWindowGrouping::FLAT, false, "øVELSE dp-1");
    CHECK(matched.size() == 1 && matched[0].windows == std::vector<size_t>({1}));
    CHECK(groupWindowViews(input, EWindowGrouping::FLAT, false, "definitely missing").empty());
    CHECK(groupWindowViews(input, EWindowGrouping::FLAT, false, "\xff").empty());
    // Blank searches take the unfiltered path, including Unicode whitespace,
    // without changing either group order or frozen recency order.
    for (const auto grouping : {EWindowGrouping::FLAT, EWindowGrouping::APP, EWindowGrouping::WORKSPACE, EWindowGrouping::MONITOR}) {
        for (bool useRecent : {false, true}) {
            const auto unfiltered = groupWindowViews(input, grouping, useRecent, "");
            const auto whitespace = groupWindowViews(input, grouping, useRecent, " \t\n　");
            CHECK(unfiltered.size() == whitespace.size());
            for (size_t i = 0; i < std::min(unfiltered.size(), whitespace.size()); ++i) {
                CHECK(unfiltered[i].key == whitespace[i].key);
                CHECK(unfiltered[i].label == whitespace[i].label);
                CHECK(unfiltered[i].windows == whitespace[i].windows);
            }
        }
    }
    input[0].appId.clear();
    input[1].appId.clear();
    CHECK(groupWindowViews(input, EWindowGrouping::APP, false, "").size() == 4);
    std::string query = "æøå";
    windowSearchBackspace(query);
    CHECK(query == "æø");
    windowSearchBackspace(query);
    CHECK(query == "æ");
    windowSearchBackspace(query);
    windowSearchBackspace(query);
    CHECK(query.empty());

    section("window views: Unicode app-label ordering and distinct identities");
    input = {{.key = 1, .appId = "z", .appName = "Straße", .recent = 3},
             {.key = 2, .appId = "a", .appName = "STRASSE", .recent = 2},
             {.key = 3, .appId = "m", .appName = "alpha", .recent = 1},
             {.key = 4, .appId = "z", .appName = "Straße", .recent = 0}};
    apps = groupWindowViews(input, EWindowGrouping::APP, false, "");
    CHECK(apps.size() == 3);
    CHECK(apps[0].key == "m" && apps[1].key == "a" && apps[2].key == "z");
    CHECK(apps[2].windows == std::vector<size_t>({0, 3}));
    apps = groupWindowViews(input, EWindowGrouping::APP, true, "");
    CHECK(apps.size() == 3);
    CHECK(apps[0].key == "z" && apps[1].key == "m" && apps[2].key == "a");
    CHECK(apps[0].windows == std::vector<size_t>({3, 0}));
    CHECK(groupWindowViews({}, EWindowGrouping::FLAT, false, "").empty());
    CHECK(groupWindowViews(input, EWindowGrouping::FLAT, false, "missing").empty());

    input.clear();
    for (size_t i = 0; i < 6; ++i)
        input.push_back({.key = i + 1, .appId = std::to_string(i / 2), .appName = "App " + std::to_string(i / 2)});
    const auto sections = layoutWindowViews(input, groupWindowViews(input, EWindowGrouping::APP, false, ""), {56, 188, 1808, 802}, 28);
    CHECK(sections.headings.size() == 3 && sections.cards.size() == 6);
    CHECK(sections.headings[0].box.x < sections.headings[1].box.x && sections.headings[1].box.x < sections.headings[2].box.x);
    CHECK(sections.cards[0].box.w > 480 && sections.cards[0].box.w < 640);
    CHECK(sections.height <= 802 + 1e-7);

    section("window views: readable cards, scrolling, portrait geometry and malformed bounds");
    for (const SBoxF viewport : {SBoxF{56, 188, 1488, 622}, SBoxF{20, 140, 560, 780}, SBoxF{-900, -500, 280, 170}}) {
        for (size_t count : {1, 3, 12, 36, 128, 256}) {
            input.clear();
            for (size_t i = 0; i < count; ++i)
                input.push_back({.key       = i + 1,
                                 .monitor   = static_cast<int64_t>(i % 3),
                                 .workspace = static_cast<int64_t>(i % 7),
                                 .appId     = std::to_string(i % 4),
                                 .appName   = "App " + std::to_string(i % 4)});
            for (auto mode : {EWindowGrouping::FLAT, EWindowGrouping::APP, EWindowGrouping::WORKSPACE, EWindowGrouping::MONITOR}) {
                const auto result = layoutWindowViews(input, groupWindowViews(input, mode, false, ""), viewport, 20);
                CHECK(result.cards.size() == count);
                CHECK(std::isfinite(result.height) && result.height > 0);
                for (size_t i = 0; i < result.cards.size(); ++i) {
                    const auto& a = result.cards[i].box;
                    CHECK(a.w >= 220 && a.h > 52);
                    CHECK(a.x >= viewport.x - 1e-7 && a.x + a.w <= viewport.x + viewport.w + 1e-7);
                    for (size_t j = i + 1; j < result.cards.size(); ++j) {
                        const auto& b = result.cards[j].box;
                        CHECK(a.x + a.w <= b.x + 1e-7 || b.x + b.w <= a.x + 1e-7 || a.y + a.h <= b.y + 1e-7 || b.y + b.h <= a.y + 1e-7);
                    }
                }
                if (count >= 36)
                    CHECK(result.height > viewport.h);
            }
        }
    }
    const auto groups = groupWindowViews(input, EWindowGrouping::FLAT, false, "");
    CHECK(layoutWindowViews(input, groups, {}, 20).cards.empty());
    CHECK(layoutWindowViews(input, groups, {0, 0, 500, 500}, std::numeric_limits<double>::quiet_NaN()).cards.empty());
    CHECK(layoutWindowViews(input, groups, {0, 0, 500, 500}, 20, 0).cards.empty());
    CHECK(layoutWindowViews(input, groups, {0, 0, std::numeric_limits<double>::infinity(), 500}, 20).cards.empty());
}
