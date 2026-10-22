#pragma once

#include "../src/SpaceStyle.hpp"

static void testSpaceStyle() {
    using namespace hyprspace::spaceStyle;
    section("space style: known contrast references and theme mode overrides");
    const auto black = rgb(0x000000), white = rgb(0xffffff);
    CHECK_NEAR(luminance(black), 0, 1e-9);
    CHECK_NEAR(luminance(white), 1, 1e-9);
    CHECK_NEAR(contrast(black, white), 21, 1e-9);
    CHECK_NEAR(contrast(rgb(0x777777), white), 4.47809, .00002);
    CHECK_NEAR(contrast(white, rgb(0x777777)), 4.47809, .00002);
    const auto decoded = rgb(0x80112233);
    CHECK_NEAR(decoded.r, 17.0 / 255, 1e-9);
    CHECK_NEAR(decoded.g, 34.0 / 255, 1e-9);
    CHECK_NEAR(decoded.b, 51.0 / 255, 1e-9);
    CHECK(decoded.a == 1);

    const auto dark  = palette(rgb(0x141518), rgb(0x202127), rgb(0xcdd6f4), rgb(0x89b4fa));
    const auto light = palette(rgb(0xf4f5f7), white, rgb(0x252733), rgb(0x3c6eaf));
    CHECK(!dark.light && light.light);
    CHECK(!palette(rgb(0x747474), black, white, rgb(0x89b4fa), "auto").light);
    CHECK(palette(rgb(0x767676), white, black, rgb(0x89b4fa), "auto").light);
    const auto forcedLight = palette(rgb(0x141518), rgb(0x202127), white, rgb(0x89b4fa), "light");
    const auto forcedDark  = palette(rgb(0xf4f5f7), white, black, rgb(0x89b4fa), "dark");
    CHECK(forcedLight.light && !forcedDark.light);
    CHECK(luminance(forcedLight.background) > .7 && luminance(forcedDark.background) < .1);
    const auto customBackground = rgb(0xe8f0fa);
    const auto custom           = palette(customBackground, rgb(0xf2f6fc), rgb(0x203048), rgb(0x2a6299), "light");
    CHECK_NEAR(custom.background.r, customBackground.r, 1e-7);
    CHECK_NEAR(custom.background.g, customBackground.g, 1e-7);
    CHECK_NEAR(custom.background.b, customBackground.b, 1e-7);

    section("space style: control surfaces stay neutral independently of window-title colors");
    const auto checkSurface = [&](const SPalette& style, uint32_t expected) {
        const auto value = rgb(expected);
        CHECK_NEAR(style.surface.r, value.r, 1e-7);
        CHECK_NEAR(style.surface.g, value.g, 1e-7);
        CHECK_NEAR(style.surface.b, value.b, 1e-7);
    };
    checkSurface(dark, 0x1b1c1f);
    checkSurface(light, 0xf4f5f7);
    checkSurface(forcedDark, 0x1b1c1f);
    checkSurface(forcedLight, 0xf4f5f7);
    const auto purpleTitle = palette(rgb(0x141518), rgb(0x6030c0), rgb(0xcdd6f4), rgb(0x89b4fa));
    const auto greenTitle  = palette(rgb(0x141518), rgb(0x40d080), rgb(0xcdd6f4), rgb(0x89b4fa));
    for (const auto& style : {purpleTitle, greenTitle}) {
        checkSurface(style, 0x1b1c1f);
        CHECK_NEAR(style.hover.r, dark.hover.r, 1e-7);
        CHECK_NEAR(style.hover.g, dark.hover.g, 1e-7);
        CHECK_NEAR(style.hover.b, dark.hover.b, 1e-7);
        CHECK_NEAR(style.selected.r, dark.selected.r, 1e-7);
        CHECK_NEAR(style.selected.g, dark.selected.g, 1e-7);
        CHECK_NEAR(style.selected.b, dark.selected.b, 1e-7);
    }

    section("space style: opaque text, actions and colored symbols remain legible in both themes");
    const auto checkPalette = [&](const SPalette& style, double textMinimum) {
        for (const auto background : {style.background, style.surface, style.raised}) {
            CHECK(contrast(style.text, background) >= textMinimum - 1e-5);
            CHECK(contrast(style.muted, background) >= 4.5 - 1e-5);
            CHECK(contrast(style.subtle, background) >= 4.5 - 1e-5);
        }
        for (const auto color : {style.background, style.surface, style.raised, style.text, style.muted, style.subtle, style.focus, style.primary, style.primaryText})
            CHECK(std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && color.a == 1 && color.r >= 0 && color.r <= 1 && color.g >= 0 &&
                  color.g <= 1 && color.b >= 0 && color.b <= 1);
        CHECK(contrast(style.primaryText, style.primary) >= 4.5 - 1e-5);
        for (const auto surface : {style.surface, style.hover, style.selected})
            CHECK(contrast(style.focus, surface) >= 3 - 1e-5);
        for (const uint32_t tint : {0x89b4fa, 0xa6e3a1, 0xf9e2af, 0xfab387, 0xf38ba8, 0xcba6f7, 0x94e2d5, 0xbac2de, 0xffffff, 0x000000}) {
            const auto symbol = identity(rgb(tint), style);
            CHECK(symbol.a == 1);
            for (const auto surface : {style.background, style.surface, style.hover, style.selected})
                CHECK(contrast(symbol, surface) >= 3 - 1e-5);
        }
    };
    for (const auto& style : {dark, light, forcedLight, forcedDark, custom})
        checkPalette(style, 7);
    // A user-provided gray theme near the auto threshold still needs readable
    // text after hover/raised surfaces are derived from its background.
    for (const uint32_t gray : {0x707070, 0x747474, 0x767676, 0x808080})
        checkPalette(palette(rgb(gray), rgb(gray), rgb(0x999999), rgb(0xf9e2af)), 4.5);

    section("space style: pastel icon corrections preserve identity without changing stored colors");
    const auto yellow     = rgb(0xf9e2af);
    const auto darkYellow = identity(yellow, dark);
    CHECK_NEAR(darkYellow.r, yellow.r, 1e-7);
    CHECK_NEAR(darkYellow.g, yellow.g, 1e-7);
    CHECK_NEAR(darkYellow.b, yellow.b, 1e-7);
    const auto lightYellow = identity(yellow, light);
    CHECK(luminance(lightYellow) < luminance(yellow));
    CHECK(contrast(lightYellow, light.surface) >= 3 - 1e-5);
    CHECK_NEAR(yellow.r, 249.0 / 255, 1e-9);
    CHECK_NEAR(yellow.g, 226.0 / 255, 1e-9);
    CHECK_NEAR(yellow.b, 175.0 / 255, 1e-9);
}
