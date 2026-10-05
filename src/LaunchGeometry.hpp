#pragma once

#include "Interaction.hpp"

namespace hyprspace {
    // Preserve the application's size while keeping its decorated rectangle
    // inside the usable work area. Oversized axes keep their leading edge
    // accessible instead of moving the title area outside the output.
    inline std::optional<SBoxF> boundedLaunchPlacement(const SBoxF& desired, const SBoxF& work, SPoint before = {}, SPoint after = {}) {
        const auto validBox = [](const SBoxF& box) {
            return std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.w) && std::isfinite(box.h) && box.w > 0 && box.h > 0;
        };
        if (!validBox(desired) || !validBox(work) || !std::isfinite(before.x) || !std::isfinite(before.y) || !std::isfinite(after.x) || !std::isfinite(after.y) ||
            before.x < 0 || before.y < 0 || after.x < 0 || after.y < 0)
            return std::nullopt;

        const double left = std::ceil(work.x + before.x), top = std::ceil(work.y + before.y);
        const double right = std::floor(work.x + work.w - after.x), bottom = std::floor(work.y + work.h - after.y);
        if (right <= left || bottom <= top)
            return std::nullopt;

        return SBoxF{desired.w <= right - left ? std::clamp(desired.x, left, right - desired.w) : left,
                     desired.h <= bottom - top ? std::clamp(desired.y, top, bottom - desired.h) : top, desired.w, desired.h};
    }
} // namespace hyprspace
