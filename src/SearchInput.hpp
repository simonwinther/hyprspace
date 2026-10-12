#pragma once

#include <glib.h>

#include <algorithm>
#include <string>
#include <utility>

namespace hyprspace {

    class CSearchInput {
      public:
        static constexpr size_t MAX_BYTES = 1024;

        const std::string& text() const {
            return m_text;
        }
        size_t cursor() const {
            return m_cursor;
        }
        std::pair<size_t, size_t> selection() const {
            return std::minmax(m_cursor, m_anchor);
        }
        void clear() {
            m_text.clear();
            m_cursor = m_anchor = 0;
        }
        void setCursor(size_t position, bool extend = false) {
            m_cursor = std::min(position, m_text.size());
            while (m_cursor && m_cursor < m_text.size() && (static_cast<unsigned char>(m_text[m_cursor]) & 0xc0) == 0x80)
                --m_cursor;
            if (!extend)
                m_anchor = m_cursor;
        }
        void home(bool extend = false) {
            setCursor(0, extend);
        }
        void end(bool extend = false) {
            setCursor(m_text.size(), extend);
        }
        void selectAll() {
            m_anchor = 0;
            m_cursor = m_text.size();
        }
        void left(bool extend = false) {
            const auto [from, to] = selection();
            if (!extend && from != to)
                setCursor(from);
            else if (m_cursor)
                setCursor(g_utf8_find_prev_char(m_text.data(), m_text.data() + m_cursor) - m_text.data(), extend);
        }
        void right(bool extend = false) {
            const auto [from, to] = selection();
            if (!extend && from != to)
                setCursor(to);
            else if (m_cursor < m_text.size())
                setCursor(g_utf8_next_char(m_text.data() + m_cursor) - m_text.data(), extend);
        }
        bool insert(const std::string& text) {
            const auto [from, to] = selection();
            if (text.empty() || !g_utf8_validate(text.data(), text.size(), nullptr) || text.size() > MAX_BYTES - (m_text.size() - (to - from)))
                return false;
            m_text.replace(from, to - from, text);
            setCursor(from + text.size());
            return true;
        }
        bool backspace() {
            if (eraseSelection())
                return true;
            if (!m_cursor)
                return false;
            const auto previous = g_utf8_find_prev_char(m_text.data(), m_text.data() + m_cursor) - m_text.data();
            m_text.erase(previous, m_cursor - previous);
            setCursor(previous);
            return true;
        }
        bool eraseForward() {
            if (eraseSelection())
                return true;
            if (m_cursor == m_text.size())
                return false;
            const auto next = g_utf8_next_char(m_text.data() + m_cursor) - m_text.data();
            m_text.erase(m_cursor, next - m_cursor);
            return true;
        }

      private:
        bool eraseSelection() {
            const auto [from, to] = selection();
            if (from == to)
                return false;
            m_text.erase(from, to - from);
            setCursor(from);
            return true;
        }

        std::string m_text;
        size_t      m_cursor = 0, m_anchor = 0;
    };

} // namespace hyprspace
