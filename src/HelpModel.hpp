#pragma once

#include "Geometry.hpp"

#include <glib.h>
#include <array>
#include <utility>

namespace hyprspace {
    inline constexpr std::array<const char*, 6> HELP_TOPICS{"All help", "Navigation", "Zoom & follow", "Window board", "Window gestures", "Settings"};

    struct SHelpEntry {
        std::string id, title, keys, description, tags, setting;
        size_t      topic = 1;
    };

    inline std::string foldHelpSearch(const std::string& text) {
        auto*       valid  = g_utf8_make_valid(text.data(), text.size());
        auto*       folded = g_utf8_casefold(valid, -1);
        std::string result{folded};
        g_free(folded);
        g_free(valid);
        return result;
    }

    class CHelpBrowser {
      public:
        void setEntries(std::vector<SHelpEntry> entries) {
            const auto selected = selection() ? selection()->id : "";
            m_entries           = std::move(entries);
            m_search.clear();
            m_search.reserve(m_entries.size());
            for (const auto& entry : m_entries)
                m_search.push_back(
                    {foldHelpSearch(std::string(HELP_TOPICS.at(entry.topic)) + " " + entry.title + " " + entry.keys + " " + entry.description + " " + entry.tags + " " +
                                    entry.setting +
                                    (entry.keys.empty() || entry.topic == 5 ? "" : " keys keybind keybinds keybinding keybindings bindings shortcuts controls keyboard")),
                     foldHelpSearch(entry.title), foldHelpSearch(entry.tags)});
            filter(selected);
        }
        void setQuery(std::string query) {
            m_query = std::move(query);
            filter();
        }
        void setTopic(size_t topic) {
            m_topic = std::min(topic, HELP_TOPICS.size() - 1);
            m_query.clear();
            filter();
        }
        void move(int delta) {
            if (!m_results.empty())
                m_selected = static_cast<size_t>(std::clamp(static_cast<long>(m_selected) + delta, 0L, static_cast<long>(m_results.size() - 1)));
        }
        void edge(bool last) {
            m_selected = last && !m_results.empty() ? m_results.size() - 1 : 0;
        }
        void select(size_t row) {
            if (row < m_results.size())
                m_selected = row;
        }
        const SHelpEntry* selection() const {
            return m_results.empty() ? nullptr : &m_entries[m_results[m_selected]];
        }
        const SHelpEntry& entry(size_t row) const {
            return m_entries.at(m_results.at(row));
        }
        size_t selected() const {
            return m_selected;
        }
        size_t topic() const {
            return m_topic;
        }
        size_t count() const {
            return m_results.size();
        }
        size_t total() const {
            return m_entries.size();
        }
        const std::string& query() const {
            return m_query;
        }

      private:
        void filter(const std::string& selected = "") {
            const auto               query = foldHelpSearch(m_query);
            std::vector<std::string> tokens;
            std::string              token;
            for (const char* p = query.c_str(); *p; p = g_utf8_next_char(p)) {
                if (g_unichar_isspace(g_utf8_get_char(p))) {
                    if (!token.empty())
                        tokens.push_back(std::exchange(token, {}));
                } else
                    token.append(p, g_utf8_next_char(p) - p);
            }
            if (!token.empty())
                tokens.push_back(std::move(token));
            m_results.clear();
            std::array<std::vector<size_t>, 5> ranked;
            for (size_t i = 0; i < m_entries.size(); ++i)
                if ((tokens.empty() ? m_topic == 0 || m_entries[i].topic == m_topic : true) &&
                    std::ranges::all_of(tokens, [&](const auto& token) { return m_search[i].haystack.find(token) != std::string::npos; })) {
                    if (tokens.empty())
                        m_results.push_back(i);
                    else {
                        const auto&  cached = m_search[i];
                        const size_t score  = cached.title == query                           ? 0
                                              : cached.title.starts_with(query)               ? 1
                                              : cached.title.find(query) != std::string::npos ? 2
                                              : cached.tags.find(query) != std::string::npos  ? 3
                                                                                              : 4;
                        ranked[score].push_back(i);
                    }
                }
            // Fixed rank buckets retain catalog order for ties without sorting
            // or repeatedly folding the same text during comparisons.
            for (const auto& bucket : ranked)
                m_results.insert(m_results.end(), bucket.begin(), bucket.end());
            m_selected = 0;
            for (size_t i = 0; i < m_results.size(); ++i)
                if (m_entries[m_results[i]].id == selected) {
                    m_selected = i;
                    break;
                }
        }
        struct SSearchEntry {
            std::string haystack, title, tags;
        };
        std::vector<SHelpEntry>   m_entries;
        std::vector<SSearchEntry> m_search;
        std::vector<size_t>       m_results;
        std::string               m_query;
        size_t                    m_topic = 0, m_selected = 0;
    };

    struct SHelpLayout {
        SBoxF  panel, search, topics, heading, results, detail, footer, close;
        double rowHeight = 48;
    };

    inline SHelpLayout helpLayout(const SBoxF& usable, double lineHeight = 22) {
        SHelpLayout  out;
        const double inset = std::min({24.0, usable.w / 20, usable.h / 20});
        const double w     = std::max(1.0, std::min(1080.0, usable.w - inset * 2));
        const double h     = std::max(1.0, std::min(760.0, usable.h - inset * 2));
        out.panel          = {usable.cx() - w / 2, usable.cy() - h / 2, w, h};
        const double pad   = std::min({24.0, w / 12, h / 24});
        const double x = out.panel.x + pad, y = out.panel.y + pad;
        const double sidebar      = w >= 720 ? 176 : 0;
        out.search                = {x, y + (h < 480 ? 36 : 46), w - pad * 2, std::max(36.0, lineHeight + 16)};
        out.close                 = {out.panel.x + w - pad - 60, y, 60, 30};
        const double top          = out.search.y + out.search.h + (h < 480 ? 12 : 20);
        const double contentX     = x + (sidebar ? sidebar + 24 : 0);
        const double contentW     = std::max(1.0, w - pad * 2 - (sidebar ? sidebar + 24 : 0));
        const double headingH     = h < 420 ? 0 : 24;
        out.heading               = {contentX, top, contentW, headingH};
        out.footer                = {x, out.panel.y + h - pad - 22, w - pad * 2, 22};
        out.rowHeight             = std::max(44.0, lineHeight + 24);
        const double resultsTop   = top + (headingH ? 32 : 0);
        const double detailBudget = h < 420 ? std::max(1.0, out.footer.y - resultsTop - out.rowHeight - 32) : h * .32;
        const double detailH      = std::min(detailBudget, std::max(128.0, lineHeight * (contentW < 480 ? 7 : 4) + 50));
        out.detail                = {contentX, out.footer.y - detailH - 16, contentW, detailH};
        out.results               = {contentX, resultsTop, contentW, std::max(1.0, out.detail.y - resultsTop - 16)};
        out.topics                = {x, top, sidebar, std::max(1.0, out.detail.y + detailH - top)};
        return out;
    }
} // namespace hyprspace
