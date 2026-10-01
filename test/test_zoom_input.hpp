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
