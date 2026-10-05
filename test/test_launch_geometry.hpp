#pragma once

#include "../src/LaunchGeometry.hpp"

static void testLaunchGeometry() {
    section("launch: cursor placement keeps size and decorated edges inside logical work areas");
    for (const auto work : {SBoxF{0, 32, 1366, 736}, SBoxF{-1000, -168, 480, 736}, SBoxF{2200, 132.5, 639.5, 367.5}}) {
        const SPoint before{3, 24}, after{3, 3};
        for (double x : {-0.2, 0.0, 0.03, 0.5, 0.97, 1.0, 1.2})
            for (double y : {-0.2, 0.0, 0.03, 0.5, 0.97, 1.0, 1.2}) {
                const SBoxF desired{work.x + work.w * x - 160, work.y + work.h * y - 120, 320, 240};
                const auto  box = boundedLaunchPlacement(desired, work, before, after);
                CHECK(box.has_value());
                CHECK_NEAR(box->w, desired.w, 1e-8);
                CHECK_NEAR(box->h, desired.h, 1e-8);
                CHECK(box->x - before.x >= work.x);
                CHECK(box->y - before.y >= work.y);
                CHECK(box->x + box->w + after.x <= work.x + work.w);
                CHECK(box->y + box->h + after.y <= work.y + work.h);
                if (x == 0.5 && y == 0.5) {
                    CHECK_NEAR(box->cx(), desired.cx(), 1e-8);
                    CHECK_NEAR(box->cy(), desired.cy(), 1e-8);
                }
            }
    }

    section("launch: oversized windows preserve size and an accessible decorated leading edge");
    const SBoxF work{100, 40, 800, 560};
    for (const SBoxF desired : {SBoxF{-400, -500, 1400, 900}, SBoxF{250, -500, 320, 900}, SBoxF{-400, 150, 1400, 240}}) {
        const auto box = boundedLaunchPlacement(desired, work, {4, 28}, {4, 4});
        CHECK(box.has_value());
        CHECK_NEAR(box->w, desired.w, 1e-8);
        CHECK_NEAR(box->h, desired.h, 1e-8);
        CHECK(box->x >= work.x + 4);
        CHECK(box->y >= work.y + 28);
        if (desired.w > work.w)
            CHECK_NEAR(box->x, work.x + 4, 1e-8);
        if (desired.h > work.h)
            CHECK_NEAR(box->y, work.y + 28, 1e-8);
    }

    section("launch: invalid geometry never produces a placement");
    CHECK(!boundedLaunchPlacement({}, work));
    CHECK(!boundedLaunchPlacement({0, 0, 100, 100}, {}));
    CHECK(!boundedLaunchPlacement({NAN, 0, 100, 100}, work));
    CHECK(!boundedLaunchPlacement({0, 0, INFINITY, 100}, work));
    CHECK(!boundedLaunchPlacement({0, 0, 100, 100}, work, {NAN, 0}));
    CHECK(!boundedLaunchPlacement({0, 0, 100, 100}, work, {-1, 0}));
    CHECK(!boundedLaunchPlacement({0, 0, 100, 100}, work, {800, 0}));
}
