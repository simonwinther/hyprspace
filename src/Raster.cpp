#include "Raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>

namespace hyprspace {

    static SImage fromSurface(cairo_surface_t* surf) {
        SImage out;
        if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
            return out;

        cairo_surface_flush(surf);

        out.w      = cairo_image_surface_get_width(surf);
        out.h      = cairo_image_surface_get_height(surf);
        out.stride = cairo_image_surface_get_stride(surf);

        const auto* src = cairo_image_surface_get_data(surf);
        if (!src || out.w <= 0 || out.h <= 0) {
            out = {};
            return out;
        }

        out.data.assign(src, src + static_cast<size_t>(out.stride) * out.h);
        return out;
    }

    static PangoLayout* makeLayout(cairo_t* cr, const std::string& text, const std::string& font, int maxWidth, double scale = 1.0) {
        PangoLayout* layout = pango_cairo_create_layout(cr);

        PangoFontDescription* desc = pango_font_description_from_string(font.c_str());

        // Scale the point size rather than the cairo matrix so hinting and
        // metrics are computed at the real output resolution.
        if (scale != 1.0) {
            const int size = pango_font_description_get_size(desc);
            if (size > 0) {
                if (pango_font_description_get_size_is_absolute(desc))
                    pango_font_description_set_absolute_size(desc, size * scale);
                else
                    pango_font_description_set_size(desc, static_cast<gint>(size * scale));
            }
        }

        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        pango_layout_set_text(layout, text.c_str(), -1);
        pango_layout_set_single_paragraph_mode(layout, TRUE);

        if (maxWidth > 0) {
            pango_layout_set_width(layout, static_cast<int>(maxWidth * scale) * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }

        return layout;
    }

    void measureText(const std::string& text, const std::string& font, int& outW, int& outH, double scale) {
        outW = outH = 0;

        cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t*         cr  = cairo_create(tmp);

        PangoLayout* layout = makeLayout(cr, text, font, 0, scale);
        pango_layout_get_pixel_size(layout, &outW, &outH);

        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_destroy(tmp);
    }

    SImage renderText(const std::string& text, const std::string& font, const SRgba& color, int maxWidth, double scale) {
        if (text.empty())
            return {};

        int w = 0, h = 0;
        {
            cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t*         cr  = cairo_create(tmp);
            PangoLayout*     l   = makeLayout(cr, text, font, maxWidth, scale);
            pango_layout_get_pixel_size(l, &w, &h);
            g_object_unref(l);
            cairo_destroy(cr);
            cairo_surface_destroy(tmp);
        }

        if (w <= 0 || h <= 0)
            return {};

        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t*         cr   = cairo_create(surf);

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        PangoLayout* layout = makeLayout(cr, text, font, maxWidth, scale);
        cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        cairo_destroy(cr);
        SImage out = fromSurface(surf);
        cairo_surface_destroy(surf);
        return out;
    }

    static SImage loadSvg(const std::string& path, int size) {
        GError*     err    = nullptr;
        RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), &err);
        if (!handle) {
            if (err)
                g_error_free(err);
            return {};
        }

        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t*         cr   = cairo_create(surf);

        const RsvgRectangle viewport = {.x = 0.0, .y = 0.0, .width = static_cast<double>(size), .height = static_cast<double>(size)};

        gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, &err);
        if (!ok && err)
            g_error_free(err);

        cairo_destroy(cr);
        g_object_unref(handle);

        SImage out;
        if (ok)
            out = fromSurface(surf);
        cairo_surface_destroy(surf);
        return out;
    }

    // gdk-pixbuf hands back straight (non-premultiplied) RGBA; cairo/GL want
    // premultiplied BGRA. Convert directly rather than pulling in all of GDK just
    // for gdk_cairo_set_source_pixbuf().
    static SImage loadPixbuf(const std::string& path, int size) {
        GError*    err = nullptr;
        GdkPixbuf* pb  = gdk_pixbuf_new_from_file_at_scale(path.c_str(), size, size, TRUE, &err);
        if (!pb) {
            if (err)
                g_error_free(err);
            return {};
        }

        const int            pw       = gdk_pixbuf_get_width(pb);
        const int            ph       = gdk_pixbuf_get_height(pb);
        const int            srcStr   = gdk_pixbuf_get_rowstride(pb);
        const int            channels = gdk_pixbuf_get_n_channels(pb);
        const bool           hasAlpha = gdk_pixbuf_get_has_alpha(pb);
        const unsigned char* src      = gdk_pixbuf_get_pixels(pb);

        SImage out;
        out.w      = size;
        out.h      = size;
        out.stride = size * 4;
        out.data.assign(static_cast<size_t>(out.stride) * size, 0);

        const int offX = (size - pw) / 2;
        const int offY = (size - ph) / 2;

        for (int y = 0; y < ph; ++y) {
            const int dy = y + offY;
            if (dy < 0 || dy >= size)
                continue;

            for (int x = 0; x < pw; ++x) {
                const int dx = x + offX;
                if (dx < 0 || dx >= size)
                    continue;

                const unsigned char* s = src + static_cast<size_t>(y) * srcStr + static_cast<size_t>(x) * channels;
                const uint8_t        r = s[0], g = s[1], b = s[2];
                const uint8_t        a = hasAlpha ? s[3] : 255;

                uint8_t* d = out.data.data() + static_cast<size_t>(dy) * out.stride + static_cast<size_t>(dx) * 4;
                d[0]       = static_cast<uint8_t>(b * a / 255);
                d[1]       = static_cast<uint8_t>(g * a / 255);
                d[2]       = static_cast<uint8_t>(r * a / 255);
                d[3]       = a;
            }
        }

        g_object_unref(pb);
        return out;
    }

    SImage loadIcon(const std::string& path, int size) {
        if (path.empty() || size <= 0)
            return {};

        const auto dot = path.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : path.substr(dot);
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".svg" || ext == ".svgz")
            return loadSvg(path, size);

        return loadPixbuf(path, size);
    }

    static void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
        constexpr double PI = 3.14159265358979323846;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2);
        cairo_arc(cr, x + r, y + h - r, r, PI / 2, PI);
        cairo_arc(cr, x + r, y + r, r, PI, 3 * PI / 2);
        cairo_close_path(cr);
    }

    SImage placeholderIcon(const std::string& letters, const std::string& font, int size, const SRgba& fg, const SRgba& bg) {
        if (size <= 0)
            return {};

        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t*         cr   = cairo_create(surf);

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
        roundedRect(cr, 0, 0, size, size, size * 0.22);
        cairo_fill(cr);

        if (!letters.empty()) {
            PangoLayout* layout = makeLayout(cr, letters, font, 0);
            int          tw = 0, th = 0;
            pango_layout_get_pixel_size(layout, &tw, &th);
            cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, fg.a);
            cairo_move_to(cr, (size - tw) / 2.0, (size - th) / 2.0);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }

        cairo_destroy(cr);
        SImage out = fromSurface(surf);
        cairo_surface_destroy(surf);
        return out;
    }

} // namespace hyprspace
