#include "Texture.hpp"

#include "DesktopDb.hpp"
#include "Config.hpp"

#include <hyprland/src/render/gl/GLTexture.hpp>

#include <drm_fourcc.h>
#include <format>

namespace hyprspace {

    SP<Render::ITexture> uploadImage(const SImage& img) {
        if (!img.ok())
            return nullptr;

        // Cairo ARGB32 and DRM_FORMAT_ARGB8888 are the same byte layout on
        // little-endian hosts, and both are premultiplied.
        return makeShared<Render::GL::CGLTexture>(DRM_FORMAT_ARGB8888, const_cast<uint8_t*>(img.data.data()), static_cast<uint32_t>(img.stride),
                                                  Vector2D{static_cast<double>(img.w), static_cast<double>(img.h)});
    }

    static SRgba toRgba(const CHyprColor& c) {
        return SRgba{c.r, c.g, c.b, c.a};
    }

    SP<Render::ITexture> CTextureCache::text(const std::string& str, const std::string& font, const CHyprColor& color, int maxWidth, double scale) {
        if (str.empty())
            return nullptr;

        const auto key = std::format("t|{}|{}|{:08x}|{}|{:.3f}", str, font, color.getAsHex(), maxWidth, scale);
        if (auto it = m_cache.find(key); it != m_cache.end())
            return it->second;

        auto tex     = uploadImage(renderText(str, font, toRgba(color), maxWidth, scale));
        m_cache[key] = tex;
        return tex;
    }

    static CDesktopDb& desktopDb() {
        static CDesktopDb db;
        static bool       scanned = false;
        if (!scanned) {
            db.scan();
            scanned = true;
        }
        return db;
    }

    SP<Render::ITexture> CTextureCache::icon(const std::string& windowClass, int size) {
        const auto key = std::format("i|{}|{}", windowClass, size);
        if (auto it = m_cache.find(key); it != m_cache.end())
            return it->second;

        SImage img;

        auto& db = desktopDb();

        // A .desktop entry is the most reliable source; its Icon= may even be an
        // absolute path, which is what Chromium web-app entries use.
        if (const auto name = db.iconNameForClass(windowClass); !name.empty()) {
            if (auto path = db.resolveIconPath(name, size))
                img = loadIcon(*path, size);
        }

        // Otherwise the class (or a normalised form of it) is often itself a
        // valid icon theme name.
        if (!img.ok()) {
            for (const auto& cand : classCandidates(windowClass)) {
                if (auto path = db.resolveIconPath(cand, size)) {
                    img = loadIcon(*path, size);
                    if (img.ok())
                        break;
                }
            }
        }

        if (!img.ok()) {
            // Last resort: a tinted rounded square with the app's initial, which
            // still reads better in a switcher than an empty slot.
            std::string initial = windowClass.empty() ? "?" : windowClass.substr(0, 1);
            for (auto& c : initial)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            img = placeholderIcon(initial, std::format("Sans Bold {}", std::max(8, size / 2)), size, toRgba(config::switcherTextColor()),
                                  toRgba(config::switcherHighlightColor()));
        }

        auto tex     = uploadImage(img);
        m_cache[key] = tex;
        return tex;
    }

    void CTextureCache::clear() {
        m_cache.clear();
    }

    CTextureCache& textures() {
        static CTextureCache cache;
        return cache;
    }

} // namespace hyprspace
