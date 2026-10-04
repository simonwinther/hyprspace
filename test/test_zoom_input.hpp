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

    section("inspection input: fractional wheels accumulate without detent quantization");
    const SBoxF     footprint{100, 70, 900, 600};
    CInspectionZoom full, fractions, fallback;
    CHECK(full.scroll({.delta = -15, .value120 = -120}, 1, footprint, 400, 300, 1));
    CHECK_NEAR(full.goal().scale, 1.15, 1e-12);
    for (int i = 0; i < 4; ++i)
        CHECK(fractions.scroll({.delta = -3.75, .value120 = -30}, 1, footprint, 400, 300, 1));
    CHECK(fallback.scroll({.delta = -15}, 1, footprint, 400, 300, 1));
    CHECK_NEAR(fractions.goal().scale, full.goal().scale, 1e-12);
    CHECK_NEAR(fractions.goal().x, full.goal().x, 1e-10);
    CHECK_NEAR(fractions.goal().y, full.goal().y, 1e-10);
    CHECK(fallback.goal() == full.goal());

    section("inspection input: the first wheel takes over an unfinished fit without losing ticks or geometry");
    const SOverviewCamera fit{3, -450, -100}, pending{1.2, -20, 100};
    const SBoxF           cell{150, 100, 300, 200};
    const auto            fittedCell = fit.apply(cell), pendingCell = pending.apply(cell);
    CInspectionZoom       early;
    CHECK(early.beginFitTransition(pending, fit));
    const auto takeover = early.current(0);
    CHECK_NEAR(takeover.scale, 0.4, 1e-12);
    CHECK(early.goal() == SOverviewCamera{});
    CHECK(!early.scroll({.value120 = 120}, 1, fittedCell, 340, 340, 0, pendingCell));
    CHECK(early.current(0) == takeover);
    CHECK(early.scroll({.value120 = -480}, 1, fittedCell, 340, 340, 0, pendingCell));
    CHECK(early.current(0) == takeover);
    CHECK_NEAR(early.goal().scale, std::pow(1.15, 4), 1e-12);
    for (double progress : {0.0, 0.2, 0.5, 0.8, 1.0}) {
        const auto camera = early.current(progress);
        CHECK_NEAR(camera.apply({450, 500, 0, 0}).x, 340, 1e-9);
        CHECK_NEAR(camera.apply({450, 500, 0, 0}).y, 340, 1e-9);
    }
    CHECK(early.scroll({.value120 = -120}, 1, fittedCell, 340, 340, 0.1));
    CHECK_NEAR(early.goal().scale, std::pow(1.15, 5), 1e-12);
    CHECK(early.scroll({.value120 = 120}, 1, fittedCell, 340, 340, 0.05));
    CHECK(early.goal() == SOverviewCamera{});
    early.reset();
    CHECK(early.current(0.3) == SOverviewCamera{});

    section("inspection input: frozen fit pans require refitting even when their scale is valid");
    CInspectionZoom pan;
    CHECK(pan.beginFitTransition({fit.scale, fit.x + 140, fit.y - 60}, fit));
    CHECK(pan.goalFits(fittedCell));
    CHECK(pan.scroll({.value120 = -120}, 1, fittedCell, 450, 500, 0));
    CHECK(pan.goalFits(fittedCell));
    pan.freeze(0);
    CHECK(pan.goal().scale == 1);
    CHECK(!pan.goalFits(fittedCell));
    pan.reset();
    CHECK(pan.beginFitTransition({fit.scale * 1.2, fit.x * 1.2 + 300, fit.y * 1.2}, fit));
    CHECK(pan.scroll({.value120 = -120}, 1, fittedCell, 450, 500, 0));
    pan.freeze(0);
    CHECK(pan.goal().scale > 1 && pan.goal().scale < 4);
    CHECK(!pan.goalFits(fittedCell));
    CInspectionZoom validFreeze;
    CHECK(validFreeze.scroll({.value120 = -480}, 1, fittedCell, fittedCell.x, fittedCell.y, 1));
    for (double progress : {0.0, 0.1, 0.5, 0.9, 1.0}) {
        auto frozen = validFreeze;
        frozen.freeze(progress);
        CHECK(frozen.goalFits(fittedCell));
    }

    section("inspection pan: pickup keeps the displayed camera and logical motion changes translation only");
    CInspectionZoom grip;
    CHECK(!grip.canPan(footprint, 1));
    CHECK(!grip.beginPan(footprint, 1));
    CHECK(!grip.pan(footprint, 20, 30));
    CHECK(grip.goal() == SOverviewCamera{});
    CHECK(grip.scroll({.value120 = -480}, 1, footprint, 400, 300, 1));
    const auto displayedGrip = grip.current(0.45);
    CHECK(grip.beginPan(footprint, 0.45));
    CHECK(grip.goal() == displayedGrip);
    CHECK(grip.current(0) == displayedGrip);
    CHECK(grip.pan(footprint, 12, -9));
    CHECK(grip.goal().scale == displayedGrip.scale);
    CHECK_NEAR(grip.goal().x, displayedGrip.x + 12, 1e-10);
    CHECK_NEAR(grip.goal().y, displayedGrip.y - 9, 1e-10);
    CHECK(grip.current(0) == grip.current(1));
    CHECK(grip.goalFits(footprint));

    section("inspection pan: bounds absorb excess without delaying reversal and wheel-out restores identity");
    CHECK(grip.pan(footprint, std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()));
    const auto extreme = grip.goal();
    CHECK_NEAR(extreme.x, footprint.x * (1 - extreme.scale), 1e-10);
    CHECK_NEAR(extreme.y, (footprint.y + footprint.h) * (1 - extreme.scale), 1e-10);
    CHECK(!grip.pan(footprint, 1000, -1000));
    CHECK(grip.pan(footprint, -1, 1));
    CHECK_NEAR(grip.goal().x, extreme.x - 1, 1e-10);
    CHECK_NEAR(grip.goal().y, extreme.y + 1, 1e-10);
    for (const SBoxF bounds : {footprint, SBoxF{-900, -470, 500, 800}, SBoxF{0, 40, 300, 900}}) {
        CInspectionZoom bounded;
        CHECK(bounded.scroll({.value120 = -480}, 1, bounds, bounds.cx(), bounds.cy(), 1));
        CHECK(bounded.beginPan(bounds, 1));
        for (const auto delta : {SPoint{-10000, -10000}, SPoint{10000, 10000}, SPoint{-1, -1}, SPoint{50, -70}}) {
            bounded.pan(bounds, delta.x, delta.y);
            CHECK(bounded.goalFits(bounds));
            const auto enlarged = bounded.goal().apply(bounds);
            CHECK(enlarged.x <= bounds.x + 1e-8);
            CHECK(enlarged.y <= bounds.y + 1e-8);
            CHECK(enlarged.x + enlarged.w >= bounds.x + bounds.w - 1e-8);
            CHECK(enlarged.y + enlarged.h >= bounds.y + bounds.h - 1e-8);
        }
        CHECK(bounded.scroll({.value120 = 3840}, 1, bounds, bounds.cx(), bounds.cy(), 1));
        CHECK(bounded.goal() == SOverviewCamera{});
    }

    section("inspection pan: pending fit and malformed input cannot freeze or corrupt the camera");
    CInspectionZoom notReady;
    CHECK(notReady.beginFitTransition(pending, fit));
    CHECK(notReady.scroll({.value120 = -480}, 1, fittedCell, 340, 340, 0, pendingCell));
    const auto pendingGoal = notReady.goal(), pendingFrom = notReady.current(0);
    CHECK(!notReady.beginPan(fittedCell, 0));
    CHECK(notReady.goal() == pendingGoal && notReady.current(0) == pendingFrom);
    CHECK(!grip.beginPan(footprint, NAN));
    const auto beforeMalformedPan = grip.goal();
    for (const auto delta : {SPoint{NAN, 0}, SPoint{0, INFINITY}, SPoint{-INFINITY, 0}})
        CHECK(!grip.pan(footprint, delta.x, delta.y));
    CHECK(!grip.pan({}, 1, 2));
    CHECK(grip.goal() == beforeMalformedPan);

    section("pan button capture: orphaned releases stay owned but the next press makes a fresh decision");
    CButtonCapture leases;
    CHECK(leases.consume(273, true, true));
    leases.orphan(273);
    CHECK(leases.consume(273, false, false));
    CHECK(!leases.captured(273));
    CHECK(leases.consume(273, true, true));
    leases.orphan(273);
    CHECK(!leases.consume(273, true, false));
    CHECK(!leases.consume(273, false, false));
    CHECK(leases.consume(273, true, true));
    leases.orphan(273);
    leases.preparePress(273);
    CHECK(!leases.captured(273));
    CHECK(leases.consume(273, true, true));
    CHECK(leases.consume(273, false, false));
    CHECK(leases.consume(272, true, true));
    CHECK(leases.consume(273, true, true));
    leases.orphan(273);
    CHECK(!leases.consume(273, true, false));
    CHECK(leases.captured(272));
    CHECK(leases.consume(272, false, false));
    leases.clear();

    section("inspection input: opening anchors use the displayed cell and handoff targets remain bounded");
    CInspectionZoom opening;
    const SBoxF     morphingCell{100, 170, 600, 600};
    CHECK(opening.scroll({.value120 = -120}, 1, fittedCell, 400, 470, 1, morphingCell));
    const SBoxF normalizedAnchor{fittedCell.cx(), fittedCell.cy(), 0, 0};
    CHECK_NEAR(opening.goal().apply(normalizedAnchor).x, 400, 1e-9);
    CHECK_NEAR(opening.goal().apply(normalizedAnchor).y, 470, 1e-9);
    CInspectionZoom repressed;
    CHECK(repressed.beginFitTransition({24, -600, -200}, fit));
    CHECK(repressed.current(0).scale == 8);
    CHECK(repressed.scroll({.value120 = -120}, 1, fittedCell, 400, 400, 0));
    CHECK_NEAR(repressed.goal().scale, 1.15, 1e-12);
    CHECK(repressed.scroll({.value120 = 120}, 1, fittedCell, 400, 400, 0.05));
    CHECK_NEAR(repressed.goal().scale, 4 / 1.15, 1e-12);
    const auto validStart = repressed.current(0), validGoal = repressed.goal();
    CHECK(!repressed.beginFitTransition({NAN, 0, 0}, fit));
    CHECK(repressed.current(0) == validStart && repressed.goal() == validGoal);

    section("inspection input: displayed anchors survive interruption at another pointer position");
    const auto checkFootprint = [&](const SOverviewCamera& camera, const SBoxF& original) {
        const auto transformed = camera.apply(original);
        CHECK(std::isfinite(camera.scale) && std::isfinite(camera.x) && std::isfinite(camera.y));
        CHECK(camera.scale >= 1 && camera.scale <= 4);
        CHECK(transformed.x <= original.x + 1e-8);
        CHECK(transformed.y <= original.y + 1e-8);
        CHECK(transformed.x + transformed.w >= original.x + original.w - 1e-8);
        CHECK(transformed.y + transformed.h >= original.y + original.h - 1e-8);
    };
    for (double progress : {-0.2, 0.0, 0.25, 0.5, 0.75, 1.0, 1.2}) {
        const auto camera = full.current(progress);
        CHECK_NEAR(camera.apply({400, 300, 0, 0}).x, 400, 1e-9);
        CHECK_NEAR(camera.apply({400, 300, 0, 0}).y, 300, 1e-9);
        checkFootprint(camera, footprint);
    }
    const auto   before  = full.current(0.4);
    const double anchorX = (800 - before.x) / before.scale, anchorY = (350 - before.y) / before.scale;
    CHECK(full.scroll({.value120 = -120}, 1, footprint, 800, 350, 0.4));
    CHECK(full.current(0) == before);
    CHECK_NEAR(full.goal().scale, 1.15 * 1.15, 1e-12);
    for (double progress : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const auto camera = full.current(progress);
        CHECK_NEAR(camera.apply({anchorX, anchorY, 0, 0}).x, 800, 1e-9);
        CHECK_NEAR(camera.apply({anchorX, anchorY, 0, 0}).y, 350, 1e-9);
        checkFootprint(camera, footprint);
    }

    section("inspection input: reversing starts from displayed magnification and clears pending excess");
    CInspectionZoom reversal;
    CHECK(reversal.scroll({.value120 = -480}, 1, footprint, 400, 300, 1));
    const auto reversalStart = reversal.current(0.8);
    CHECK(reversal.scroll({.value120 = 120}, 1, footprint, 400, 300, 0.8));
    CHECK(reversal.current(0) == reversalStart);
    CHECK_NEAR(reversal.goal().scale, reversalStart.scale / 1.15, 1e-12);
    CHECK(reversal.goal().scale < reversalStart.scale);
    CHECK(reversal.scroll({.value120 = 100000}, 1, footprint, 400, 300, 1));
    CHECK(reversal.goal() == SOverviewCamera{});
    CHECK(reversal.current(1) == SOverviewCamera{});
    CHECK(!reversal.scroll({.value120 = 120}, 1, footprint, 900, 500, 1));
    CHECK(reversal.scroll({.value120 = -120}, 1, footprint, 400, 300, 1));
    CHECK_NEAR(reversal.goal().scale, 1.15, 1e-12);

    section("inspection input: upper-limit events retain the current anchor and animation");
    CInspectionZoom limit;
    CHECK(limit.scroll({.value120 = -100000}, 1, footprint, 400, 300, 1));
    CHECK(limit.goal().scale == 4);
    const auto limitStart = limit.current(0), limitGoal = limit.goal();
    CHECK(!limit.scroll({.value120 = -120}, 1, footprint, 900, 500, 0.3));
    CHECK(limit.current(0) == limitStart);
    CHECK(limit.goal() == limitGoal);
    CHECK(limit.scroll({.value120 = 120}, 1, footprint, 400, 300, 1));
    CHECK_NEAR(limit.goal().scale, 4 / 1.15, 1e-12);
    limit.freeze(0.35);
    CHECK(limit.current(0) == limit.goal());
    CHECK(limit.current(1) == limit.goal());
    limit.reset();
    CHECK(limit.current(0) == SOverviewCamera{});
    CHECK(limit.goal() == SOverviewCamera{});

    section("inspection input: containment survives out-of-bounds anchors and negative origins");
    for (const auto area : {footprint, SBoxF{-1200, -750, 1000, 600}, SBoxF{20, 40, 420, 840}}) {
        for (const auto point : {area.x - 300, area.x, area.cx(), area.x + area.w, area.x + area.w + 300}) {
            CInspectionZoom constrained;
            CHECK(constrained.scroll({.value120 = -240}, 1, area, point, area.y - 200, 1));
            for (double progress : {0.0, 0.2, 0.8, 1.0})
                checkFootprint(constrained.current(progress), area);
            CHECK(constrained.scroll({.value120 = 100000}, 1, area, point, area.y - 200, 1));
            CHECK(constrained.goal() == SOverviewCamera{});
            for (double progress : {0.0, 0.2, 0.8, 1.0})
                checkFootprint(constrained.current(progress), area);
        }
    }

    section("inspection input: source filtering and malformed events preserve active state");
    CInspectionZoom filtered;
    CHECK(filtered.scroll({.value120 = -120}, 1, footprint, 400, 300, 1));
    const auto filteredStart = filtered.current(0), filteredGoal = filtered.goal();
    for (const auto event :
         {SScrollInput{.value120 = -120, .wheel = false}, SScrollInput{.value120 = -120, .horizontal = true}, SScrollInput{.value120 = -120, .wheelTilt = true},
          SScrollInput{.delta = NAN, .value120 = -120}, SScrollInput{.delta = INFINITY}, SScrollInput{}})
        CHECK(!filtered.scroll(event, 1, footprint, 400, 300, 0.5));
    for (double factor : {0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        CHECK(!filtered.scroll({.value120 = -120}, factor, footprint, 400, 300, 0.5));
    for (const auto area : {SBoxF{}, SBoxF{NAN, 0, 900, 600}, SBoxF{0, 0, -900, 600}, SBoxF{0, 0, 900, INFINITY},
                            SBoxF{std::numeric_limits<double>::max(), 0, std::numeric_limits<double>::max(), 600}})
        CHECK(!filtered.scroll({.value120 = -120}, 1, area, 400, 300, 0.5));
    CHECK(!filtered.scroll({.value120 = -120}, 1, footprint, NAN, 300, 0.5));
    CHECK(!filtered.scroll({.value120 = -120}, 1, footprint, 400, INFINITY, 0.5));
    CHECK(!filtered.scroll({.value120 = -120}, 1, footprint, 400, 300, NAN));
    CHECK(filtered.current(0) == filteredStart);
    CHECK(filtered.goal() == filteredGoal);

    section("inspection input: signed device factors apply once and huge finite values remain bounded");
    CInspectionZoom signedFactor;
    CHECK(signedFactor.scroll({.value120 = 120}, -1, footprint, 400, 300, 1));
    CHECK_NEAR(signedFactor.goal().scale, 1.15, 1e-12);
    signedFactor.reset();
    CHECK(signedFactor.scroll({.value120 = -120}, 2, footprint, 400, 300, 1));
    CHECK_NEAR(signedFactor.goal().scale, 1.15 * 1.15, 1e-12);
    for (double sign : {-1.0, 1.0}) {
        CInspectionZoom huge;
        const double    maximum = std::numeric_limits<double>::max();
        CHECK(huge.scroll({.delta = -sign * maximum}, sign * maximum, footprint, 400, 300, 1));
        CHECK(huge.goal().scale == 4);
        checkFootprint(huge.current(1), footprint);
        CHECK(huge.scroll({.delta = sign * maximum}, sign * maximum, footprint, 400, 300, 1));
        CHECK(huge.goal() == SOverviewCamera{});
    }
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
