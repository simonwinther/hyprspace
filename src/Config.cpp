#include "Config.hpp"

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

#include <memory>

namespace hyprspace::config {

    namespace {
        struct SValues {
            SP<Config::Values::CFloatValue>  overviewBgDim;
            SP<Config::Values::CColorValue>  overviewBgColor;
            SP<Config::Values::CIntValue>    overviewPadding;
            SP<Config::Values::CIntValue>    overviewGap;
            SP<Config::Values::CIntValue>    overviewBandGap;
            SP<Config::Values::CIntValue>    overviewRounding;
            SP<Config::Values::CIntValue>    overviewBorderSize;
            SP<Config::Values::CColorValue>  overviewActiveBorder;
            SP<Config::Values::CColorValue>  overviewHoverBorder;
            SP<Config::Values::CBoolValue>   overviewShowLabels;
            SP<Config::Values::CBoolValue>   overviewIncludeSpecial;
            SP<Config::Values::CBoolValue>   overviewAllWorkspaces;
            SP<Config::Values::CColorValue>  overviewLabelColor;
            SP<Config::Values::CColorValue>  overviewTileBgColor;
            SP<Config::Values::CColorValue>  overviewTileBorderColor;
            SP<Config::Values::CColorValue>  overviewTitleBgColor;
            SP<Config::Values::CStringValue> overviewFont;
            SP<Config::Values::CColorValue>  overviewFullscreenBorder;

            SP<Config::Values::CIntValue>    switcherIconSize;
            SP<Config::Values::CIntValue>    switcherPadding;
            SP<Config::Values::CIntValue>    switcherGap;
            SP<Config::Values::CIntValue>    switcherRounding;
            SP<Config::Values::CColorValue>  switcherBgColor;
            SP<Config::Values::CColorValue>  switcherHighlightColor;
            SP<Config::Values::CColorValue>  switcherTextColor;
            SP<Config::Values::CBoolValue>   switcherShowTitle;
            SP<Config::Values::CBoolValue>   switcherCurrentWorkspaceOnly;
            SP<Config::Values::CStringValue> switcherFont;

            SP<Config::Values::CBoolValue>   followMouse;
            SP<Config::Values::CBoolValue>   warpCursor;
        };

        SValues g_values;

        template <typename T, typename... Args>
        SP<T> reg(Args&&... args) {
            auto v = makeShared<T>(std::forward<Args>(args)...);
            HyprlandAPI::addConfigValueV2(PHANDLE, v);
            return v;
        }

        CHyprColor colorOf(const SP<Config::Values::CColorValue>& v, uint64_t fallback) {
            return CHyprColor(v ? static_cast<uint64_t>(v->value()) : fallback);
        }
    }

    void registerAll() {
        using namespace Config::Values;

        // ---- overview ----
        g_values.overviewBgDim = reg<CFloatValue>("plugin:hyprspace:overview:bg_dim", "how much to dim the desktop behind the overview (0-1)", 0.80F,
                                                  SFloatValueOptions{.min = 0.F, .max = 1.F});
        g_values.overviewBgColor =
            reg<CColorValue>("plugin:hyprspace:overview:bg_color", "colour mixed over the desktop behind the overview", 0xff11111b);
        g_values.overviewPadding    = reg<CIntValue>("plugin:hyprspace:overview:padding", "outer padding of the overview, in px", 56, SIntValueOptions{.min = 0, .max = 512});
        g_values.overviewGap        = reg<CIntValue>("plugin:hyprspace:overview:gap", "gap between workspace tiles, in px", 28, SIntValueOptions{.min = 0, .max = 256});
        g_values.overviewBandGap    = reg<CIntValue>("plugin:hyprspace:overview:band_gap", "gap between workspace rows, in px", 28, SIntValueOptions{.min = 0, .max = 256});
        g_values.overviewRounding   = reg<CIntValue>("plugin:hyprspace:overview:rounding", "corner radius of workspace tiles, in px", 14, SIntValueOptions{.min = 0, .max = 64});
        g_values.overviewBorderSize = reg<CIntValue>("plugin:hyprspace:overview:border_size", "selection border thickness, in px", 3, SIntValueOptions{.min = 0, .max = 16});
        g_values.overviewActiveBorder = reg<CColorValue>("plugin:hyprspace:overview:active_border", "border colour of the selected tile", 0xff89b4fa);
        g_values.overviewHoverBorder  = reg<CColorValue>("plugin:hyprspace:overview:hover_border", "border colour of the hovered tile", 0x8089b4fa);
        g_values.overviewShowLabels   = reg<CBoolValue>("plugin:hyprspace:overview:workspace_labels", "show the workspace name under each tile", true);
        g_values.overviewIncludeSpecial = reg<CBoolValue>("plugin:hyprspace:overview:include_special", "include special (scratchpad) workspaces", true);
        g_values.overviewAllWorkspaces  = reg<CBoolValue>("plugin:hyprspace:overview:all_workspaces", "show every workspace on the monitor, not just the active one", true);
        g_values.overviewLabelColor     = reg<CColorValue>("plugin:hyprspace:overview:label_color", "workspace label colour", 0xffcdd6f4);
        g_values.overviewTileBgColor    = reg<CColorValue>("plugin:hyprspace:overview:tile_bg_color", "backing plate drawn behind each workspace tile", 0xd90d0d14);
        g_values.overviewTileBorderColor = reg<CColorValue>("plugin:hyprspace:overview:tile_border_color", "hairline drawn around every workspace tile", 0x1affffff);
        g_values.overviewTitleBgColor   = reg<CColorValue>("plugin:hyprspace:overview:title_bg_color", "window title backdrop colour", 0xe61e1e2e);
        g_values.overviewFont           = reg<CStringValue>("plugin:hyprspace:overview:font", "pango font description used in the overview", "Sans 12");
        g_values.overviewFullscreenBorder =
            reg<CColorValue>("plugin:hyprspace:overview:fullscreen_border", "outline and badge marking the window that is fullscreen", 0xff89b4fa);

        // ---- switcher ----
        g_values.switcherIconSize = reg<CIntValue>("plugin:hyprspace:switcher:icon_size", "app icon size in the alt-tab switcher, in px", 96, SIntValueOptions{.min = 24, .max = 256});
        g_values.switcherPadding  = reg<CIntValue>("plugin:hyprspace:switcher:padding", "inner padding of the switcher panel, in px", 24, SIntValueOptions{.min = 0, .max = 128});
        g_values.switcherGap      = reg<CIntValue>("plugin:hyprspace:switcher:gap", "gap between switcher entries, in px", 12, SIntValueOptions{.min = 0, .max = 128});
        g_values.switcherRounding = reg<CIntValue>("plugin:hyprspace:switcher:rounding", "corner radius of the switcher panel, in px", 20, SIntValueOptions{.min = 0, .max = 64});
        g_values.switcherBgColor  = reg<CColorValue>("plugin:hyprspace:switcher:bg_color", "switcher panel background colour", 0xf01e1e2e);
        g_values.switcherHighlightColor = reg<CColorValue>("plugin:hyprspace:switcher:highlight_color", "selection highlight colour", 0x4089b4fa);
        g_values.switcherTextColor      = reg<CColorValue>("plugin:hyprspace:switcher:text_color", "switcher title colour", 0xffcdd6f4);
        g_values.switcherShowTitle      = reg<CBoolValue>("plugin:hyprspace:switcher:show_title", "show the selected window's title under the icons", true);
        g_values.switcherCurrentWorkspaceOnly =
            reg<CBoolValue>("plugin:hyprspace:switcher:current_workspace_only", "restrict the switcher to the active workspace", true);
        g_values.switcherFont = reg<CStringValue>("plugin:hyprspace:switcher:font", "pango font description used in the switcher", "Sans 13");

        // ---- shared ----
        g_values.followMouse = reg<CBoolValue>("plugin:hyprspace:follow_mouse", "move the selection to whatever the pointer hovers", true);
        g_values.warpCursor  = reg<CBoolValue>("plugin:hyprspace:warp_cursor",
                                               "warp the pointer onto the chosen window when committing; needed for the selection to stick when "
                                               "input:follow_mouse is enabled",
                                               true);
    }

    float overviewBgDim() {
        return g_values.overviewBgDim ? g_values.overviewBgDim->value() : 0.80F;
    }
    CHyprColor overviewBgColor() {
        return colorOf(g_values.overviewBgColor, 0xff11111b);
    }
    int overviewPadding() {
        return g_values.overviewPadding ? g_values.overviewPadding->value() : 56;
    }
    int overviewGap() {
        return g_values.overviewGap ? g_values.overviewGap->value() : 28;
    }
    int overviewBandGap() {
        return g_values.overviewBandGap ? g_values.overviewBandGap->value() : 28;
    }
    int overviewRounding() {
        return g_values.overviewRounding ? g_values.overviewRounding->value() : 14;
    }
    int overviewBorderSize() {
        return g_values.overviewBorderSize ? g_values.overviewBorderSize->value() : 3;
    }
    CHyprColor overviewActiveBorder() {
        return colorOf(g_values.overviewActiveBorder, 0xff89b4fa);
    }
    CHyprColor overviewHoverBorder() {
        return colorOf(g_values.overviewHoverBorder, 0x8089b4fa);
    }
    bool overviewShowLabels() {
        return g_values.overviewShowLabels ? g_values.overviewShowLabels->value() : true;
    }
    bool overviewIncludeSpecial() {
        return g_values.overviewIncludeSpecial ? g_values.overviewIncludeSpecial->value() : true;
    }
    bool overviewAllWorkspaces() {
        return g_values.overviewAllWorkspaces ? g_values.overviewAllWorkspaces->value() : true;
    }
    CHyprColor overviewLabelColor() {
        return colorOf(g_values.overviewLabelColor, 0xffcdd6f4);
    }
    CHyprColor overviewTileBgColor() {
        return colorOf(g_values.overviewTileBgColor, 0xd90d0d14);
    }
    CHyprColor overviewTileBorderColor() {
        return colorOf(g_values.overviewTileBorderColor, 0x1affffff);
    }
    CHyprColor overviewTitleBgColor() {
        return colorOf(g_values.overviewTitleBgColor, 0xe61e1e2e);
    }
    std::string overviewFont() {
        return g_values.overviewFont ? g_values.overviewFont->value() : "Sans 12";
    }
    CHyprColor overviewFullscreenBorder() {
        return colorOf(g_values.overviewFullscreenBorder, 0xff89b4fa);
    }

    int switcherIconSize() {
        return g_values.switcherIconSize ? g_values.switcherIconSize->value() : 96;
    }
    int switcherPadding() {
        return g_values.switcherPadding ? g_values.switcherPadding->value() : 24;
    }
    int switcherGap() {
        return g_values.switcherGap ? g_values.switcherGap->value() : 12;
    }
    int switcherRounding() {
        return g_values.switcherRounding ? g_values.switcherRounding->value() : 20;
    }
    CHyprColor switcherBgColor() {
        return colorOf(g_values.switcherBgColor, 0xf01e1e2e);
    }
    CHyprColor switcherHighlightColor() {
        return colorOf(g_values.switcherHighlightColor, 0x4089b4fa);
    }
    CHyprColor switcherTextColor() {
        return colorOf(g_values.switcherTextColor, 0xffcdd6f4);
    }
    bool switcherShowTitle() {
        return g_values.switcherShowTitle ? g_values.switcherShowTitle->value() : true;
    }
    bool switcherCurrentWorkspaceOnly() {
        return g_values.switcherCurrentWorkspaceOnly ? g_values.switcherCurrentWorkspaceOnly->value() : false;
    }
    std::string switcherFont() {
        return g_values.switcherFont ? g_values.switcherFont->value() : "Sans 13";
    }

    bool followMouse() {
        return g_values.followMouse ? g_values.followMouse->value() : true;
    }
    bool warpCursor() {
        return g_values.warpCursor ? g_values.warpCursor->value() : true;
    }

} // namespace hyprspace::config
