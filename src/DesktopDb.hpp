// hyprspace - .desktop entry + icon theme resolution.
//
// No Hyprland dependencies: unit-testable on the host.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyprspace {

    struct SDesktopEntry {
        std::string name;    // Name=
        std::string icon;    // Icon=
        std::string wmClass; // StartupWMClass=
        std::string id;      // basename without .desktop
        bool        noDisplay = false;
    };

    // Parse a single .desktop file's [Desktop Entry] group.
    // Exposed for testing.
    SDesktopEntry parseDesktopEntry(const std::string& contents, const std::string& id = "");

    // Normalise a window class for matching: trimmed and lowercased.
    std::string normaliseClass(const std::string& cls);

    // Ordered lookup keys to try for a window class, most specific first.
    //
    // Handles reverse-DNS classes ("org.gnome.Nautilus" -> "nautilus") and the
    // Chromium/Edge web-app format ("chrome-chatgpt.com__-Default" ->
    // "chatgpt.com" -> "chatgpt"), which is what Omarchy's web apps report.
    // Exposed for testing.
    std::vector<std::string> classCandidates(const std::string& cls);

    class CDesktopDb {
      public:
        CDesktopDb() = default;

        // Scan XDG_DATA_DIRS + XDG_DATA_HOME for application entries.
        void scan();

        // Look up the icon *name* for a window class. Empty if unknown.
        std::string iconNameForClass(const std::string& cls) const;

        // Look up a human app name for a window class. Empty if unknown.
        std::string appNameForClass(const std::string& cls) const;

        // Resolve an icon name (or absolute path) to a file on disk.
        // `preferredSize` steers which themed size is picked.
        std::optional<std::string> resolveIconPath(const std::string& iconName, int preferredSize = 64) const;

        size_t entryCount() const {
            return m_entries.size();
        }

        // Directories searched for icons, in priority order. Populated by scan().
        const std::vector<std::string>& iconRoots() const {
            return m_iconRoots;
        }

        // Overridable for tests.
        void setIconRoots(std::vector<std::string> roots) {
            m_iconRoots = std::move(roots);
            m_iconCache.clear();
            m_iconIndex.clear();
            m_iconIndexReady = false;
        }
        void addEntry(const SDesktopEntry& e);

      private:
        struct SIconFile {
            std::string path;
            size_t      root     = 0;
            int         size     = 0;
            bool        scalable = false;
            bool        svg      = false;
        };

        void indexIcons() const;

        std::vector<SDesktopEntry>                                      m_entries;
        std::map<std::string, size_t>                                   m_byClass; // normalised class -> index
        std::vector<std::string>                                        m_iconRoots;
        mutable std::map<std::string, std::string>                      m_iconCache; // "name@size" -> path
        mutable std::unordered_map<std::string, std::vector<SIconFile>> m_iconIndex;
        mutable bool                                                    m_iconIndexReady = false;
    };

    // Helper: list of data dirs per the XDG basedir spec.
    std::vector<std::string> xdgDataDirs();

} // namespace hyprspace
