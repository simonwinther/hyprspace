#pragma once
#include "../src/InputModel.hpp"

static void testInputModel() {
    section("input: shared Vim grammar, literal Insert, and repeat context ownership");
    CInputModel input;
    input.sync({});
    const auto initial = input.epoch();
    CHECK(input.key(XKB_KEY_j, 0, "j").action == EInputAction::MoveDown);
    CHECK(input.epoch() == initial);
    CHECK(input.key(XKB_KEY_g, 0, "g").action == EInputAction::Prefix);
    CHECK(input.prefix() == CInputModel::EPrefix::G);
    CHECK(input.key(XKB_KEY_g, 0, "g").action == EInputAction::First);
    CHECK(input.prefix() == CInputModel::EPrefix::None);
    CHECK(input.key(XKB_KEY_G, InputShift, "G").action == EInputAction::Last);
    CHECK(input.key(XKB_KEY_g, InputShift, "G").action == EInputAction::Last);
    CHECK(input.key(XKB_KEY_space, 0, " ").action == EInputAction::Prefix);
    CHECK(input.prefix() == CInputModel::EPrefix::Leader);
    CHECK(input.peek(XKB_KEY_s, 0, "s").action == EInputAction::ToggleSpaces);
    CHECK(input.prefix() == CInputModel::EPrefix::Leader);
    CHECK(input.key(XKB_KEY_s, 0, "s").action == EInputAction::ToggleSpaces);
    CHECK(input.prefix() == CInputModel::EPrefix::None);
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_Escape, 0, "").action == EInputAction::CancelPrefix);
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_z, 0, "z").action == EInputAction::UnknownPrefix);
    input.key(XKB_KEY_space, 0, " ");
    CHECK(!input.key(XKB_KEY_Shift_L, InputShift, "").handled());
    CHECK(input.prefix() == CInputModel::EPrefix::Leader);
    CHECK(!input.key(XKB_KEY_Print, 0, "").handled());
    CHECK(input.prefix() == CInputModel::EPrefix::None);
    CHECK(!input.key(XKB_KEY_o, InputCtrl, "").handled());
    CHECK(!input.key(XKB_KEY_i, InputCtrl, "").handled());
    CHECK(input.key(XKB_KEY_n, 0, "n").action == EInputAction::NextMatch);
    CHECK(input.key(XKB_KEY_N, InputShift, "N").action == EInputAction::PrevMatch);
    CHECK(input.key(XKB_KEY_n, InputShift, "N").action == EInputAction::PrevMatch);
    CHECK(!input.key(XKB_KEY_s, 0, "s").handled());
    CHECK(input.key(XKB_KEY_8, 0, "8").digit == 8);
    CHECK(input.key(XKB_KEY_question, InputShift, "?").action == EInputAction::Help);
    CHECK(input.key(XKB_KEY_slash, 0, "/").action == EInputAction::Search);
    input.sync({.context = EInputContext::Windows, .mode = EInputMode::Insert, .field = "search", .generation = 1, .textEditable = true});
    CHECK(input.epoch() != initial);
    const auto        insertEpoch = input.epoch();
    const std::string literal     = "hjkl /gnsawrxmbM ";
    for (char c : literal) {
        const auto command = input.key(c, 0, std::string(1, c));
        CHECK(command.text == std::string(1, c));
        CHECK(command.action == EInputAction::None);
        CHECK(command.repeatable);
        CHECK(input.epoch() == insertEpoch);
    }
    CHECK(input.key(XKB_KEY_Left, InputShift, "").edit == ETextAction::Left);
    CHECK(input.key(XKB_KEY_Left, InputShift, "").extend);
    CHECK(input.key(XKB_KEY_u, InputCtrl, "").edit == ETextAction::Clear);
    CHECK(input.key(XKB_KEY_Return, 0, "").action == EInputAction::FinishEditing);
    CHECK(input.key(XKB_KEY_Escape, 0, "").action == EInputAction::FinishEditing);
    CHECK(!input.key(XKB_KEY_o, InputCtrl, "").handled());
    CHECK(!input.key(XKB_KEY_i, InputCtrl, "").handled());
    input.sync({.context = EInputContext::SpaceEditor, .mode = EInputMode::Insert, .field = "name", .generation = 2, .textEditable = true});
    CHECK(input.key(XKB_KEY_Return, InputCtrl, "").action == EInputAction::Save);
    CHECK(input.key(XKB_KEY_Tab, 0, "\t").action == EInputAction::NextField);
    input.sync({.context = EInputContext::SpaceEditor, .mode = EInputMode::Normal, .field = "icon", .generation = 3, .textEditable = true});
    const auto iconEpoch = input.epoch();
    CHECK(input.key(XKB_KEY_l, 0, "l").action == EInputAction::MoveRight);
    CHECK(input.epoch() == iconEpoch);
    CHECK(!input.key(XKB_KEY_space, 0, " ").handled());
    CHECK(!input.key(XKB_KEY_o, InputCtrl, "").handled());
    CHECK(input.key(XKB_KEY_slash, 0, "/").action == EInputAction::Search);
    CHECK(input.key(XKB_KEY_i, 0, "i").action == EInputAction::FocusText);
    CHECK(input.key(XKB_KEY_Return, InputCtrl, "").action == EInputAction::Save);
    CHECK(input.key(XKB_KEY_Escape, 0, "").action == EInputAction::Back);
    input.sync({.context = EInputContext::SpaceEditor, .mode = EInputMode::Normal, .field = "name", .generation = 4, .textEditable = true});
    CHECK(input.epoch() != iconEpoch);
    CHECK(input.key(XKB_KEY_i, 0, "i").action == EInputAction::FocusText);
    CHECK(!input.key(XKB_KEY_slash, 0, "/").handled());
    input.sync({.context = EInputContext::SpaceEditor, .mode = EInputMode::Insert, .field = "icon", .textEditable = true});
    CHECK(input.key(XKB_KEY_l, 0, "l").text == "l");
    CHECK(input.key(XKB_KEY_slash, 0, "/").text == "/");
    CHECK(input.key(XKB_KEY_u, InputCtrl, "").edit == ETextAction::Clear);
    CHECK(input.key(XKB_KEY_Escape, 0, "").action == EInputAction::FinishEditing);
    CHECK(input.key(XKB_KEY_Return, InputCtrl, "").action == EInputAction::Save);
    input.sync({.context = EInputContext::Spaces, .generation = 5});
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_r, 0, "r").action == EInputAction::UnknownPrefix);
    CHECK(input.key(XKB_KEY_Delete, 0, "").action == EInputAction::Remove);
    CHECK(!input.key(XKB_KEY_5, 0, "5").handled());
    input.key(XKB_KEY_space, 0, " ");
    input.sync({.context = EInputContext::SpacePicker, .generation = 6});
    CHECK(input.prefix() == CInputModel::EPrefix::None);
    CHECK(!input.key(XKB_KEY_s, 0, "s").handled());
    input.sync({.context = EInputContext::Help, .field = "query", .generation = 7, .textEditable = true, .protectedDraft = true});
    CHECK(!input.key(XKB_KEY_o, InputCtrl, "").handled());
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_v, 0, "v").action == EInputAction::UnknownPrefix);
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_question, InputShift, "?").action == EInputAction::Help);
    CHECK(input.key(XKB_KEY_slash, 0, "/").action == EInputAction::Search);
    section("input: Shift J/K reorders only spaces, while search keeps literal text");
    for (const auto context : {EInputContext::Spaces, EInputContext::SpacePicker}) {
        input.sync({.context = context, .mode = EInputMode::Normal, .field = "list", .textEditable = true});
        CHECK(input.key(XKB_KEY_J, InputShift, "J").action == EInputAction::ReorderLater);
        CHECK(input.key(XKB_KEY_j, InputShift, "J").action == EInputAction::ReorderLater);
        CHECK(input.key(XKB_KEY_K, InputShift, "K").action == EInputAction::ReorderEarlier);
        CHECK(input.key(XKB_KEY_k, InputShift, "K").action == EInputAction::ReorderEarlier);
        CHECK(input.key(XKB_KEY_j, 0, "j").action == EInputAction::MoveDown);
        CHECK(input.key(XKB_KEY_k, 0, "k").action == EInputAction::MoveUp);
        CHECK(!input.key(XKB_KEY_j, InputCtrl | InputShift, "").handled());
        input.sync({.context = context, .mode = EInputMode::Insert, .field = "query", .textEditable = true});
        CHECK(input.key(XKB_KEY_J, InputShift, "J").text == "J");
        CHECK(input.key(XKB_KEY_K, InputShift, "K").text == "K");
    }
    for (const auto context : {EInputContext::SpaceMenu, EInputContext::SpaceEditor, EInputContext::SpaceMove, EInputContext::SpaceRemove,
                               EInputContext::Workspaces, EInputContext::Windows, EInputContext::Help}) {
        input.sync({.context = context, .mode = EInputMode::Normal});
        CHECK(!input.key(XKB_KEY_J, InputShift, "J").handled());
        CHECK(!input.key(XKB_KEY_K, InputShift, "K").handled());
        CHECK(input.key(XKB_KEY_j, InputShift, "J").action != EInputAction::ReorderLater);
        CHECK(input.key(XKB_KEY_k, InputShift, "K").action != EInputAction::ReorderEarlier);
    }
    section("input: explicit window and whole-workspace leader transfers");
    for (const auto context : {EInputContext::Workspaces, EInputContext::Windows}) {
        input.sync({.context = context});
        input.key(XKB_KEY_space, 0, " ");
        CHECK(input.peek(XKB_KEY_m, 0, "m").action == EInputAction::MoveWindow);
        CHECK(input.prefix() == CInputModel::EPrefix::Leader);
        const auto moveWindow = input.key(XKB_KEY_m, 0, "m");
        CHECK(moveWindow.action == EInputAction::MoveWindow);
        CHECK(!moveWindow.repeatable);
        CHECK(input.prefix() == CInputModel::EPrefix::None);
        CHECK(!input.key(XKB_KEY_m, 0, "m").handled());
        for (const auto sym : {XKB_KEY_m, XKB_KEY_M}) {
            input.key(XKB_KEY_space, 0, " ");
            const auto moveWorkspace = input.key(sym, InputShift, "M");
            CHECK(moveWorkspace.action == EInputAction::MoveWorkspace);
            CHECK(!moveWorkspace.repeatable);
            CHECK(input.prefix() == CInputModel::EPrefix::None);
        }
        input.key(XKB_KEY_space, 0, " ");
        CHECK(input.key(XKB_KEY_M, 0, "M").action == EInputAction::MoveWorkspace);
        input.key(XKB_KEY_space, 0, " ");
        CHECK(!input.key(XKB_KEY_m, InputCtrl | InputShift, "").handled());
        CHECK(input.prefix() == CInputModel::EPrefix::None);
        input.key(XKB_KEY_space, 0, " ");
        CHECK(input.key(XKB_KEY_w, 0, "w").action == EInputAction::ShowWorkspaces);
        input.sync({.context = context, .mode = EInputMode::Insert, .field = "query", .textEditable = true});
        CHECK(input.key(XKB_KEY_space, 0, " ").text == " ");
        CHECK(input.key(XKB_KEY_m, 0, "m").text == "m");
        CHECK(input.key(XKB_KEY_m, InputShift, "M").text == "M");
        CHECK(input.key(XKB_KEY_M, 0, "M").text == "M");
        CHECK(input.prefix() == CInputModel::EPrefix::None);
    }
    for (const auto context : {EInputContext::Spaces, EInputContext::SpaceMenu, EInputContext::SpacePicker,
                               EInputContext::SpaceEditor, EInputContext::SpaceRemove, EInputContext::SpaceMove, EInputContext::Help,
                               EInputContext::Switcher}) {
        input.sync({.context = context});
        for (const auto action : {EInputAction::MoveWindow, EInputAction::MoveWorkspace})
            CHECK(!inputActionAvailable(action, input.context()));
        input.key(XKB_KEY_space, 0, " ");
        CHECK(input.key(XKB_KEY_m, 0, "m").action != EInputAction::MoveWindow);
        input.key(XKB_KEY_space, 0, " ");
        CHECK(input.key(XKB_KEY_m, InputShift, "M").action != EInputAction::MoveWorkspace);
    }
    input.sync({.context = EInputContext::Windows, .protectedDraft = true});
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_m, 0, "m").action == EInputAction::UnknownPrefix);
    input.key(XKB_KEY_space, 0, " ");
    CHECK(input.key(XKB_KEY_m, InputShift, "M").action == EInputAction::UnknownPrefix);
    for (const auto& binding : INPUT_LEADER_BINDINGS)
        CHECK(binding.action != EInputAction::Move);
    section("input: history shortcuts removed without changing navigation or text editing");
    for (const auto context : {EInputContext::Workspaces, EInputContext::Windows, EInputContext::Spaces, EInputContext::SpaceMenu,
                               EInputContext::SpacePicker, EInputContext::SpaceEditor, EInputContext::SpaceRemove, EInputContext::SpaceMove,
                               EInputContext::Help, EInputContext::Switcher}) {
        input.sync({.context = context, .textEditable = true});
        CHECK(!input.key(XKB_KEY_o, InputCtrl, "").handled());
        CHECK(!input.key(XKB_KEY_i, InputCtrl, "").handled());
        CHECK(input.key(XKB_KEY_h, 0, "h").action == EInputAction::MoveLeft);
        input.key(XKB_KEY_space, 0, " ");
        CHECK(input.key(XKB_KEY_h, 0, "h").action == (inputForm(context) ? EInputAction::MoveLeft : EInputAction::UnknownPrefix));
        CHECK(input.key(XKB_KEY_Tab, 0, "\t").action == EInputAction::NextField);
        input.sync({.context = context, .mode = EInputMode::Insert, .field = "query", .textEditable = true});
        CHECK(!input.key(XKB_KEY_o, InputCtrl, "").handled());
        CHECK(!input.key(XKB_KEY_i, InputCtrl, "").handled());
        for (const auto letter : std::string("hoi ")) {
            const auto command = input.key(letter, 0, std::string(1, letter));
            CHECK(command.text == std::string(1, letter));
            CHECK(command.action == EInputAction::None);
        }
        CHECK(input.key(XKB_KEY_Tab, 0, "\t").action == EInputAction::NextField);
    }
    for (const auto& binding : INPUT_LEADER_BINDINGS)
        CHECK(binding.key != 'h');
    input.reset();
    CHECK(input.prefix() == CInputModel::EPrefix::None);
    for (const auto& binding : INPUT_LEADER_BINDINGS) {
        input.sync({.context = EInputContext::Windows});
        input.key(XKB_KEY_space, 0, " ");
        const auto command = input.key(binding.key, 0, std::string(1, binding.key));
        CHECK(command.action == (inputActionAvailable(binding.action, EInputContext::Windows) ? binding.action : EInputAction::UnknownPrefix));
    }
}
