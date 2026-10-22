#pragma once

#include "../src/SpaceAppearance.hpp"
#include "../src/SpaceIcons.hpp"
#include "../src/SpaceIconPicker.hpp"

#include <set>

static void testSpaceIcons() {
    section("space icons: keyword search, multi-term matching and bounded row navigation");
    using namespace hyprspace::spaces;
    CHECK(filterSpaceIcons("").size() == SPACE_ICON_CATALOG.size());
    CHECK(filterSpaceIcons("  \t ").size() == SPACE_ICON_CATALOG.size());
    CHECK(filterSpaceIcons("SCHOOL").size() == 1 && filterSpaceIcons("SCHOOL").front()->id == "graduation-cap");
    CHECK(filterSpaceIcons("gaming").size() == 1 && filterSpaceIcons("gaming").front()->id == "gamepad-2");
    CHECK(filterSpaceIcons("space stars").size() == 1 && filterSpaceIcons("space stars").front()->id == "telescope");
    CHECK(filterSpaceIcons("foo no such icon").empty());
    CHECK(filterSpaceIcons("../icon.svg").empty());
    for (const size_t columns : {1, 3, 6})
        for (const size_t rows : {1, 2, 4})
            for (size_t selected = 0; selected < SPACE_ICON_CATALOG.size(); ++selected) {
                const auto viewport = spaceIconViewport(SPACE_ICON_CATALOG.size(), selected, columns, rows);
                CHECK(viewport.first % columns == 0);
                CHECK(viewport.count <= columns * rows);
                CHECK(viewport.first <= selected && selected < viewport.first + viewport.count);
                CHECK(viewport.first + viewport.count <= SPACE_ICON_CATALOG.size());
                CHECK(viewport.previous == (viewport.first > 0));
                CHECK(viewport.next == (viewport.first + viewport.count < SPACE_ICON_CATALOG.size()));
            }
    const auto emptyViewport = spaceIconViewport(0, 0, 0, 0);
    CHECK(emptyViewport.count == 0 && emptyViewport.first == 0 && !emptyViewport.previous && !emptyViewport.next);
    const auto lastViewport = spaceIconViewport(47, 999, 6, 4);
    CHECK(lastViewport.first <= 46 && lastViewport.first + lastViewport.count == 47);

    section("space icons: every bundled symbol renders distinctly at native and fractional output sizes");
    std::set<uint64_t> fingerprints;
    for (const auto& icon : hyprspace::spaces::SPACE_ICON_CATALOG) {
        for (const int size : {16, 24, 30, 36, 48}) {
            const auto image = renderSpaceIcon(icon.id, {1, 1, 1, 1}, size);
            CHECK(image.ok());
            CHECK(image.w == size && image.h == size);
            if (!image.ok())
                continue;
            size_t   visible       = 0;
            uint64_t fingerprint   = 14695981039346656037ULL;
            bool     premultiplied = true;
            for (size_t offset = 0; offset < image.data.size(); offset += 4) {
                const auto alpha = image.data[offset + 3];
                visible += alpha != 0;
                for (size_t channel = 0; channel < 3; ++channel)
                    premultiplied &= image.data[offset + channel] <= alpha;
            }
            for (const auto byte : image.data)
                fingerprint = (fingerprint ^ byte) * 1099511628211ULL;
            CHECK(visible > 8);
            CHECK(premultiplied);
            if (size == 24)
                CHECK(fingerprints.insert(fingerprint).second);
        }
    }
    CHECK(fingerprints.size() == hyprspace::spaces::SPACE_ICON_CATALOG.size());

    section("space icons: tint, group opacity and transparency preserve premultiplied pixels");
    for (const auto& icon : hyprspace::spaces::SPACE_ICON_CATALOG) {
        const auto red   = renderSpaceIcon(icon.id, {1, 0, 0, 1}, 24);
        const auto blue  = renderSpaceIcon(icon.id, {0, 0, 1, 1}, 24);
        const auto half  = renderSpaceIcon(icon.id, {1, 0, 0, .5}, 24);
        const auto clear = renderSpaceIcon(icon.id, {1, 0, 0, 0}, 24);
        CHECK(red.ok() && blue.ok() && half.ok() && clear.ok());
        if (!red.ok() || !blue.ok() || !half.ok() || !clear.ok())
            continue;
        CHECK(red.data != blue.data);
        bool    tinted = true, boundedAlpha = true, transparent = true;
        uint8_t maxHalf = 0;
        for (size_t offset = 0; offset < red.data.size(); offset += 4) {
            tinted &= red.data[offset] == 0 && red.data[offset + 1] == 0 && red.data[offset + 2] == red.data[offset + 3];
            tinted &= blue.data[offset] == blue.data[offset + 3] && blue.data[offset + 1] == 0 && blue.data[offset + 2] == 0;
            boundedAlpha &= half.data[offset + 3] <= 128 && half.data[offset + 2] <= half.data[offset + 3];
            maxHalf = std::max(maxHalf, half.data[offset + 3]);
            for (size_t channel = 0; channel < 4; ++channel)
                transparent &= clear.data[offset + channel] == 0;
        }
        CHECK(tinted);
        CHECK(boundedAlpha && maxHalf > 0);
        CHECK(transparent);
    }

    section("space icons: untrusted keys fall back and invalid raster inputs allocate nothing");
    const auto fallback = renderSpaceIcon(hyprspace::spaces::DEFAULT_SPACE_ICON, {1, 1, 1, 1}, 24);
    CHECK(renderSpaceIcon("unknown", {1, 1, 1, 1}, 24).data == fallback.data);
    CHECK(renderSpaceIcon("</svg><image href='external.svg'/>", {1, 1, 1, 1}, 24).data == fallback.data);
    for (const int size : {-1, 0, MAX_SPACE_ICON_SIZE + 1, std::numeric_limits<int>::max()})
        CHECK(!renderSpaceIcon("grid", {1, 1, 1, 1}, size).ok());
    CHECK(!renderSpaceIcon("grid", {std::numeric_limits<double>::quiet_NaN(), 1, 1, 1}, 24).ok());
    CHECK(!renderSpaceIcon("grid", {1, 1, 1, std::numeric_limits<double>::infinity()}, 24).ok());
}
