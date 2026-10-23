#pragma once

#include "../src/HelpModel.hpp"
#include <ranges>

static void testHelp() {
    section("help: global token search, ranking, Unicode, topics and stable selection");
    std::vector<SHelpEntry> entries{
        {"zoom", "Inspect", "Hold Z", "Magnify the workspace", "zoom inspect", "overview:zoom_key", 2},
        {"follow", "Follow the cursor", "Z + F", "Follow is enabled and persists across restarts", "mouse waybar corners", "overview:follow_key", 2},
        {"resize", "Resize a window", "Super + right-drag", "Resize the selected window", "resize drag", "", 4},
        {"bindings", "Configure keybindings", "", "Remap your keys", "keybinds shortcuts", "overview:*_key", 5},
        {"unicode", "Øvelse", "U", "A Unicode help entry", "", "", 1},
        {"hint", "Hide the hint", "Hidden", "Hide the follow hint", "hint", "overview:show_follow_hint", 5},
    };
    CHelpBrowser browser;
    CHECK(browser.selection() == nullptr);
    browser.move(-100);
    browser.edge(true);
    browser.setEntries(entries);
    CHECK(browser.count() == 6 && browser.selection()->id == "zoom");
    browser.setTopic(4);
    CHECK(browser.count() == 1 && browser.selection()->id == "resize");
    browser.setQuery("CURSOR enabled");
    CHECK(browser.count() == 1 && browser.selection()->id == "follow");
    for (const auto* separator : {"\t", "\u00a0", "\u2003", "\u3000"}) {
        browser.setQuery(std::string("CURSOR") + separator + "enabled");
        CHECK(browser.count() == 1 && browser.selection()->id == "follow");
    }
    browser.setQuery("\u00a0\u2003\u3000");
    CHECK(browser.count() == 1 && browser.selection()->id == "resize");
    browser.setQuery("keybinds");
    CHECK(browser.count() == 5);
    CHECK(browser.selection()->id == "bindings");
    CHECK(std::ranges::none_of(std::views::iota(size_t{0}, browser.count()), [&](size_t row) { return browser.entry(row).id == "hint"; }));
    browser.setQuery("follow");
    CHECK(browser.selection()->id == "follow");
    browser.setQuery("øVELSE");
    CHECK(browser.count() == 1 && browser.selection()->id == "unicode");
    browser.setQuery("nothing-matches");
    CHECK(browser.count() == 0 && browser.selection() == nullptr);
    browser.move(100);
    browser.setTopic(0);
    browser.edge(true);
    CHECK(browser.selection()->id == "hint");
    browser.move(100);
    CHECK(browser.selection()->id == "hint");
    browser.move(-100);
    CHECK(browser.selected() == 0);
    browser.select(1);
    entries[1].keys = "X + C";
    browser.setEntries(entries);
    CHECK(browser.selection()->id == "follow" && browser.selection()->keys == "X + C");
    browser.setQuery("cursor enabled");
    entries[1].description = "Follow is disabled";
    browser.setEntries(entries);
    CHECK(browser.count() == 0);
    browser.setQuery("  cursor   disabled  ");
    CHECK(browser.count() == 1 && browser.selection()->id == "follow");

    section("help: cached search ranks remain stable and refresh after catalog edits");
    CHelpBrowser            ranked;
    std::vector<SHelpEntry> ranking{
        {"details", "An unrelated tool", "", "Inspect its contents", "", "", 1},
        {"tags", "Another tool", "", "", "inspect", "", 1},
        {"inside", "How to inspect a window", "", "", "", "", 1},
        {"prefix", "Inspect further", "", "", "", "", 1},
        {"first", "Inspect", "", "", "", "", 1},
        {"second", "INSPECT", "", "", "", "", 1},
    };
    ranked.setEntries(ranking);
    ranked.setQuery("inspect");
    const std::array expected{"first", "second", "prefix", "inside", "tags", "details"};
    CHECK(ranked.count() == expected.size());
    for (size_t row = 0; row < std::min(ranked.count(), expected.size()); ++row)
        CHECK(ranked.entry(row).id == expected[row]);
    ranked.select(4);
    ranking[1].title = "Inspect";
    ranking[1].tags.clear();
    ranked.setEntries(ranking);
    CHECK(ranked.count() == 6 && ranked.entry(0).id == "tags");
    CHECK(ranked.selection() && ranked.selection()->id == "tags" && ranked.selected() == 0);
    ranking[1].title = "Unrelated";
    ranked.setEntries(ranking);
    CHECK(ranked.count() == 5 && ranked.selection()->id == "first");
    ranked.setEntries({});
    CHECK(ranked.count() == 0 && ranked.selection() == nullptr);

    section("help: responsive geometry respects reserved space and keeps list/detail separate");
    const auto inside = [](const SBoxF& box, const SBoxF& bounds) {
        return box.x >= bounds.x && box.y >= bounds.y && box.x + box.w <= bounds.x + bounds.w + .01 && box.y + box.h <= bounds.y + bounds.h + .01;
    };
    for (const auto usable : {SBoxF{0, 112, 1920, 968}, {0, 56, 1600, 844}, {0, 112, 960, 488}, {0, 0, 480, 768}, {0, 0, 640, 400}}) {
        for (double lineHeight : {18.0, 22.0, 28.0}) {
            const auto layout = helpLayout(usable, lineHeight);
            CHECK(inside(layout.panel, usable));
            for (const auto box : {layout.search, layout.close, layout.results, layout.detail, layout.footer})
                CHECK(inside(box, layout.panel));
            CHECK(layout.results.h >= 1 && layout.results.y + layout.results.h <= layout.detail.y);
            CHECK(layout.detail.y + layout.detail.h < layout.footer.y);
            CHECK(layout.search.y + layout.search.h < layout.results.y);
            CHECK((layout.topics.w > 0) == (layout.panel.w >= 720));
        }
    }

    section("help: paragraphs wrap and stay within their raster budget at fractional scales");
    const std::string paragraph = "Follow the cursor to inspect a magnified workspace. Your choice survives restarts. Æøå and Unicode stay readable.";
    for (double scale : {1.0, 1.2, 1.25, 1.5, 2.0}) {
        const auto wide     = renderParagraph(paragraph, "Sans 12", {}, 500, 160, scale);
        const auto narrow   = renderParagraph(paragraph, "Sans 12", {}, 180, 160, scale);
        const auto shortBox = renderParagraph(paragraph, "Sans 12", {}, 180, 25, scale);
        CHECK(wide.ok() && narrow.ok() && shortBox.ok());
        CHECK(narrow.h > wide.h);
        CHECK(narrow.w <= static_cast<int>(180 * scale));
        CHECK(shortBox.h <= static_cast<int>(25 * scale));
    }
    CHECK(!renderParagraph(paragraph, "Sans 12", {}, 0, 100).ok());
    CHECK(!renderParagraph(paragraph, "Sans 12", {}, 180, 0).ok());
}
