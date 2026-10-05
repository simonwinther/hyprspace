#pragma once

#include "../src/CaptureGeometry.hpp"

// Included after the host suite's CHECK helpers and section() declaration.
static void testCaptureGeometry() {
    constexpr size_t LIMIT = 32 * 1024 * 1024;
    section("capture: native resolution survives and large captures stay bounded with their original aspect");
    for (const auto source : {SBoxF{0, 0, 1, 1}, SBoxF{0, 0, 320, 200}, SBoxF{0, 0, 3840, 2160}}) {
        const auto geometry = boundedCaptureGeometry(source.w, source.h, LIMIT);
        CHECK(geometry.has_value());
        CHECK(!geometry->downsampled());
        CHECK(geometry->pixelW == source.w && geometry->pixelH == source.h);
    }
    for (const auto source : {SBoxF{0, 0, 5120, 2880}, SBoxF{0, 0, 2880, 5120}, SBoxF{0, 0, 7680, 4320}, SBoxF{0, 0, 5000, 5000}}) {
        const auto geometry = boundedCaptureGeometry(source.w, source.h, LIMIT);
        CHECK(geometry.has_value());
        CHECK(geometry->downsampled());
        CHECK(geometry->sourceW == source.w && geometry->sourceH == source.h);
        CHECK(static_cast<uint64_t>(geometry->pixelW) * geometry->pixelH * 4 <= LIMIT);
        CHECK(geometry->pixelW <= source.w && geometry->pixelH <= source.h);
        CHECK(std::abs(geometry->pixelW - geometry->pixelH * source.w / source.h) <= source.w / source.h + 1);
        const auto rotated = boundedCaptureGeometry(source.h, source.w, LIMIT);
        CHECK(rotated->pixelW == geometry->pixelH && rotated->pixelH == geometry->pixelW);
        const auto full = capturePixelClip(*geometry, source);
        CHECK(full == (SBoxF{0, 0, static_cast<double>(geometry->pixelW), static_cast<double>(geometry->pixelH)}));
    }

    section("capture: integer, GPU dimension and extreme aspect bounds reject unsafe input");
    for (double invalid :
         {0.0, -1.0, 0.99, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), static_cast<double>(std::numeric_limits<int>::max()) + 1}) {
        CHECK(!boundedCaptureGeometry(invalid, 300, LIMIT));
        CHECK(!boundedCaptureGeometry(300, invalid, LIMIT));
    }
    CHECK(!boundedCaptureGeometry(400, 300, 3));
    CHECK(!boundedCaptureGeometry(400, 300, LIMIT, 0));
    const auto fractional = boundedCaptureGeometry(320.9, 200.9, LIMIT);
    CHECK(fractional->sourceW == 320 && fractional->sourceH == 200);
    CHECK(!fractional->downsampled());
    const auto dimension = boundedCaptureGeometry(20000, 2, LIMIT, 16384);
    CHECK(dimension->pixelW <= 16384 && dimension->pixelH >= 1);
    CHECK(static_cast<uint64_t>(dimension->pixelW) * dimension->pixelH * 4 <= LIMIT);
    for (size_t limit : {size_t{4}, size_t{8}, size_t{16}, LIMIT}) {
        const auto skinny = boundedCaptureGeometry(std::numeric_limits<int>::max(), 1, limit);
        CHECK(skinny.has_value());
        CHECK(skinny->pixelW >= 1 && skinny->pixelH == 1);
        CHECK(static_cast<uint64_t>(skinny->pixelW) * skinny->pixelH * 4 <= limit);
    }

    section("capture: source-space clips cover their mapped edges and never escape the texture");
    const auto geometry = *boundedCaptureGeometry(5120, 2880, LIMIT);
    for (const auto source :
         {SBoxF{25.2, 39.7, 800.1, 610.4}, SBoxF{-40, -50, 100, 120}, SBoxF{5000, 2780, 400, 300}, SBoxF{-500, -500, 6000, 4000}, SBoxF{2300.9, 1300.2, 0.2, 0.1}}) {
        const auto pixels = capturePixelClip(geometry, source);
        CHECK(pixels.x >= 0 && pixels.y >= 0 && pixels.w > 0 && pixels.h > 0);
        CHECK(pixels.x + pixels.w <= geometry.pixelW && pixels.y + pixels.h <= geometry.pixelH);
        CHECK(std::floor(pixels.x) == pixels.x && std::floor(pixels.y) == pixels.y);
        CHECK(std::floor(pixels.w) == pixels.w && std::floor(pixels.h) == pixels.h);
        const double left   = std::clamp(source.x, 0.0, static_cast<double>(geometry.sourceW)) * geometry.scaleX();
        const double top    = std::clamp(source.y, 0.0, static_cast<double>(geometry.sourceH)) * geometry.scaleY();
        const double right  = std::clamp(source.x + source.w, 0.0, static_cast<double>(geometry.sourceW)) * geometry.scaleX();
        const double bottom = std::clamp(source.y + source.h, 0.0, static_cast<double>(geometry.sourceH)) * geometry.scaleY();
        CHECK(pixels.x <= left && left - pixels.x < 1);
        CHECK(pixels.y <= top && top - pixels.y < 1);
        CHECK(pixels.x + pixels.w >= right && pixels.x + pixels.w - right < 1);
        CHECK(pixels.y + pixels.h >= bottom && pixels.y + pixels.h - bottom < 1);
    }
    for (const auto source :
         {SBoxF{-50, -40, 30, 20}, SBoxF{5200, 2900, 20, 30}, SBoxF{20, 30, 0, 10}, SBoxF{20, 30, 10, -1}, SBoxF{std::numeric_limits<double>::quiet_NaN(), 0, 10, 10}}) {
        const auto pixels = capturePixelClip(geometry, source);
        CHECK(pixels.w == 0 || pixels.h == 0);
    }
}
