#pragma once

#include "Geometry.hpp"

#include <glib.h>
#include <array>
#include <sstream>
#include <numeric>

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
            m_haystacks.clear();
            for (const auto& entry : m_entries)
                m_haystacks.push_back(foldHelpSearch(
                    std::string(HELP_TOPICS.at(entry.topic)) + " " + entry.title + " " + entry.keys + " " + entry.description + " " + entry.tags + " " + entry.setting +
                    (entry.keys.empty() || entry.topic == 5 ? "" : " keys keybind keybinds keybinding keybindings bindings shortcuts controls keyboard")));
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
            std::istringstream       words(query);
            std::vector<std::string> tokens;
            for (std::string token; words >> token;)
                tokens.push_back(std::move(token));
            m_results.clear();
            for (size_t i = 0; i < m_entries.size(); ++i)
                if ((tokens.empty() ? m_topic == 0 || m_entries[i].topic == m_topic : true) &&
                    std::ranges::all_of(tokens, [&](const auto& token) { return m_haystacks[i].find(token) != std::string::npos; }))
                    m_results.push_back(i);
            if (!tokens.empty()) {
                const auto score = [&](size_t i) {
                    const auto title = foldHelpSearch(m_entries[i].title);
                    const auto tags  = foldHelpSearch(m_entries[i].tags);
                    return title == query ? 0 : title.starts_with(query) ? 1 : title.find(query) != std::string::npos ? 2 : tags.find(query) != std::string::npos ? 3 : 4;
                };
                std::stable_sort(m_results.begin(), m_results.end(), [&](size_t a, size_t b) { return score(a) < score(b); });
            }
            m_selected = 0;
            for (size_t i = 0; i < m_results.size(); ++i)
                if (m_entries[m_results[i]].id == selected) {
                    m_selected = i;
                    break;
                }
        }
        std::vector<SHelpEntry>  m_entries;
        std::vector<std::string> m_haystacks;
        std::vector<size_t>      m_results;
        std::string              m_query;
        size_t                   m_topic = 0, m_selected = 0;
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
