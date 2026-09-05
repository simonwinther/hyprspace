// hyprspace - upload CPU-rasterised images into Hyprland textures, with caching.

#pragma once

#include "ImageCache.hpp"
#include "globals.hpp"

#include <hyprland/src/render/Texture.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hyprspace {

    // Requires a current EGL context.
    SP<Render::ITexture> uploadImage(const SImage& img);

    // Text and icon textures keyed by content; cleared when the overlay closes so
    // that GPU memory is not held while idle.
    class CTextureCache {
      public:
        struct SIconTexture {
            SP<Render::ITexture> texture;
            bool                 pending = false;
        };

        // maxWidth is logical px; `scale` is the output device scale, so the
        // glyphs are rasterised at native resolution. The returned texture's
        // m_size is therefore in physical px.
        SP<Render::ITexture> text(const std::string& str, const std::string& font, const CHyprColor& color, int maxWidth = 0, double scale = 1.0);

        // `size` is in physical px.
        SIconTexture icon(const std::string& windowClass, int size);

        void   clear();
        void   invalidate(); // also discard decoded icons after config/theme changes
        size_t size() const {
            return m_cache.size();
        }

      private:
        std::unordered_map<std::string, SP<Render::ITexture>> m_cache;
        std::unordered_set<std::string>                       m_provisionalIcons;
        CImageCache                                           m_iconImages;
    };

    CTextureCache& textures();

    // Desktop files and icon themes are filesystem work, so prepare them away
    // from Hyprland's compositor thread. `finishIconDiscovery` joins that work
    // before the plugin is unloaded.
    void startIconDiscovery();
    void finishIconDiscovery();

} // namespace hyprspace
