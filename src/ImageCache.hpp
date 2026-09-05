#pragma once

#include "Raster.hpp"

#include <algorithm>
#include <cstddef>
#include <list>
#include <string>
#include <utility>

namespace hyprspace {

    // Decoded pixels can survive overlay teardown without retaining GL objects.
    // Both limits apply: unusually large icons cannot displace the whole cache,
    // and tiny icons cannot accumulate an unbounded number of entries.
    class CImageCache {
      public:
        explicit CImageCache(size_t maxBytes = 16 * 1024 * 1024, size_t maxEntries = 128) : m_maxBytes(maxBytes), m_maxEntries(maxEntries) {}

        CImageCache(const CImageCache&)            = delete;
        CImageCache& operator=(const CImageCache&) = delete;

        const SImage* get(const std::string& key) {
            const auto entry = std::ranges::find(m_entries, key, &SEntry::key);
            if (entry == m_entries.end())
                return nullptr;

            m_entries.splice(m_entries.begin(), m_entries, entry);
            return &m_entries.front().image;
        }

        void put(std::string key, SImage image) {
            const size_t bytes = image.data.capacity();
            if (!image.ok() || bytes > m_maxBytes || m_maxEntries == 0)
                return;

            if (const auto existing = std::ranges::find(m_entries, key, &SEntry::key); existing != m_entries.end()) {
                m_bytes -= existing->image.data.capacity();
                m_entries.erase(existing);
            }

            while (!m_entries.empty() && (m_bytes > m_maxBytes - bytes || m_entries.size() >= m_maxEntries)) {
                m_bytes -= m_entries.back().image.data.capacity();
                m_entries.pop_back();
            }

            m_entries.push_front(SEntry{std::move(key), std::move(image)});
            m_bytes += bytes;
        }

        void clear() {
            m_entries.clear();
            m_bytes = 0;
        }

        size_t bytes() const {
            return m_bytes;
        }
        size_t size() const {
            return m_entries.size();
        }

      private:
        struct SEntry {
            std::string key;
            SImage      image;
        };

        size_t            m_maxBytes;
        size_t            m_maxEntries;
        size_t            m_bytes = 0;
        std::list<SEntry> m_entries;
    };

} // namespace hyprspace
