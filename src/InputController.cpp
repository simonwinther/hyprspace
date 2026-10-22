#include "InputController.hpp"
#include "SpaceSelector.hpp"
#include "HelpOverlay.hpp"
#include "Overview.hpp"
#include "Config.hpp"
#include "PassElements.hpp"
#include "SpaceStyle.hpp"
#include "Texture.hpp"
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/output/Monitor.hpp>

namespace hyprspace {
    CInputController& inputController() {
        static CInputController input;
        return input;
    }
    namespace {
        uint32_t modifiers(uint32_t native) {
            return ((native & HL_MODIFIER_SHIFT) ? InputShift : 0) | ((native & HL_MODIFIER_CTRL) ? InputCtrl : 0) | ((native & HL_MODIFIER_ALT) ? InputAlt : 0) |
                   ((native & HL_MODIFIER_META) ? InputSuper : 0);
        }
    } // namespace
    SInputContext CInputController::context() const {
        if (session().helpActive()) {
            auto state           = session().helpOverlay()->inputContext();
            state.protectedDraft = spaceSelector().modal() && inputForm(spaceSelector().inputContext().context);
            return state;
        }
        if (spaceSelector().modal())
            return spaceSelector().inputContext();
        if (session().windowViewActive())
            return session().windowBoard()->inputContext();
        return {.context = EInputContext::Workspaces, .generation = m_generation};
    }
    void CInputController::reset() {
        m_parents.clear();
        ++m_generation;
        m_model.reset();
    }
    uint64_t CInputController::epoch() {
        m_model.sync(context());
        return m_model.epoch();
    }
    SInputCommand CInputController::preview(xkb_keysym_t sym, uint32_t native, const std::string& text) {
        m_model.sync(context());
        const auto command = m_model.peek(sym, modifiers(native), text);
        if (!command.handled() && context().mode == EInputMode::Normal && !(native & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)) &&
            config::overviewHelpKey() != XKB_KEY_NoSymbol && xkb_keysym_to_lower(sym) == config::overviewHelpKey())
            return {.action = EInputAction::Help};
        return command;
    }
    SInputCommand CInputController::onKey(xkb_keysym_t sym, uint32_t native, const std::string& text) {
        m_model.sync(context());
        const auto alias   = preview(sym, native, text);
        auto       command = m_model.key(sym, modifiers(native), text);
        if (!command.handled() && alias.handled())
            command = alias;
        if (command.edit || !command.text.empty()) {
            if (session().helpActive()) {
                if (command.edit)
                    session().helpOverlay()->editInput(*command.edit, command.extend);
                else
                    session().helpOverlay()->insertText(command.text);
            } else if (spaceSelector().modal()) {
                if (command.edit)
                    spaceSelector().editInput(*command.edit, command.extend);
                else
                    spaceSelector().insertText(command.text);
            } else if (session().windowViewActive()) {
                if (command.edit)
                    session().windowBoard()->editInput(*command.edit, command.extend);
                else
                    session().windowBoard()->insertText(command.text);
            }
        } else if (command.action == EInputAction::WorkspaceDigit) {
            if (session().windowViewActive())
                (void)session().windowView("off");
            if (auto* view = session().keyboardView()) {
                view->workspaceDigit(command.digit);
                if (view->closing()) {
                    for (const auto& other : session().views)
                        other->close(false);
                    session().stopInput();
                }
            }
        } else if (command.handled())
            perform(command.action);
        session().damage();
        return command;
    }
    void CInputController::saveCanvas() {
        SParent parent{false, std::nullopt};
        if (session().windowViewActive())
            parent = {true, session().windowBoard()->snapshotInput()};
        if (!m_parents.empty()) {
            const auto& previous = m_parents.back();
            if (!parent.board && !previous.board)
                return;
            if (parent.snapshot && previous.snapshot) {
                const auto& a = *parent.snapshot;
                const auto& b = *previous.snapshot;
                if (a.search.text() == b.search.text() && a.search.cursor() == b.search.cursor() && a.search.selection() == b.search.selection() &&
                    a.grouping == b.grouping && a.recent == b.recent && a.selected == b.selected && a.scroll == b.scroll && a.mode == b.mode)
                    return;
            }
        }
        if (m_parents.size() == 16)
            m_parents.erase(m_parents.begin());
        m_parents.push_back(std::move(parent));
    }
    void CInputController::back() {
        if (session().helpActive()) {
            session().closeHelp();
            return;
        }
        if (spaceSelector().modal()) {
            spaceSelector().perform(EInputAction::Back);
            return;
        }
        if (!m_parents.empty()) {
            auto parent = std::move(m_parents.back());
            m_parents.pop_back();
            if (parent.board && parent.snapshot) {
                if (!session().windowViewActive())
                    (void)session().windowView("flat");
                session().windowBoard()->restoreInput(*parent.snapshot);
            } else if (session().windowViewActive())
                (void)session().windowView("off");
            ++m_generation;
            return;
        }
        if (session().windowViewActive()) {
            (void)session().windowView("off");
            return;
        }
        if (session().gestureActive()) {
            session().cancelDrag();
            session().cancelWorkspaceDrag();
            return;
        }
        for (const auto& view : session().views)
            view->close(false);
        session().stopInput();
    }
    bool CInputController::perform(EInputAction action) {
        const auto current = context();
        if (action == EInputAction::Prefix || action == EInputAction::CancelPrefix || action == EInputAction::UnknownPrefix)
            return true;
        if (session().gestureActive() || session().pendingResize || session().panHeld()) {
            if (action == EInputAction::Back) {
                session().cancelDrag();
                session().cancelPan();
            }
            return true;
        }
        if (action == EInputAction::Back) {
            back();
            return true;
        }
        if (action == EInputAction::Help) {
            (void)session().help();
            return true;
        }
        if (inputForm(current.context))
            return spaceSelector().perform(action);
        if (session().helpActive() && spaceSelector().modal() && inputForm(spaceSelector().inputContext().context))
            return session().helpOverlay()->perform(action);
        switch (action) {
        case EInputAction::ToggleSpaces:
            if (session().helpActive())
                session().closeHelp();
            spaceSelector().toggleSidebar();
            return true;
        case EInputAction::OpenManager:
            if (session().helpActive())
                session().closeHelp();
            spaceSelector().openManager();
            return true;
        case EInputAction::Edit:
        case EInputAction::Create:
        case EInputAction::Move:
        case EInputAction::MoveWindow:
        case EInputAction::MoveWorkspace:
        case EInputAction::Remove:
            return spaceSelector().perform(action);
        case EInputAction::PreviousSpace:
            (void)spaces::dispatch("prev");
            return true;
        case EInputAction::NextSpace:
            (void)spaces::dispatch("next");
            return true;
        case EInputAction::ShowWorkspaces:
            spaceSelector().close();
            session().closeHelp();
            if (session().windowViewActive())
                (void)session().windowView("off");
            m_parents.clear();
            ++m_generation;
            return true;
        case EInputAction::ShowWindows:
            spaceSelector().close();
            session().closeHelp();
            if (!session().windowViewActive()) {
                saveCanvas();
                (void)session().windowView("flat");
            }
            return true;
        case EInputAction::NewWorkspace:
            (void)session().emptyWorkspace();
            return true;
        case EInputAction::CloseWorkspace:
            (void)session().closeWorkspace();
            return true;
        default:
            break;
        }
        if (session().helpActive())
            return session().helpOverlay()->perform(action);
        if (spaceSelector().modal())
            return spaceSelector().perform(action);
        if (action == EInputAction::Search) {
            saveCanvas();
            if (!session().windowViewActive())
                (void)session().windowView("flat");
            return session().windowViewActive() && session().windowBoard()->perform(action);
        }
        if (session().windowViewActive())
            return session().windowBoard()->perform(action);
        if (auto* view = session().keyboardView()) {
            const auto handled = view->perform(action);
            if (action != EInputAction::Back)
                session().keyboard(*view);
            if (view->closing()) {
                for (const auto& other : session().views)
                    other->close(false);
                session().stopInput();
            }
            return handled;
        }
        return false;
    }
    nlohmann::json CInputController::inspect() {
        epoch();
        const auto     state    = context();
        nlohmann::json commands = nlohmann::json::array();
        for (const auto& binding : INPUT_LEADER_BINDINGS)
            if (inputActionAvailable(binding.action, state))
                commands.push_back({{"key", std::string(1, binding.key)}, {"label", binding.label}});
        return {{"mode", inputModeName(state.mode)},
                {"context", inputContextName(state.context)},
                {"field", state.field},
                {"epoch", m_model.epoch()},
                {"prefix", m_model.prefixName()},
                {"parents", m_parents.size()},
                {"commands", commands}};
    }
    std::vector<UP<IPassElement>> CInputController::buildPass(PHLMONITOR monitor) {
        std::vector<UP<IPassElement>> out;
        if (!session().live() || !session().cursorOwned() || !monitor)
            return out;
        const auto view   = session().keyboardView();
        const auto target = session().helpActive()         ? session().helpOverlay()->monitor()
                            : spaceSelector().modal()      ? spaceSelector().monitor()
                            : session().windowViewActive() ? session().windowBoard()->monitor()
                            : view                         ? view->monitor()
                                                           : nullptr;
        if (monitor != target)
            return out;
        epoch();
        const auto prefix = m_model.prefix();
        if (prefix == CInputModel::EPrefix::None)
            return out;
        const auto                   rgba = [](const CHyprColor& c) { return SRgba{c.r, c.g, c.b, c.a}; };
        const auto                   bg = rgba(config::overviewBgColor()), ink = rgba(config::overviewLabelColor()), accent = rgba(config::overviewActiveBorder());
        const std::array<double, 12> colors{bg.r, bg.g, bg.b, bg.a, ink.r, ink.g, ink.b, ink.a, accent.r, accent.g, accent.b, accent.a};
        const auto                   theme = config::spacesTheme();
        struct SStyle {
            std::array<double, 12> colors;
            std::string            theme;
            spaceStyle::SPalette   palette;
        };
        static std::optional<SStyle> cached;
        if (!cached || cached->colors != colors || cached->theme != theme)
            cached = SStyle{colors, theme, spaceStyle::palette(bg, {}, ink, accent, theme)};
        const auto& palette = cached->palette;
        const auto  color   = [](const SRgba& c) { return CHyprColor{c.r, c.g, c.b, c.a}; };
        const auto  scale   = monitor->m_scale;
        const auto  text    = [&](const std::string& label, const SBoxF& box, const SRgba& ink) {
            if (auto texture = textures().text(label, config::overviewFont(), color(ink), box.w, scale * .8)) {
                const auto size = texture->m_size / scale;
                out.emplace_back(makeUnique<CTexPassElement>(
                    CTexPassElement::SRenderData{.tex = texture, .box = CBox{box.x * scale, (box.cy() - size.y / 2) * scale, size.x * scale, size.y * scale}, .a = 1}));
            }
        };
        const auto   state        = context();
        const double pendingWidth = std::min(330.0, monitor->m_size.x - monitor->m_reservedArea.left() - monitor->m_reservedArea.right() - 40);
        const double x      = spaceSelector().modal() ? monitor->m_size.x - monitor->m_reservedArea.right() - 20 - pendingWidth : monitor->m_reservedArea.left() + 20;
        const double bottom = monitor->m_size.y - monitor->m_reservedArea.bottom() - 16;
        if (prefix == CInputModel::EPrefix::G) {
            const SBoxF pending{x, bottom - 28, pendingWidth, 28};
            renderPass::appendSolidFill(out, pending, color(palette.surface), scale, 6);
            text("g  ·  g First    Esc Cancel", {x + 10, pending.y, pending.w - 20, pending.h}, palette.muted);
        }
        if (prefix == CInputModel::EPrefix::Leader) {
            std::vector<SInputBinding> bindings;
            for (const auto& binding : INPUT_LEADER_BINDINGS)
                if (inputActionAvailable(binding.action, state))
                    bindings.push_back(binding);
            const int    columns = bottom - monitor->m_reservedArea.top() < 440 ? 2 : 1;
            const int    rows    = (bindings.size() + columns - 1) / columns;
            const double width = std::min(columns == 2 ? 620.0 : 370.0, monitor->m_size.x - 40), height = 52 + rows * 27;
            const double popupX = std::min(x, monitor->m_size.x - monitor->m_reservedArea.right() - 20 - width);
            const SBoxF  popup{popupX, bottom - height, width, height};
            renderPass::appendSolidFill(out, popup, color(palette.surface), scale, 9);
            renderPass::appendOutline(out, popup, color(palette.border), scale, 1, 9);
            text("SPACE  ·  Commands", {popupX + 14, popup.y + 8, width - 28, 28}, palette.text);
            for (size_t i = 0; i < bindings.size(); ++i) {
                const auto&  binding     = bindings[i];
                const double columnWidth = width / columns;
                text(std::string(1, binding.key) + "    " + std::string(binding.label),
                     {popupX + 14 + (i / rows) * columnWidth, popup.y + 36 + (i % rows) * 27, columnWidth - 28, 27}, palette.text);
            }
            text("? Help     Esc Cancel", {popupX + 14, popup.y + height - 20, width - 28, 20}, palette.muted);
        }
        return out;
    }
} // namespace hyprspace
