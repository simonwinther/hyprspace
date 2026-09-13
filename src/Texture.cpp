#include "Texture.hpp"

#include "DesktopDb.hpp"
#include "Config.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <drm_fourcc.h>
#include <chrono>
#include <format>
#include <future>
#include <optional>

namespace hyprspace {

    namespace {
        CTextureCache::SResources resourceCounts;
        struct SResidentTexture {
            WP<Render::ITexture> texture;
            size_t               bytes;
        };
        std::vector<SResidentTexture> residents;
        void                          pruneTextures() {
            std::erase_if(residents, [](const auto& entry) { return entry.texture.expired(); });
            resourceCounts.bytes = 0;
            for (const auto& entry : residents)
                resourceCounts.bytes += entry.bytes;
        }
    } // namespace

    const CTextureCache::SResources& CTextureCache::resources() {
        pruneTextures();
        return resourceCounts;
    }

    SP<Render::ITexture> uploadImage(const SImage& img) {
        pruneTextures();
        if (residents.size() >= CTextureCache::SResources::MAX_ENTRIES || !img.ok() ||
            static_cast<size_t>(img.w) * img.h * 4 > CTextureCache::SResources::MAX_BYTES - resourceCounts.bytes)
            return nullptr;

        // Cairo ARGB32 and DRM_FORMAT_ARGB8888 are the same byte layout on
        // little-endian hosts, and both are premultiplied.
        // The compositor owns both the texture's virtual methods and shared
        // pointer deleter. A retained pass may release it after plugin unload.
        auto texture = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, const_cast<uint8_t*>(img.data.data()), static_cast<uint32_t>(img.stride),
                                                      Vector2D{static_cast<double>(img.w), static_cast<double>(img.h)});
        if (!texture)
            return nullptr;
        // ok() only checks the GL name in this ABI, not successful storage.
        GLint previous = 0, width = 0, height = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        texture->bind();
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glBindTexture(GL_TEXTURE_2D, previous);
        const auto error = glGetError();
        if (width != img.w || height != img.h || error != GL_NO_ERROR)
            return nullptr;
        residents.push_back({texture, static_cast<size_t>(img.w) * img.h * 4});
        pruneTextures();
        resourceCounts.peakBytes = std::max(resourceCounts.peakBytes, resourceCounts.bytes);
        return texture;
    }

    SP<Render::ITexture> CTextureCache::store(const std::string& key, const SImage& image, bool provisional) {
        const size_t bytes = image.ok() ? static_cast<size_t>(image.w) * image.h * 4 : 0;
        if (std::chrono::steady_clock::now() < m_retryAfter)
            return nullptr;
        if (!bytes || bytes > SResources::MAX_BYTES) {
            ++resourceCounts.failures;
            m_retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return nullptr;
        }
        m_cache.erase(key);
        pruneTextures();
        while (!m_cache.empty() &&
               (m_cache.size() >= SResources::MAX_ENTRIES || residents.size() >= SResources::MAX_ENTRIES || resourceCounts.bytes > SResources::MAX_BYTES - bytes)) {
            const auto oldest = std::ranges::min_element(m_cache, {}, [](const auto& entry) { return entry.second.used; });
            m_cache.erase(oldest);
            ++resourceCounts.evictions;
            pruneTextures();
        }
        // Pass elements own shared references. Eviction releases only our
        // reference; live bytes include those leases until the frame ends.
        SP<Render::ITexture> texture;
        try {
            texture = uploadImage(image);
        } catch (const std::bad_alloc&) {
        }
        if (!texture) {
            ++resourceCounts.failures;
            m_retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return nullptr;
        }
        m_cache.emplace(key, SEntry{texture, ++m_sequence, provisional});
        resourceCounts.peakEntries = std::max(resourceCounts.peakEntries, m_cache.size());
        return texture;
    }

    static SRgba toRgba(const CHyprColor& c) {
        return SRgba{c.r, c.g, c.b, c.a};
    }

    SP<Render::ITexture> CTextureCache::text(const std::string& str, const std::string& font, const CHyprColor& color, int maxWidth, double scale) {
        if (str.empty())
            return nullptr;

        const auto text = boundedText(str);
        const auto key  = std::format("t|{}|{}|{:08x}|{}|{:.3f}", text, boundedText(font), color.getAsHex(), maxWidth, scale);
        if (auto it = m_cache.find(key); it != m_cache.end()) {
            it->second.used = ++m_sequence;
            ++resourceCounts.hits;
            return it->second.texture;
        }
        ++resourceCounts.misses;
        if (std::chrono::steady_clock::now() < m_retryAfter)
            return nullptr;

        return store(key, renderText(text, font, toRgba(color), maxWidth, scale));
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
        const auto key    = std::format("i|{}|{}", boundedText(windowClass), size);
        const auto cached = m_cache.find(key);
        const auto db     = desktopDbIfReady();

        const bool PROVISIONAL = cached != m_cache.end() && cached->second.provisional;
        if (cached != m_cache.end() && (!PROVISIONAL || !db)) {
            cached->second.used = ++m_sequence;
            ++resourceCounts.hits;
            return {cached->second.texture, PROVISIONAL};
        }
        ++resourceCounts.misses;
        if (std::chrono::steady_clock::now() < m_retryAfter)
            return {};

        if (const auto* img = m_iconImages.get(key)) {
            return {store(key, *img), false};
        }

        if (!db) {
            return {store(key, placeholderFor(windowClass, size), true), true};
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

        auto tex = store(key, img);
        if (RESOLVED)
            m_iconImages.put(key, std::move(img));
        return {tex, false};
    }

    void CTextureCache::clear() {
        m_cache.clear();
        pruneTextures();
        m_sequence   = 0;
        m_retryAfter = {};
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
