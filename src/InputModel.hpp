#pragma once

#include <cstdint>
#include <utility>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace hyprspace {
    enum class EInputMode { Normal, Insert };
    enum class EInputContext { Workspaces, Windows, Spaces, SpaceMenu, SpacePicker, SpaceEditor, SpaceRemove, SpaceMove, Help, Switcher };
    enum class ETextAction { Backspace, Delete, Left, Right, Home, End, SelectAll, Clear };
    enum class EInputAction {
        None,
        MoveLeft,
        MoveDown,
        MoveUp,
        MoveRight,
        First,
        Last,
        NextField,
        PrevField,
        PagePrevious,
        PageNext,
        Accept,
        Back,
        Search,
        FocusText,
        FinishEditing,
        NextMatch,
        PrevMatch,
        ReorderEarlier,
        ReorderLater,
        Edit,
        Create,
        Move, // Legacy menu action; keyboard transfers always choose an explicit scope.
        MoveWindow,
        MoveWorkspace,
        Remove,
        Save,
        ToggleSpaces,
        OpenManager,
        ShowWorkspaces,
        ShowWindows,
        CycleGrouping,
        ToggleRecent,
        NewWorkspace,
        CloseWorkspace,
        Help,
        PreviousSpace,
        NextSpace,
        WorkspaceDigit,
        Prefix,
        CancelPrefix,
        UnknownPrefix,
    };

    struct SInputContext {
        EInputContext context = EInputContext::Workspaces;
        EInputMode    mode    = EInputMode::Normal;
        std::string   field;
        uint64_t      generation                             = 0;
        bool          textEditable                           = false;
        bool          protectedDraft                         = false;
        bool          operator==(const SInputContext&) const = default;
    };

    constexpr std::string_view inputModeName(EInputMode mode) {
        return mode == EInputMode::Insert ? "insert" : "normal";
    }
    constexpr std::string_view inputContextName(EInputContext context) {
        switch (context) {
        case EInputContext::Workspaces:
            return "workspaces";
        case EInputContext::Windows:
            return "windows";
        case EInputContext::Spaces:
            return "spaces";
        case EInputContext::SpaceMenu:
            return "space-menu";
        case EInputContext::SpacePicker:
            return "space-picker";
        case EInputContext::SpaceEditor:
            return "space-editor";
        case EInputContext::SpaceRemove:
            return "space-remove";
        case EInputContext::SpaceMove:
            return "space-move";
        case EInputContext::Help:
            return "help";
        case EInputContext::Switcher:
            return "switcher";
        }
        return "workspaces";
    }
    constexpr bool inputForm(EInputContext context) {
        return context == EInputContext::SpaceEditor || context == EInputContext::SpaceRemove || context == EInputContext::SpaceMove;
    }

    enum EInputModifier : uint32_t { InputShift = 1, InputCtrl = 2, InputAlt = 4, InputSuper = 8 };
    struct SInputBinding {
        char             key;
        EInputAction     action;
        std::string_view label;
    };
    inline constexpr std::array INPUT_LEADER_BINDINGS{
        SInputBinding{'s', EInputAction::ToggleSpaces, "Spaces"},
        SInputBinding{'p', EInputAction::OpenManager, "Manage spaces"},
        SInputBinding{'e', EInputAction::Edit, "Edit space / icon / color"},
        SInputBinding{'a', EInputAction::Create, "Add space"},
        SInputBinding{'w', EInputAction::ShowWorkspaces, "Workspaces"},
        SInputBinding{'v', EInputAction::ShowWindows, "Windows"},
        SInputBinding{'g', EInputAction::CycleGrouping, "Window grouping"},
        SInputBinding{'r', EInputAction::ToggleRecent, "Recent-first order"},
        SInputBinding{'n', EInputAction::NewWorkspace, "New workspace"},
        SInputBinding{'x', EInputAction::CloseWorkspace, "Close empty workspace"},
        SInputBinding{'m', EInputAction::MoveWindow, "Move selected window to space"},
        SInputBinding{'M', EInputAction::MoveWorkspace, "Move workspace to space"},
        SInputBinding{'d', EInputAction::Remove, "Remove space…"},
    };
    constexpr bool inputActionAvailable(EInputAction action, EInputContext context) {
        if (inputForm(context))
            return action == EInputAction::Help;
        if (action == EInputAction::CycleGrouping || action == EInputAction::ToggleRecent)
            return context == EInputContext::Windows;
        if (action == EInputAction::CloseWorkspace)
            return context == EInputContext::Workspaces;
        if (action == EInputAction::Move || action == EInputAction::MoveWindow || action == EInputAction::MoveWorkspace || action == EInputAction::NewWorkspace)
            return context == EInputContext::Workspaces || context == EInputContext::Windows;
        return true;
    }
    constexpr bool inputActionAvailable(EInputAction action, const SInputContext& context) {
        return context.protectedDraft ? action == EInputAction::Help : inputActionAvailable(action, context.context);
    }
    struct SInputCommand {
        EInputAction               action = EInputAction::None;
        std::optional<ETextAction> edit;
        std::string                text;
        bool                       extend     = false;
        int                        digit      = -1;
        bool                       repeatable = false;
        bool                       handled() const {
            return action != EInputAction::None || edit || !text.empty();
        }
    };

    // Pure key grammar. Views provide context and execute semantic commands;
    // repeats are tied to this context epoch rather than a particular widget.
    class CInputModel {
      public:
        enum class EPrefix { None, Leader, G };
        void sync(const SInputContext& context) {
            if (!m_initialized || context != m_context) {
                m_context     = context;
                m_prefix      = EPrefix::None;
                m_initialized = true;
                ++m_epoch;
            }
        }
        void reset() {
            m_prefix      = EPrefix::None;
            m_initialized = false;
            ++m_epoch;
        }
        uint64_t epoch() const {
            return m_epoch;
        }
        EPrefix prefix() const {
            return m_prefix;
        }
        const SInputContext& context() const {
            return m_context;
        }
        std::string_view prefixName() const {
            return m_prefix == EPrefix::Leader ? "space" : m_prefix == EPrefix::G ? "g" : "";
        }
        SInputCommand peek(uint32_t sym, uint32_t mods, const std::string& text) const {
            return decode(sym, mods, text).first;
        }
        SInputCommand key(uint32_t sym, uint32_t mods, const std::string& text) {
            auto [command, prefix] = decode(sym, mods, text);
            if (prefix != m_prefix) {
                m_prefix = prefix;
                ++m_epoch;
            }
            return command;
        }

      private:
        std::pair<SInputCommand, EPrefix> decode(uint32_t sym, uint32_t mods, const std::string& text) const {
            if ((sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) || sym == XKB_KEY_Mode_switch ||
                sym == XKB_KEY_Num_Lock || sym == XKB_KEY_Scroll_Lock)
                return {{}, m_prefix};
            if (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req || (sym >= 0x10080000 && sym <= 0x1008FFFF))
                return {{}, EPrefix::None};
            const bool shift = mods & InputShift;
            if (shift && sym == XKB_KEY_g)
                sym = XKB_KEY_G;
            if (shift && sym == XKB_KEY_n)
                sym = XKB_KEY_N;
            if (shift && sym == XKB_KEY_m)
                sym = XKB_KEY_M;
            const bool plain  = (mods & (InputCtrl | InputAlt | InputSuper)) == 0;
            auto       action = [&](EInputAction value, bool repeat = false, EPrefix prefix = EPrefix::None) {
                return std::pair{SInputCommand{.action = value, .repeatable = repeat}, prefix};
            };
            auto       edit   = [&](ETextAction value) { return std::pair{SInputCommand{.edit = value, .extend = shift, .repeatable = true}, EPrefix::None}; };
            const bool escape = sym == XKB_KEY_Escape || (mods == InputCtrl && sym == XKB_KEY_bracketleft);
            if (m_context.mode == EInputMode::Insert) {
                if ((mods & InputCtrl) && !(mods & (InputAlt | InputSuper)) && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) &&
                    m_context.context == EInputContext::SpaceEditor)
                    return action(EInputAction::Save);
                if (escape || (plain && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)))
                    return action(EInputAction::FinishEditing);
                if (plain && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab))
                    return action(shift || sym == XKB_KEY_ISO_Left_Tab ? EInputAction::PrevField : EInputAction::NextField);
                if (mods == InputCtrl && (sym == XKB_KEY_a || sym == XKB_KEY_A))
                    return edit(ETextAction::SelectAll);
                if (mods == InputCtrl && (sym == XKB_KEY_u || sym == XKB_KEY_U))
                    return edit(ETextAction::Clear);
                if (plain) {
                    if (sym == XKB_KEY_BackSpace)
                        return edit(ETextAction::Backspace);
                    if (sym == XKB_KEY_Delete)
                        return edit(ETextAction::Delete);
                    if (sym == XKB_KEY_Left)
                        return edit(ETextAction::Left);
                    if (sym == XKB_KEY_Right)
                        return edit(ETextAction::Right);
                    if (sym == XKB_KEY_Home)
                        return edit(ETextAction::Home);
                    if (sym == XKB_KEY_End)
                        return edit(ETextAction::End);
                    if (!text.empty())
                        return {SInputCommand{.text = text, .repeatable = true}, EPrefix::None};
                }
                return {{}, EPrefix::None};
            }
            if (m_prefix != EPrefix::None && !plain && !escape)
                return {{}, EPrefix::None};
            if (m_prefix != EPrefix::None) {
                if (escape || (plain && sym == XKB_KEY_space))
                    return action(EInputAction::CancelPrefix);
                if (m_prefix == EPrefix::G && plain && sym == XKB_KEY_g)
                    return action(EInputAction::First);
                if (m_prefix == EPrefix::Leader && plain) {
                    for (const auto& binding : INPUT_LEADER_BINDINGS)
                        if (sym == static_cast<uint32_t>(binding.key) && inputActionAvailable(binding.action, m_context))
                            return action(binding.action);
                    if (text == "?" || sym == XKB_KEY_question)
                        return action(EInputAction::Help);
                }
                return action(EInputAction::UnknownPrefix);
            }
            if (escape)
                return action(EInputAction::Back);
            if (mods == InputCtrl && !inputForm(m_context.context) && !m_context.protectedDraft) {
                if (sym == XKB_KEY_space)
                    return action(EInputAction::ToggleSpaces);
                if (sym == XKB_KEY_Page_Up)
                    return action(EInputAction::PreviousSpace);
                if (sym == XKB_KEY_Page_Down)
                    return action(EInputAction::NextSpace);
            }
            if ((mods & InputCtrl) && !(mods & (InputAlt | InputSuper)) && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) &&
                m_context.context == EInputContext::SpaceEditor)
                return action(EInputAction::Save);
            if (!plain)
                return {{}, EPrefix::None};
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
                return action(EInputAction::Accept);
            if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)
                return action(shift || sym == XKB_KEY_ISO_Left_Tab ? EInputAction::PrevField : EInputAction::NextField);
            if (m_context.context == EInputContext::Spaces || m_context.context == EInputContext::SpacePicker) {
                if (sym == XKB_KEY_K || (shift && sym == XKB_KEY_k))
                    return action(EInputAction::ReorderEarlier, true);
                if (sym == XKB_KEY_J || (shift && sym == XKB_KEY_j))
                    return action(EInputAction::ReorderLater, true);
            }
            if (sym == XKB_KEY_h || sym == XKB_KEY_Left)
                return action(EInputAction::MoveLeft, true);
            if (sym == XKB_KEY_j || sym == XKB_KEY_Down)
                return action(EInputAction::MoveDown, true);
            if (sym == XKB_KEY_k || sym == XKB_KEY_Up)
                return action(EInputAction::MoveUp, true);
            if (sym == XKB_KEY_l || sym == XKB_KEY_Right)
                return action(EInputAction::MoveRight, true);
            if (sym == XKB_KEY_Home)
                return action(EInputAction::First);
            if (sym == XKB_KEY_End || sym == XKB_KEY_G)
                return action(EInputAction::Last);
            if (sym == XKB_KEY_g)
                return action(EInputAction::Prefix, false, EPrefix::G);
            if (sym == XKB_KEY_space && !inputForm(m_context.context))
                return action(EInputAction::Prefix, false, EPrefix::Leader);
            if (sym == XKB_KEY_Page_Up)
                return action(EInputAction::PagePrevious, true);
            if (sym == XKB_KEY_Page_Down)
                return action(EInputAction::PageNext, true);
            if (sym == XKB_KEY_i && m_context.textEditable)
                return action(EInputAction::FocusText);
            if (text == "?" || sym == XKB_KEY_question)
                return action(EInputAction::Help);
            if ((!inputForm(m_context.context) || (m_context.context == EInputContext::SpaceEditor && m_context.field == "icon")) &&
                (text == "/" || sym == XKB_KEY_slash))
                return action(EInputAction::Search);
            if (!inputForm(m_context.context)) {
                if (sym == XKB_KEY_n)
                    return action(EInputAction::NextMatch, true);
                if (sym == XKB_KEY_N)
                    return action(EInputAction::PrevMatch, true);
                if (sym == XKB_KEY_F2)
                    return action(EInputAction::Edit);
                if (sym == XKB_KEY_Delete && (m_context.context == EInputContext::Spaces || m_context.context == EInputContext::SpacePicker))
                    return action(EInputAction::Remove);
                if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9 && (m_context.context == EInputContext::Workspaces || m_context.context == EInputContext::Windows))
                    return {SInputCommand{.action = EInputAction::WorkspaceDigit, .digit = static_cast<int>(sym - XKB_KEY_0)}, EPrefix::None};
            }
            return {{}, EPrefix::None};
        }
        SInputContext m_context;
        EPrefix       m_prefix      = EPrefix::None;
        uint64_t      m_epoch       = 0;
        bool          m_initialized = false;
    };
} // namespace hyprspace
