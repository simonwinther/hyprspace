#pragma once

#include "../src/RenderGeometry.hpp"

#include <limits>

static void testRenderGeometry() {
    using namespace hyprspace::renderGeometry;
    section("render geometry: physical shape endpoints survive native integer damage clipping");
    for (const double scale : {1.0, 1.2, 1.25, 1.5}) {
        for (const SBoxF logical : {SBoxF{31.2, 41.2, 116.72, 56.72}, SBoxF{-.4, -20.2, 100.1, 68.3}, SBoxF{259.2, 94, 676.8, 482}}) {
            const auto box = physicalBox(logical, scale);
            CHECK(box.x == std::floor(box.x) && box.y == std::floor(box.y));
            CHECK(box.w == std::floor(box.w) && box.h == std::floor(box.h));
            CHECK(std::abs(box.x - logical.x * scale) <= .5 + 1e-9);
            CHECK(std::abs(box.y - logical.y * scale) <= .5 + 1e-9);
            CHECK(std::abs(box.x + box.w - (logical.x + logical.w) * scale) <= .5 + 1e-9);
            CHECK(std::abs(box.y + box.h - (logical.y + logical.h) * scale) <= .5 + 1e-9);
            // Pixman truncates the native border region's origin and size
            // independently. Every snapped far edge must remain identical.
            CHECK(static_cast<int>(box.x) + static_cast<int>(box.w) == box.x + box.w);
            CHECK(static_cast<int>(box.y) + static_cast<int>(box.h) == box.y + box.h);
        }
    }
    const auto bounds = physicalBox({31.2, 41.2, 116.72, 56.72}, 1.2);
    CHECK(bounds.x == 37 && bounds.y == 49 && bounds.w == 141 && bounds.h == 69);
    CHECK(physicalRadius(7, 1.2) == 8 && physicalRadius(7, 1.25) == 9);
    CHECK(physicalRadius(7, 1.5) == 11 && physicalRadius(0, 1.5) == 0);
    // Passing a physical width here would make Hyprland scale it twice.
    CHECK(logicalBorderWidth(1) == 1 && logicalBorderWidth(3) == 3);
    CHECK(logicalBorderWidth(.4) == 1 && logicalBorderWidth(0) == 0);
    CHECK(logicalBorderWidth(-1) == 0);

    section("render factories: paired shapes share snapped bounds and one logical border width");
    for (const double scale : {1.0, 1.2, 1.25, 1.5}) {
        const SBoxF logical{-12.4, 41.2, 116.72, 56.72};
        const auto  fill    = physicalShape(logical, scale, 7);
        const auto  outline = physicalShape(logical, scale, 7, 1);
        CHECK(fill && outline);
        CHECK(fill->box.x == outline->box.x && fill->box.y == outline->box.y && fill->box.w == outline->box.w && fill->box.h == outline->box.h);
        CHECK(fill->radius == outline->radius && outline->radius == physicalRadius(7, scale));
        CHECK(fill->borderWidth == 0 && outline->borderWidth == 1);
        const auto thick = physicalShape(logical, scale, 7, 3);
        CHECK(thick && thick->borderWidth == 3);
    }
    section("render factories: invalid, collapsed and unrepresentable shapes are omitted");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto logical : {SBoxF{0, 0, 0, 20}, SBoxF{0, 0, 20, -1}, SBoxF{nan, 0, 20, 20}, SBoxF{0, 0, inf, 20}, SBoxF{inf, 0, 20, 20}})
        CHECK(!physicalShape(logical, 1.2));
    for (const double scale : {0.0, -1.0, nan, inf})
        CHECK(!physicalShape({0, 0, 20, 20}, scale));
    CHECK(!physicalShape({.1, .1, .1, .1}, 1));
    CHECK(!physicalShape({0, 0, 20, 20}, 1.2, inf));
    CHECK(!physicalShape({0, 0, 20, 20}, 1.2, 2, nan));
    CHECK(!physicalShape({std::numeric_limits<double>::max(), 0, 20, 20}, 1.2));
    CHECK(!physicalShape({0, 0, 20, 20}, 1.2, std::numeric_limits<double>::max()));
    CHECK(physicalShape({0, 0, 20, 20}, 1.2, -2)->radius == 0);
}
