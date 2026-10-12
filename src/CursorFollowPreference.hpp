#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unistd.h>

namespace hyprspace {

    inline std::filesystem::path cursorFollowPreferencePath(const char* stateHome, const char* userHome) {
        if (stateHome && std::filesystem::path(stateHome).is_absolute())
            return std::filesystem::path(stateHome) / "hyprspace/cursor-follow";
        if (userHome && std::filesystem::path(userHome).is_absolute())
            return std::filesystem::path(userHome) / ".local/state/hyprspace/cursor-follow";
        return {};
    }

    inline bool loadCursorFollowPreference(const std::filesystem::path& file) {
        std::ifstream       input(file);
        std::array<char, 9> buffer{};
        input.read(buffer.data(), buffer.size());
        const std::string_view value(buffer.data(), input.gcount());
        return value == "enabled" || value == "enabled\n";
    }

    inline bool saveCursorFollowPreference(const std::filesystem::path& file, bool enabled) {
        if (!file.is_absolute())
            return false;
        std::error_code error;
        const bool      created = std::filesystem::create_directories(file.parent_path(), error);
        if (error)
            return false;
        if (created) {
            std::filesystem::permissions(file.parent_path(), std::filesystem::perms::owner_all, error);
            if (error)
                return false;
        }
        auto      temporary  = file.string() + ".XXXXXX";
        const int descriptor = mkstemp(temporary.data());
        if (descriptor < 0)
            return false;
        const std::string_view value   = enabled ? "enabled\n" : "disabled\n";
        const auto             written = write(descriptor, value.data(), value.size());
        const int              closed  = close(descriptor);
        if (written == static_cast<ssize_t>(value.size()) && closed == 0) {
            std::filesystem::rename(temporary, file, error);
            if (!error)
                return true;
        }
        std::filesystem::remove(temporary, error);
        return false;
    }

} // namespace hyprspace
