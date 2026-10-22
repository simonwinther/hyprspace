#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace hyprspace::spaces {
    inline constexpr std::string_view DEFAULT_SPACE_ICON  = "grid";
    inline constexpr std::string_view DEFAULT_SPACE_COLOR = "#89b4fa";

    struct SSpaceIcon {
        std::string_view id, label, keywords;
    };

    inline constexpr std::array<SSpaceIcon, 48> SPACE_ICON_CATALOG = {{{"grid", "Grid", "overview windows workspace dashboard"},
                                                                       {"graduation-cap", "Study", "school university college course learning education"},
                                                                       {"briefcase", "Work", "job business office career"},
                                                                       {"gamepad-2", "Games", "gaming controller fun play"},
                                                                       {"code-2", "Code", "programming development software html"},
                                                                       {"flask-conical", "Science", "chemistry lab research experiment"},
                                                                       {"music", "Music", "audio song instrument sound"},
                                                                       {"book-open", "Reading", "books literature library study"},
                                                                       {"heart", "Heart", "love health personal family"},
                                                                       {"house", "Home", "household personal family"},
                                                                       {"palette", "Art", "design painting creative drawing color"},
                                                                       {"globe", "World", "web internet language travel"},
                                                                       {"camera", "Photography", "photo picture image"},
                                                                       {"film", "Film", "movie video cinema editing"},
                                                                       {"headphones", "Headphones", "music audio sound listen"},
                                                                       {"mic", "Microphone", "record podcast voice singing"},
                                                                       {"pen-tool", "Design", "vector creative graphics drawing"},
                                                                       {"brush", "Painting", "paint art drawing creative"},
                                                                       {"folder", "Files", "documents project directory archive"},
                                                                       {"terminal", "Terminal", "shell console command linux development"},
                                                                       {"cpu", "Hardware", "computer processor chip electronics"},
                                                                       {"database", "Database", "data sql storage analytics"},
                                                                       {"server", "Server", "hosting cloud network infrastructure"},
                                                                       {"monitor", "Desktop", "computer screen display pc"},
                                                                       {"smartphone", "Phone", "mobile app android ios device"},
                                                                       {"calculator", "Math", "mathematics numbers finance accounting"},
                                                                       {"atom", "Physics", "science quantum chemistry"},
                                                                       {"brain", "Thinking", "ideas psychology learning intelligence"},
                                                                       {"microscope", "Biology", "science research lab medicine"},
                                                                       {"telescope", "Astronomy", "science stars space planet sky"},
                                                                       {"rocket", "Rocket", "launch space project startup"},
                                                                       {"wrench", "Tools", "repair maintenance engineering diy"},
                                                                       {"hammer", "Building", "construction craft maker diy"},
                                                                       {"settings", "Settings", "preferences configuration system"},
                                                                       {"leaf", "Nature", "plant environment garden ecology"},
                                                                       {"tree-pine", "Forest", "trees nature outdoors camping"},
                                                                       {"mountain", "Mountains", "hiking climbing outdoors adventure"},
                                                                       {"sun", "Sun", "day light weather summer"},
                                                                       {"moon", "Moon", "night dark evening sleep"},
                                                                       {"coffee", "Coffee", "break cafe drink tea"},
                                                                       {"cooking-pot", "Cooking", "food kitchen recipe meal"},
                                                                       {"dumbbell", "Fitness", "gym exercise health sport strength"},
                                                                       {"bike", "Cycling", "bicycle sport exercise transport"},
                                                                       {"plane", "Travel", "airplane flight holiday vacation"},
                                                                       {"train-front", "Train", "rail commute transport travel"},
                                                                       {"map", "Map", "navigation geography travel location"},
                                                                       {"compass", "Explore", "navigation direction discover outdoors"},
                                                                       {"sparkles", "Ideas", "inspiration magic creative special"}}};

    inline const SSpaceIcon* spaceIcon(std::string_view id) {
        for (const auto& icon : SPACE_ICON_CATALOG)
            if (icon.id == id)
                return &icon;
        return nullptr;
    }

    inline bool validSpaceIcon(std::string_view id) {
        return spaceIcon(id) != nullptr;
    }

    inline std::optional<std::string> normalizeSpaceColor(std::string_view text) {
        if (text.size() != 7 || text.front() != '#')
            return std::nullopt;
        std::string result(text);
        for (size_t i = 1; i < result.size(); ++i) {
            const auto value = result[i];
            if (value >= 'A' && value <= 'F')
                result[i] = static_cast<char>(value - 'A' + 'a');
            else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
                return std::nullopt;
        }
        return result;
    }
} // namespace hyprspace::spaces
