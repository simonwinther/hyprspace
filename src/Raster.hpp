// hyprspace - CPU rasterisation of text and icons into ARGB32 buffers.
//
// Depends on cairo/pango/gdk-pixbuf/librsvg but NOT on Hyprland, so it is
// unit-testable on the host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hyprspace {

    // Premultiplied ARGB32, little-endian byte order (B,G,R,A) — matches both
    // CAIRO_FORMAT_ARGB32 and DRM_FORMAT_ARGB8888 on LE hosts.
    struct SImage {
        int                  w      = 0;
        int                  h      = 0;
        int                  stride = 0;
        std::vector<uint8_t> data;

        bool ok() const {
            return w > 0 && h > 0 && stride > 0 && static_cast<size_t>(w) <= static_cast<size_t>(stride) / 4 &&
                   static_cast<size_t>(h) <= data.size() / static_cast<size_t>(stride);
        }
    };

    struct SRgba {
        double r = 1, g = 1, b = 1, a = 1;
    };

    // Render a single line of text, ellipsised at maxWidth logical px
    // (0 = no limit). `font` is a Pango font description string, e.g. "Sans 12".
    //
    // `scale` is the output's device scale: the glyphs are rasterised that much
    // larger so text stays sharp on HiDPI monitors. maxWidth stays logical.
    SImage renderText(const std::string& text, const std::string& font, const SRgba& color, int maxWidth = 0, double scale = 1.0);

    // Measure without rasterising.
    void measureText(const std::string& text, const std::string& font, int& outW, int& outH, double scale = 1.0);

    // Load an icon file (svg/svgz via librsvg, everything else via gdk-pixbuf)
    // scaled to fit size x size, centred, preserving aspect ratio.
    SImage loadIcon(const std::string& path, int size);

    // Flat rounded rectangle, used as a fallback when no icon can be resolved.
    SImage placeholderIcon(const std::string& letters, const std::string& font, int size, const SRgba& fg, const SRgba& bg);

} // namespace hyprspace
