#pragma once

#include "Raster.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace hyprspace::spaceStyle {
    // Opaque semantic colors keep text and state cues predictable over previews.
    struct SPalette {
        bool  light = false;
        SRgba background, surface, raised, text, muted, subtle, border;
        SRgba hover, selected, focus, primary, primaryText, error, veil, shadow;
    };

    inline SRgba rgb(uint32_t value) {
        return {static_cast<double>((value >> 16) & 255) / 255, static_cast<double>((value >> 8) & 255) / 255, static_cast<double>(value & 255) / 255, 1};
    }

    inline SRgba opaque(const SRgba& value) {
        return SRgba{static_cast<float>(value.r), static_cast<float>(value.g), static_cast<float>(value.b), 1.F};
    }

    inline SRgba mix(const SRgba& from, const SRgba& to, double amount) {
        amount = std::clamp(amount, 0.0, 1.0);
        return SRgba{static_cast<float>(from.r + (to.r - from.r) * amount), static_cast<float>(from.g + (to.g - from.g) * amount),
                     static_cast<float>(from.b + (to.b - from.b) * amount), 1.F};
    }

    inline double luminance(const SRgba& value) {
        const auto linear = [](double channel) { return channel <= .04045 ? channel / 12.92 : std::pow((channel + .055) / 1.055, 2.4); };
        return .2126 * linear(value.r) + .7152 * linear(value.g) + .0722 * linear(value.b);
    }

    inline double contrast(const SRgba& a, const SRgba& b) {
        const double first = luminance(a), second = luminance(b);
        return (std::max(first, second) + .05) / (std::min(first, second) + .05);
    }

    inline SRgba readableOn(const SRgba& value, std::initializer_list<SRgba> backgrounds, double minimum) {
        const auto score = [&](const SRgba& candidate) {
            double result = 21;
            for (const auto& background : backgrounds)
                result = std::min(result, contrast(candidate, background));
            return result;
        };
        if (score(value) >= minimum)
            return opaque(value);
        const SRgba black{0.F, 0.F, 0.F, 1.F}, white{1.F, 1.F, 1.F, 1.F};
        const auto  target = score(black) > score(white) ? black : white;
        double      low = 0, high = 1;
        for (int i = 0; i < 24; ++i) {
            const double middle = (low + high) / 2;
            if (score(mix(value, target, middle)) < minimum)
                low = middle;
            else
                high = middle;
        }
        return mix(value, target, high);
    }

    inline SRgba readable(const SRgba& value, const SRgba& background, double minimum) {
        return readableOn(value, {background}, minimum);
    }

    inline SPalette palette(SRgba background, SRgba, SRgba label, SRgba accent, std::string_view mode = "auto") {
        SPalette result;
        result.light = mode == "light" || (mode != "dark" && luminance(background) >= .18);
        const SRgba white{1.F, 1.F, 1.F, 1.F}, black{0.F, 0.F, 0.F, 1.F};
        const auto  foreground = result.light ? black : white;
        // A forced mode also works when the overview still has its default colors.
        if ((luminance(background) >= .18) != result.light)
            background = result.light ? rgb(0xfff4f5f7) : rgb(0xff141518);
        result.background = opaque(background);
        // Carry the overview's hue into controls instead of placing unrelated
        // charcoal cards over it. A neutral anchor keeps unusual backgrounds
        // readable; title-label backgrounds do not influence the controls.
        result.surface     = mix(result.light ? white : rgb(0x0c0c12), result.background, .65);
        result.surface     = mix(result.surface, foreground, result.light ? .015 : .02);
        result.raised      = mix(result.surface, foreground, .025);
        result.border      = mix(result.surface, foreground, result.light ? .12 : .10);
        result.hover       = mix(result.surface, foreground, .035);
        result.selected    = mix(result.surface, accent, result.light ? .08 : .09);
        result.text        = readableOn(label, {result.background, result.surface, result.raised, result.hover, result.selected}, 7);
        result.muted       = readableOn(mix(result.surface, result.text, .66), {result.background, result.surface, result.raised, result.hover, result.selected}, 4.5);
        result.subtle      = readableOn(mix(result.surface, result.text, .54), {result.background, result.surface, result.raised, result.hover, result.selected}, 4.5);
        result.focus       = readableOn(accent, {result.surface, result.hover, result.selected}, 3);
        result.primary     = readable(accent, result.surface, 3);
        result.primaryText = readable(result.text, result.primary, 4.5);
        result.error       = readable(result.light ? rgb(0xffad2237) : rgb(0xffff8a98), result.surface, 4.5);
        result.veil        = SRgba{0.F, 0.F, 0.F, result.light ? .24F : .48F};
        result.shadow      = SRgba{0.F, 0.F, 0.F, result.light ? .12F : .28F};
        return result;
    }

    inline SRgba identity(const SRgba& value, const SPalette& palette) {
        return readableOn(value, {palette.background, palette.surface, palette.hover, palette.selected}, 3);
    }
} // namespace hyprspace::spaceStyle
