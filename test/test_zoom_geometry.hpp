#pragma once

// Included after the host suite's CHECK helpers and section() declaration.
static void testZoomGeometry() {
    section("zoom: ten-workspace focus fits laptop, desktop and portrait work areas");
    for (const auto usable : {SBoxF{0, 32, 1366, 736}, SBoxF{0, 48, 2560, 1392}, SBoxF{0, 40, 1080, 1880}}) {
        SLayoutParams params;
        params.screenW    = usable.w;
        params.screenH    = usable.h;
        params.aspect     = usable.w / usable.h;
        params.padding    = 56;
        params.labelSpace = 34;
        std::vector<STileInput> input;
        for (size_t i = 0; i < 10; ++i)
            input.push_back({i, static_cast<long>(i + 1)});
        const auto tiles = layout(input, params).tiles;
        CHECK(tiles.size() == 10);
        for (const auto& tile : tiles) {
            auto base = tile.box;
            base.x += usable.x;
            base.y += usable.y;
            const auto camera  = overviewZoomCamera(base, usable, 56, 34);
            const auto focused = camera.apply(base);
            CHECK(camera.scale > 1);
            CHECK(focused.x >= usable.x + 56 - 1e-8);
            CHECK(focused.y >= usable.y + 56 - 1e-8);
            CHECK(focused.x + focused.w <= usable.x + usable.w - 56 + 1e-8);
            CHECK(focused.y + focused.h <= usable.y + usable.h - 56 - 34 + 1e-8);
            CHECK(focused.w > usable.w * 0.7);
            CHECK(focused.h > usable.h * 0.7);
            CHECK_NEAR(focused.cx(), usable.cx(), 1e-8);
            CHECK_NEAR(focused.cy(), usable.cy() - 17, 1e-8);
            CHECK_NEAR(focused.w / focused.h, base.w / base.h, 1e-8);
            CHECK(overviewCameraAt(camera, 0).apply(base) == base);
            CHECK(overviewCameraAt(camera, 1).apply(base) == focused);
        }
    }

    section("zoom: enlarged preview hit coordinates retain their desktop meaning");
    const SBoxF desktop{20, 40, 1320, 710}, cell{180, 160, 320, 172}, preview{210, 180, 230, 130};
    const auto  camera = overviewZoomCamera(cell, {0, 32, 1366, 736}, 56, 34);
    for (double progress : {0.0, 0.15, 0.6, 1.0}) {
        const auto current = overviewCameraAt(camera, progress);
        const auto box     = current.apply(preview);
        const auto clip    = current.apply(cell);
        CHECK(previewContains({box, clip}, box.cx(), box.cy()));
        CHECK(!previewContains({box, clip}, clip.x - 1, box.cy()));
        const auto point = mapPreviewPoint({box.x + box.w * 0.3, box.y + box.h * 0.7}, box, desktop);
        CHECK(point.has_value());
        CHECK_NEAR(point->x, desktop.x + desktop.w * 0.3, 1e-8);
        CHECK_NEAR(point->y, desktop.y + desktop.h * 0.7, 1e-8);
        const SOverviewCamera inverse{1 / current.scale, -current.x / current.scale, -current.y / current.scale};
        const auto            restored = inverse.apply(box);
        CHECK_NEAR(restored.x, preview.x, 1e-8);
        CHECK_NEAR(restored.y, preview.y, 1e-8);
        CHECK_NEAR(restored.w, preview.w, 1e-8);
        CHECK_NEAR(restored.h, preview.h, 1e-8);
    }

    section("zoom: resize pictures follow zoom-out without modifying pickup geometry");
    const SBoxF pickupCell = camera.apply(cell), pickupBox = camera.apply(preview);
    const auto  originalCell = pickupCell, originalBox = pickupBox;
    for (double progress : {1.0, 0.75, 0.2, 0.0}) {
        const auto current  = overviewCameraAt(camera, progress);
        const auto ghost    = reprojectOverviewBox(pickupBox, pickupCell, current.apply(cell));
        const auto expected = current.apply(preview);
        CHECK_NEAR(ghost.x, expected.x, 1e-8);
        CHECK_NEAR(ghost.y, expected.y, 1e-8);
        CHECK_NEAR(ghost.w, expected.w, 1e-8);
        CHECK_NEAR(ghost.h, expected.h, 1e-8);
        CHECK(pickupCell == originalCell);
        CHECK(pickupBox == originalBox);
    }

    section("zoom: close snapshots preserve partial-open geometry and old anchor visibility");
    const SWindowPreviewGeometry endpoint{{0, 0, 1366, 768}, {0, 0, 1366, 768}};
    for (float opening : {0.F, 0.15F, 0.65F, 1.F}) {
        const auto                   raw     = overviewWindowGeometry({preview, cell}, endpoint, true, opening);
        const auto                   current = overviewCameraAt(camera, opening * 0.45);
        const SWindowPreviewGeometry snapshot{current.apply(raw.box), current.apply(raw.clip)};
        const auto                   before = closingWindowPreviewGeometry(snapshot, endpoint, 1);
        CHECK(before.box == snapshot.box);
        CHECK(before.clip == snapshot.clip);
        const auto after = closingWindowPreviewGeometry(snapshot, endpoint, 0);
        CHECK(after.box == endpoint.box);
        CHECK(after.clip == endpoint.clip);
        const auto midway = closingWindowPreviewGeometry(snapshot, endpoint, 0.5);
        CHECK_NEAR(midway.box.x, (snapshot.box.x + endpoint.box.x) / 2, 1e-8);
        CHECK_NEAR(midway.clip.h, (snapshot.clip.h + endpoint.clip.h) / 2, 1e-8);
        for (bool oldAnchor : {false, true}) {
            const auto style   = workspacePreviewStyle(oldAnchor, false, opening);
            const auto initial = closingWorkspacePreviewStyle(style, !oldAnchor, 1);
            CHECK_NEAR(initial.visibility, style.visibility, 1e-8);
            CHECK_NEAR(initial.windowVisibility, style.windowVisibility, 1e-8);
            CHECK_NEAR(initial.plateVisibility, style.plateVisibility, 1e-8);
            const auto final = closingWorkspacePreviewStyle(style, !oldAnchor, 0);
            CHECK_NEAR(final.visibility, oldAnchor ? 0 : 1, 1e-8);
            CHECK_NEAR(final.plateVisibility, 0, 1e-8);
            const float visible = style.windowVisibility * overviewWindowVisibility(0.4F, oldAnchor, opening);
            CHECK_NEAR(closingWindowPreviewVisibility(visible, 0.4F, !oldAnchor, 1), visible, 1e-8);
            CHECK_NEAR(closingWindowPreviewVisibility(visible, 0.4F, !oldAnchor, 0), oldAnchor ? 0 : 0.4F, 1e-8);
        }
    }

    section("zoom: a single fitted tile and invalid layout inputs leave the camera unchanged");
    const SBoxF usable{0, 32, 1366, 736};
    const auto  fitted = fitBox({56, 88, 1254, 590}, usable.w / usable.h);
    CHECK(overviewZoomCamera(fitted, usable, 56, 34) == SOverviewCamera{});
    CHECK(overviewZoomCamera({}, usable, 56, 34) == SOverviewCamera{});
    CHECK(overviewZoomCamera(cell, usable, 1000, 34) == SOverviewCamera{});
    CHECK(overviewZoomCamera(cell, usable, NAN, 34) == SOverviewCamera{});
    CHECK(overviewCameraAt({NAN, 0, 0}, 1) == SOverviewCamera{});
    CHECK(reprojectOverviewBox(preview, {}, cell) == preview);
}
