#include "Texture.hpp"

#include "DesktopDb.hpp"
#include "Config.hpp"

#include <hyprland/src/render/gl/GLTexture.hpp>

#include <drm_fourcc.h>
#include <chrono>
#include <format>
#include <future>
#include <optional>

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

    namespace {
        struct SDesktopDbState {
            std::optional<CDesktopDb> db;
            std::future<CDesktopDb>   pending;
        };

        SDesktopDbState& desktopDbState() {
            static SDesktopDbState state;
            return state;
        }

        const CDesktopDb* desktopDbIfReady() {
            auto& state = desktopDbState();
            if (state.db)
                return &*state.db;

            startIconDiscovery();
            if (!state.pending.valid() || state.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return nullptr;

            try {
                state.db.emplace(state.pending.get());
            } catch (...) {
                // Discovery failure must degrade to placeholders, never make the
                // compositor retry forever or let an exception escape rendering.
                state.db.emplace();
            }

            return &*state.db;
        }

        SImage placeholderFor(const std::string& windowClass, int size) {
            std::string initial = windowClass.empty() ? "?" : windowClass.substr(0, 1);
            for (auto& c : initial)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            return placeholderIcon(initial, std::format("Sans Bold {}", std::max(8, size / 2)), size, toRgba(config::switcherTextColor()),
                                   toRgba(config::switcherHighlightColor()));
        }
    } // namespace

    void startIconDiscovery() {
        auto& state = desktopDbState();
        if (state.db || state.pending.valid())
            return;

        try {
            state.pending = std::async(std::launch::async, [] {
                CDesktopDb db;
                db.scan();
                return db;
            });
        } catch (...) {
            state.db.emplace();
        }
    }

    void finishIconDiscovery() {
        auto& state = desktopDbState();
        if (state.db || !state.pending.valid())
            return;

        try {
            state.db.emplace(state.pending.get());
        } catch (...) {
            state.db.emplace();
        }
    }

    CTextureCache::SIconTexture CTextureCache::icon(const std::string& windowClass, int size) {
        const auto key    = std::format("i|{}|{}", windowClass, size);
        const auto cached = m_cache.find(key);
        const auto db     = desktopDbIfReady();

        const bool PROVISIONAL = m_provisionalIcons.contains(key);
        if (cached != m_cache.end() && (!PROVISIONAL || !db))
            return {cached->second, PROVISIONAL};

        if (const auto* img = m_iconImages.get(key)) {
            auto tex     = uploadImage(*img);
            m_cache[key] = tex;
            m_provisionalIcons.erase(key);
            return {tex, false};
        }

        if (!db) {
            auto tex     = uploadImage(placeholderFor(windowClass, size));
            m_cache[key] = tex;
            m_provisionalIcons.insert(key);
            return {tex, true};
        }

        SImage img;

        // A .desktop entry is the most reliable source; its Icon= may even be an
        // absolute path, which is what Chromium web-app entries use.
        if (const auto name = db->iconNameForClass(windowClass); !name.empty()) {
            if (auto path = db->resolveIconPath(name, size))
                img = loadIcon(*path, size);
        }

        // Otherwise the class (or a normalised form of it) is often itself a
        // valid icon theme name.
        if (!img.ok()) {
            for (const auto& cand : classCandidates(windowClass)) {
                if (auto path = db->resolveIconPath(cand, size)) {
                    img = loadIcon(*path, size);
                    if (img.ok())
                        break;
                }
            }
        }

        const bool RESOLVED = img.ok();
        if (!RESOLVED) {
            // Last resort: a tinted rounded square with the app's initial, which
            // still reads better in a switcher than an empty slot.
            img = placeholderFor(windowClass, size);
        }

        auto tex = uploadImage(img);
        if (RESOLVED)
            m_iconImages.put(key, std::move(img));
        m_cache[key] = tex;
        m_provisionalIcons.erase(key);
        return {tex, false};
    }

    void CTextureCache::clear() {
        m_cache.clear();
        m_provisionalIcons.clear();
    }

    void CTextureCache::invalidate() {
        clear();
        m_iconImages.clear();
    }

    CTextureCache& textures() {
        static CTextureCache cache;
        return cache;
    }

} // namespace hyprspace
