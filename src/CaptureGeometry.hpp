// Capture resolution is independent of desktop and overview input geometry.
#pragma once

#include "Geometry.hpp"

#include <limits>
#include <optional>

namespace hyprspace {

    struct SCaptureGeometry {
        int sourceW = 0, sourceH = 0;
        int pixelW = 0, pixelH = 0;

        bool downsampled() const {
            return pixelW != sourceW || pixelH != sourceH;
        }
        double scaleX() const {
            return static_cast<double>(pixelW) / sourceW;
        }
        double scaleY() const {
            return static_cast<double>(pixelH) / sourceH;
        }
    };

    inline std::optional<SCaptureGeometry> boundedCaptureGeometry(double width, double height, size_t byteLimit, int dimensionLimit = std::numeric_limits<int>::max()) {
        if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 || width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max() || byteLimit < 4 || dimensionLimit < 1)
            return std::nullopt;

        const int    sourceW = static_cast<int>(width), sourceH = static_cast<int>(height);
        const size_t pixels = byteLimit / 4;
        const double scale  = std::min({1.0, std::sqrt(static_cast<double>(pixels) / (static_cast<double>(sourceW) * sourceH)),
                                        static_cast<double>(dimensionLimit) / sourceW, static_cast<double>(dimensionLimit) / sourceH});
        int          pixelW = std::max(1, static_cast<int>(std::floor(sourceW * scale)));
        int          pixelH = std::max(1, static_cast<int>(std::floor(sourceH * scale)));
        // The minimum one-pixel dimension and floating-point rounding must never
        // let an extreme aspect ratio exceed the byte budget.
        if (static_cast<uint64_t>(pixelW) * pixelH > pixels) {
            if (pixelW >= pixelH)
                pixelW = static_cast<int>(pixels / pixelH);
            else
                pixelH = static_cast<int>(pixels / pixelW);
        }
        if (pixelW < 1 || pixelH < 1)
            return std::nullopt;
        return SCaptureGeometry{sourceW, sourceH, pixelW, pixelH};
    }

    // Native surface clipping remains in source pixels. Scale only at the GL
    // scissor boundary, rounding outward so fractional edges retain coverage.
    inline SBoxF capturePixelClip(const SCaptureGeometry& geometry, const SBoxF& source) {
        if (geometry.sourceW < 1 || geometry.sourceH < 1 || geometry.pixelW < 1 || geometry.pixelH < 1 || !std::isfinite(source.x) || !std::isfinite(source.y) ||
            !std::isfinite(source.w) || !std::isfinite(source.h) || source.w <= 0 || source.h <= 0)
            return {};
        const double right = source.x + source.w, bottom = source.y + source.h;
        if (!std::isfinite(right) || !std::isfinite(bottom))
            return {};
        const double sourceLeft   = std::clamp(source.x, 0.0, static_cast<double>(geometry.sourceW));
        const double sourceTop    = std::clamp(source.y, 0.0, static_cast<double>(geometry.sourceH));
        const double sourceRight  = std::clamp(right, 0.0, static_cast<double>(geometry.sourceW));
        const double sourceBottom = std::clamp(bottom, 0.0, static_cast<double>(geometry.sourceH));
        if (sourceRight <= sourceLeft || sourceBottom <= sourceTop)
            return {};
        const double left = std::floor(sourceLeft * geometry.scaleX());
        const double top  = std::floor(sourceTop * geometry.scaleY());
        const double endX = std::min(static_cast<double>(geometry.pixelW), std::ceil(sourceRight * geometry.scaleX()));
        const double endY = std::min(static_cast<double>(geometry.pixelH), std::ceil(sourceBottom * geometry.scaleY()));
        return {left, top, std::max(0.0, endX - left), std::max(0.0, endY - top)};
    }

} // namespace hyprspace
