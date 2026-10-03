#pragma once

// Include after the host suite's CHECK and section helpers.
static void testZoomInput() {
    section("input: zoom holds are independent and late releases cannot end a new session");
    hyprspace::CZoomHoldState holds;
    CHECK(!holds.held());
    CHECK(!holds.release(0));

    const auto first = holds.press();
    CHECK(first != 0);
    CHECK(holds.held());
    const auto second = holds.press();
    CHECK(second > first);
    CHECK(holds.release(first));
    CHECK(holds.held());
    CHECK(!holds.release(first));
    CHECK(holds.held());
    CHECK(holds.release(second));
    CHECK(!holds.held());

    const auto cancelled = holds.press();
    holds.cancel();
    CHECK(!holds.held());
    const auto current = holds.press();
    CHECK(current > cancelled);
    CHECK(!holds.release(cancelled));
    CHECK(!holds.release(second));
    CHECK(holds.held());
    CHECK(holds.release(current));
    CHECK(!holds.held());

    const auto unplugged = holds.press();
    const auto remaining = holds.press();
    CHECK(holds.release(unplugged));
    CHECK(holds.held());
    CHECK(holds.release(remaining));
    CHECK(!holds.held());
    holds.cancel();
    holds.cancel();
    CHECK(!holds.held());
}

static void testZoomEdgeHover() {
    using Clock = CZoomEdgeHover::Clock;
    using namespace std::chrono_literals;
    const auto     start = Clock::time_point{};
    CZoomEdgeHover hover;

    section("zoom edges: dwell waits 250ms and fires once per real entry");
    CHECK(hover.motion(EDirection::RIGHT, true, start));
    CHECK(!hover.advance(EDirection::RIGHT, true, start + 249ms));
    CHECK(!hover.motion(EDirection::RIGHT, true, start + 249ms));
    CHECK(hover.advance(EDirection::RIGHT, true, start + 250ms) == EDirection::RIGHT);
    CHECK(!hover.advance(EDirection::RIGHT, true, start + 1s));
    CHECK(!hover.motion(EDirection::RIGHT, true, start + 1s));
    hover.sync(EDirection::RIGHT);
    CHECK(!hover.motion(EDirection::RIGHT, true, start + 1s));
    CHECK(!hover.motion(std::nullopt, true, start + 1s));
    CHECK(hover.motion(EDirection::RIGHT, true, start + 2s));
    CHECK(hover.advance(EDirection::RIGHT, true, start + 2250ms) == EDirection::RIGHT);

    section("zoom edges: cancellation and resynchronization wait for real motion");
    hover.sync(std::nullopt);
    CHECK(hover.motion(EDirection::UP, true, start));
    CHECK(!hover.motion(std::nullopt, true, start + 100ms));
    CHECK(!hover.advance(std::nullopt, true, start + 1s));
    CHECK(hover.motion(EDirection::DOWN, true, start + 2s));
    hover.cancel();
    CHECK(!hover.advance(EDirection::DOWN, true, start + 3s));
    CHECK(hover.motion(EDirection::DOWN, true, start + 3s));
    hover.sync(EDirection::LEFT);
    CHECK(!hover.pending());
    CHECK(!hover.advance(EDirection::LEFT, true, start + 4s));
    CHECK(hover.motion(EDirection::LEFT, true, start + 4s));

    section("zoom edges: disabled input and changed targets cancel pending intent");
    hover.sync(std::nullopt);
    CHECK(!hover.motion(EDirection::RIGHT, false, start));
    CHECK(!hover.advance(EDirection::RIGHT, true, start + 1s));
    CHECK(hover.motion(EDirection::RIGHT, true, start + 1s));
    hover.sync(std::nullopt);
    CHECK(hover.motion(EDirection::RIGHT, true, start));
    hover.cancel();
    CHECK(!hover.advance(EDirection::RIGHT, true, start + 1s));
    hover.sync(std::nullopt);
    CHECK(hover.motion(EDirection::RIGHT, true, start));
    CHECK(!hover.advance(EDirection::LEFT, true, start + 250ms));

    section("zoom edges: an unfinished visit recovers on real motion after readiness changes");
    hover.sync(std::nullopt);
    CHECK(!hover.motion(EDirection::UP, false, start));
    CHECK(hover.motion(EDirection::UP, true, start + 100ms));
    CHECK(!hover.advance(EDirection::UP, true, start + 349ms));
    CHECK(hover.advance(EDirection::UP, true, start + 350ms) == EDirection::UP);
    CHECK(!hover.motion(EDirection::UP, true, start + 1s));

    section("zoom edges: an intentional entry waits for the camera, then receives a full dwell");
    hover.sync(std::nullopt);
    CHECK(hover.motion(EDirection::UP, true, start, false));
    CHECK(!hover.advance(EDirection::UP, false, start + 1s));
    CHECK(hover.pending() == EDirection::UP);
    CHECK(!hover.advance(EDirection::UP, true, start + 2s));
    CHECK(!hover.advance(EDirection::UP, true, start + 2249ms));
    CHECK(hover.advance(EDirection::UP, true, start + 2250ms) == EDirection::UP);
    CHECK(!hover.motion(EDirection::UP, true, start + 3s));

    section("zoom edges: a camera restart resets elapsed dwell without losing the visit");
    hover.sync(std::nullopt);
    CHECK(hover.motion(EDirection::DOWN, true, start));
    CHECK(!hover.advance(EDirection::DOWN, false, start + 200ms));
    CHECK(!hover.advance(EDirection::DOWN, true, start + 1s));
    CHECK(!hover.advance(EDirection::DOWN, true, start + 1249ms));
    CHECK(hover.advance(EDirection::DOWN, true, start + 1250ms) == EDirection::DOWN);
}
