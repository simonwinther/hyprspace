#include "DesktopDb.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hyprspace {

    static std::string trim(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static std::string toLower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string normaliseClass(const std::string& cls) {
        auto s = toLower(trim(cls));
        // Some toolkits report "org.gnome.Nautilus", others "nautilus". Keep both
        // forms available by indexing the full string; callers also try the tail.
        return s;
    }

    static std::string classTail(const std::string& normalised) {
        const auto dot = normalised.find_last_of('.');
        if (dot == std::string::npos || dot + 1 >= normalised.size())
            return "";
        return normalised.substr(dot + 1);
    }

    std::vector<std::string> classCandidates(const std::string& cls) {
        std::vector<std::string> out;

        auto push = [&out](const std::string& s) {
            if (!s.empty() && std::ranges::find(out, s) == out.end())
                out.push_back(s);
        };

        const auto base = normaliseClass(cls);
        if (base.empty())
            return out;

        push(base);

        // Chromium-family web apps: "chrome-<host><path>-<profile>".
        for (const auto* prefix : {"chrome-", "chromium-", "msedge-", "brave-"}) {
            if (!base.starts_with(prefix))
                continue;

            auto rest = base.substr(std::string_view(prefix).size());

            // Drop the trailing "-<profile>" segment ("-default", "-profile 1"...).
            if (const auto dash = rest.find_last_of('-'); dash != std::string::npos && dash > 0)
                rest = rest.substr(0, dash);

            // The path part is encoded with underscores; strip it.
            while (!rest.empty() && rest.back() == '_')
                rest.pop_back();
            if (const auto us = rest.find('_'); us != std::string::npos)
                rest = rest.substr(0, us);

            push(rest); // e.g. "chatgpt.com"

            // ...and without the TLD, which is how such entries are usually named.
            if (const auto dot = rest.find_last_of('.'); dot != std::string::npos && dot > 0)
                push(rest.substr(0, dot)); // e.g. "chatgpt"

            break;
        }

        // Reverse-DNS ids: "org.gnome.Nautilus" -> "nautilus".
        push(classTail(base));

        return out;
    }

    SDesktopEntry parseDesktopEntry(const std::string& contents, const std::string& id) {
        SDesktopEntry out;
        out.id = id;

        std::istringstream in(contents);
        std::string        line;
        bool               inGroup = false;

        while (std::getline(in, line)) {
            const auto t = trim(line);
            if (t.empty() || t[0] == '#')
                continue;

            if (t.front() == '[' && t.back() == ']') {
                inGroup = (t == "[Desktop Entry]");
                continue;
            }

            if (!inGroup)
                continue;

            const auto eq = t.find('=');
            if (eq == std::string::npos)
                continue;

            const auto key = trim(t.substr(0, eq));
            const auto val = trim(t.substr(eq + 1));

            // Ignore localised variants such as Name[de].
            if (key == "Name")
                out.name = val;
            else if (key == "Icon")
                out.icon = val;
            else if (key == "StartupWMClass")
                out.wmClass = val;
            else if (key == "NoDisplay")
                out.noDisplay = (toLower(val) == "true");
        }

        return out;
    }

    std::vector<std::string> xdgDataDirs() {
        std::vector<std::string> dirs;

        if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home)
            dirs.emplace_back(home);
        else if (const char* h = std::getenv("HOME"); h && *h)
            dirs.emplace_back(std::string(h) + "/.local/share");

        std::string sys = "/usr/local/share:/usr/share";
        if (const char* d = std::getenv("XDG_DATA_DIRS"); d && *d)
            sys = d;

        std::stringstream ss(sys);
        std::string       part;
        while (std::getline(ss, part, ':')) {
            if (!part.empty())
                dirs.push_back(part);
        }

        return dirs;
    }

    void CDesktopDb::addEntry(const SDesktopEntry& e) {
        if (e.icon.empty())
            return;

        m_entries.push_back(e);
        const size_t idx = m_entries.size() - 1;

        auto index = [&](const std::string& raw) {
            if (raw.empty())
                return;
            const auto k = normaliseClass(raw);
            if (k.empty())
                return;
            // First writer wins, except that an explicit StartupWMClass always
            // outranks a guess derived from the file name.
            m_byClass.insert_or_assign(k, idx);
        };

        // Weakest signal first so stronger ones overwrite.
        index(e.id);
        if (!e.name.empty())
            index(e.name);
        if (!e.wmClass.empty())
            index(e.wmClass);
    }

    void CDesktopDb::scan() {
        m_entries.clear();
        m_byClass.clear();
        m_iconCache.clear();
        m_iconRoots.clear();

        std::error_code ec;

        for (const auto& base : xdgDataDirs()) {
            const fs::path apps = fs::path(base) / "applications";
            if (fs::is_directory(apps, ec)) {
                for (auto it = fs::recursive_directory_iterator(apps, fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec)
                        break;
                    if (!it->is_regular_file(ec))
                        continue;
                    if (it->path().extension() != ".desktop")
                        continue;

                    std::ifstream f(it->path());
                    if (!f)
                        continue;

                    std::stringstream buf;
                    buf << f.rdbuf();

                    auto entry = parseDesktopEntry(buf.str(), it->path().stem().string());
                    addEntry(entry);
                }
            }

            const fs::path icons = fs::path(base) / "icons";
            if (fs::is_directory(icons, ec))
                m_iconRoots.push_back(icons.string());
        }

        if (fs::is_directory("/usr/share/pixmaps", ec))
            m_iconRoots.emplace_back("/usr/share/pixmaps");
    }

    std::string CDesktopDb::iconNameForClass(const std::string& cls) const {
        for (const auto& key : classCandidates(cls)) {
            if (auto it = m_byClass.find(key); it != m_byClass.end())
                return m_entries[it->second].icon;
        }
        return "";
    }

    std::string CDesktopDb::appNameForClass(const std::string& cls) const {
        for (const auto& key : classCandidates(cls)) {
            if (auto it = m_byClass.find(key); it != m_byClass.end())
                return m_entries[it->second].name;
        }
        return "";
    }

    // Pull a pixel size out of an icon theme path segment, e.g. ".../48x48/apps/x.png"
    // or ".../scalable/apps/x.svg". Scalable sorts as "perfect at any size".
    static int sizeFromPath(const fs::path& p, bool& scalable) {
        scalable = false;
        for (const auto& part : p) {
            const auto s = part.string();
            if (s == "scalable" || s == "symbolic") {
                scalable = true;
                return 0;
            }
            const auto x = s.find('x');
            if (x == std::string::npos || x == 0)
                continue;
            if (!std::all_of(s.begin(), s.begin() + x, [](unsigned char c) { return std::isdigit(c); }))
                continue;
            try {
                return std::stoi(s.substr(0, x));
            } catch (...) { /* not a size segment */ }
        }
        return 0;
    }

    std::optional<std::string> CDesktopDb::resolveIconPath(const std::string& iconName, int preferredSize) const {
        if (iconName.empty())
            return std::nullopt;

        std::error_code ec;

        // Absolute path in Icon= is legal and common for Steam/Flatpak entries.
        if (iconName.front() == '/') {
            if (fs::is_regular_file(iconName, ec))
                return iconName;
            return std::nullopt;
        }

        const std::string cacheKey = iconName + "@" + std::to_string(preferredSize);
        if (auto it = m_iconCache.find(cacheKey); it != m_iconCache.end())
            return it->second.empty() ? std::nullopt : std::optional<std::string>(it->second);

        static const std::vector<std::string> EXTS = {".svg", ".png", ".xpm"};

        std::string bestPath;
        long        bestScore = -1;

        for (const auto& root : m_iconRoots) {
            if (!fs::is_directory(root, ec))
                continue;

            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec)
                    break;
                if (!it->is_regular_file(ec))
                    continue;

                const auto& p = it->path();
                if (p.stem().string() != iconName)
                    continue;
                if (std::ranges::find(EXTS, p.extension().string()) == EXTS.end())
                    continue;

                bool       scalable = false;
                const int  sz       = sizeFromPath(p, scalable);
                const bool isSvg    = p.extension() == ".svg";

                // Scoring: scalable/svg wins, then closest size at or above the
                // requested one, then anything else.
                long score = 0;
                if (scalable || isSvg)
                    score = 10'000;
                else if (sz >= preferredSize)
                    score = 5'000 - (sz - preferredSize);
                else
                    score = 1'000 + sz;

                if (score > bestScore) {
                    bestScore = score;
                    bestPath  = p.string();
                }
            }

            // A hit in a higher-priority root wins outright.
            if (bestScore > 0)
                break;
        }

        m_iconCache[cacheKey] = bestPath;
        if (bestPath.empty())
            return std::nullopt;
        return bestPath;
    }

} // namespace hyprspace
