#include "SpaceHooks.hpp"

#include "CompositorHooks.hpp"
#include "Spaces.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspacePlacementController.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace hyprspace::spacehooks {
    namespace {
        CFunctionHook* parserHook       = nullptr;
        CFunctionHook* matcherHook      = nullptr;
        CFunctionHook* activateHook     = nullptr;
        CFunctionHook* specialHook      = nullptr;
        CFunctionHook* changeHook       = nullptr;
        CFunctionHook* renameHook       = nullptr;
        CFunctionHook* renumberHook     = nullptr;
        CFunctionHook* pinHook          = nullptr;
        CFunctionHook* persistentHook   = nullptr;
        CFunctionHook* boundMonitorHook = nullptr;
        CFunctionHook* focusActionHook  = nullptr;
        CFunctionHook* fullFocusHook    = nullptr;
        CFunctionHook* rawFocusHook     = nullptr;
        CFunctionHook* monitorFocusHook = nullptr;

        using ActionResult  = Config::Actions::ActionResult;
        using MoveWorkspace = State::CWorkspacePlacementController::FMoveWorkspace;
        using Rules         = std::vector<SP<Config::CWorkspaceRule>>;

        std::string trim(std::string_view value) {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.remove_prefix(1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.remove_suffix(1);
            return std::string(value);
        }

        ActionResult actionResult(const SDispatchResult& result) {
            if (!result.success)
                return Config::Actions::actionError(result.error);
            return Config::Actions::SActionResult{};
        }

        SWorkspaceIDName parse(const std::string& selector) {
            using Fn = SWorkspaceIDName (*)(const std::string&);
            if (spaces::enabled() && !spaces::internal())
                if (const auto resolved = spaces::resolve(selector))
                    return *resolved;
            return reinterpret_cast<Fn>(parserHook->m_original)(selector);
        }

        bool matches(CWorkspace* self, const std::string& selector) {
            using Fn            = bool (*)(CWorkspace*, const std::string&);
            const auto original = reinterpret_cast<Fn>(matcherHook->m_original);
            if (!spaces::enabled() || spaces::nativeDefault())
                return original(self, selector);
            const auto workspace = self->m_self.lock();
            if (!workspace || spaces::owner(workspace).empty())
                return original(self, selector);
            const auto text = trim(selector);
            if (text.size() < 2 || text[1] != '[') {
                bool       handled  = false;
                const bool identity = spaces::matchesIdentity(workspace, text, handled);
                if (handled)
                    return identity;
            }

            // Native selectors concatenate clauses such as r[1-5] m[DP-1]
            // w[t1]. Only identity clauses change meaning in a space. Let
            // Hyprland evaluate the remaining live monitor/window properties.
            if (text.empty())
                return original(self, text);
            std::string dynamic;
            size_t      position  = 0;
            bool        sawClause = false;
            while (position < text.size()) {
                while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
                    ++position;
                if (position == text.size())
                    break;
                const auto close = text.find(']', position);
                if (position + 1 >= text.size() || text[position + 1] != '[' || close == std::string::npos)
                    return false;
                const auto clause        = text.substr(position, close - position + 1);
                bool       clauseHandled = false;
                const bool clauseMatches = spaces::matchesIdentity(workspace, clause, clauseHandled);
                if (clauseHandled) {
                    if (!clauseMatches)
                        return false;
                } else {
                    if (!dynamic.empty())
                        dynamic += ' ';
                    dynamic += clause;
                }
                sawClause = true;
                position  = close + 1;
            }
            return sawClause && (dynamic.empty() || original(self, dynamic));
        }

        bool activateOwner(PHLWORKSPACE workspace) {
            if (!workspace || !spaces::enabled() || spaces::internal())
                return true;
            spaces::observe(workspace);
            const auto id = spaces::owner(workspace);
            return id.empty() || id == spaces::activeId() || spaces::switchTo(id).success;
        }

        void activate(Monitor::CMonitor* self, const PHLWORKSPACE& destination, bool internal, bool noMouseMove, bool noFocus) {
            using Fn            = void (*)(Monitor::CMonitor*, const PHLWORKSPACE&, bool, bool, bool);
            const auto original = reinterpret_cast<Fn>(activateHook->m_original);
            if (spaces::backgroundContext())
                return;
            if (!spaces::enabled() || spaces::internal() || !destination) {
                original(self, destination, internal, noMouseMove, noFocus);
                return;
            }
            spaces::observe(destination);
            const auto owner = spaces::owner(destination);
            if (internal && !owner.empty() && owner != spaces::activeId()) {
                // Monitor reconnect restores a remembered native workspace.
                // It must not change the space on every other output.
                const auto monitor     = self->m_self.lock();
                const auto replacement = spaces::workspace(std::max<int64_t>(1, spaces::localNumber(destination)), monitor);
                if (replacement)
                    original(self, replacement, internal, noMouseMove, noFocus);
                return;
            }
            if (!activateOwner(destination))
                return;
            original(self, destination, internal, noMouseMove, noFocus);
            spaces::changed(destination);
        }

        void activateSpecial(Monitor::CMonitor* self, const PHLWORKSPACE& destination) {
            using Fn = void (*)(Monitor::CMonitor*, const PHLWORKSPACE&);
            if (spaces::backgroundContext())
                return;
            if (!activateOwner(destination))
                return;
            reinterpret_cast<Fn>(specialHook->m_original)(self, destination);
            if (destination && spaces::enabled() && !spaces::internal())
                spaces::changed(destination);
        }

        ActionResult changeWorkspace(const std::string& selector) {
            using Fn = ActionResult (*)(const std::string&);
            if (!spaces::enabled() || spaces::internal() || spaces::nativeDefault())
                return reinterpret_cast<Fn>(changeHook->m_original)(selector);
            const auto monitor = Desktop::focusState()->monitor();
            if (!monitor)
                return Config::Actions::actionError("No focused monitor");
            auto resolved = spaces::resolve(selector, monitor);
            if (!resolved)
                return reinterpret_cast<Fn>(changeHook->m_original)(selector);
            if (resolved->id == WORKSPACE_INVALID || resolved->id == WORKSPACE_NOT_CHANGED)
                return Config::Actions::actionError("No workspace matches in the active space");
            static const auto backAndForth = CConfigValue<Config::INTEGER>("binds:workspace_back_and_forth");
            if (!trim(selector).starts_with("prev") && monitor->m_activeWorkspace && resolved->id == monitor->m_activeWorkspace->m_id && *backAndForth) {
                resolved = spaces::resolve("previous", monitor);
                if (!resolved || resolved->id == WORKSPACE_INVALID || resolved->id == WORKSPACE_NOT_CHANGED)
                    return Config::Actions::actionError("No previous workspace in the active space");
            }
            auto workspace = State::workspaceState()->query().id(resolved->id).run();
            if (!workspace)
                workspace = State::workspaceState()->create(resolved->id, monitor->m_id, resolved->name);
            if (!valid(workspace))
                return Config::Actions::actionError("Could not create workspace in the active space");
            return Config::Actions::changeWorkspace(workspace);
        }

        ActionResult renameWorkspace(PHLWORKSPACE workspace, const std::string& name) {
            using Fn = ActionResult (*)(PHLWORKSPACE, const std::string&);
            if (spaces::enabled() && !spaces::internal() && !spaces::nativeDefault() && workspace && !spaces::owner(workspace).empty())
                return actionResult(spaces::renameWorkspace(workspace, name));
            const auto result = reinterpret_cast<Fn>(renameHook->m_original)(workspace, name);
            if (result && workspace && spaces::enabled() && !spaces::internal())
                spaces::changed(workspace);
            return result;
        }

        ActionResult renumberWorkspace(PHLWORKSPACE workspace, int64_t number) {
            using Fn = ActionResult (*)(PHLWORKSPACE, int64_t);
            if (spaces::enabled() && !spaces::internal() && !spaces::nativeDefault() && workspace && !spaces::owner(workspace).empty())
                return actionResult(spaces::renumberWorkspace(workspace, number));
            const auto result = reinterpret_cast<Fn>(renumberHook->m_original)(workspace, number);
            if (result && workspace && spaces::enabled() && !spaces::internal())
                spaces::changed(workspace);
            return result;
        }

        ActionResult pinWindow(Config::Actions::eTogglableAction action, std::optional<PHLWINDOW> target) {
            using Fn          = ActionResult (*)(Config::Actions::eTogglableAction, std::optional<PHLWINDOW>);
            const auto window = target ? *target : Desktop::focusState()->window();
            if (spaces::enabled() && !spaces::internal() && window && window->m_workspace && !spaces::inActive(window->m_workspace)) {
                // The native action assigns even an unpinned target to the
                // monitor's current workspace. A hidden space's pin request
                // changes its desired state without moving the window here.
                if (!window->m_isFloating || Fullscreen::controller()->isFullscreen(window))
                    return Config::Actions::actionError("Window does not qualify to be pinned");
                bool desired = spaces::desiredPin(window);
                if (action == Config::Actions::TOGGLE_ACTION_TOGGLE)
                    desired = !desired;
                else
                    desired = action == Config::Actions::TOGGLE_ACTION_ENABLE;
                spaces::rememberPin(window, desired);
                return Config::Actions::SActionResult{};
            }
            const auto result = reinterpret_cast<Fn>(pinHook->m_original)(action, target);
            if (result && window && spaces::enabled() && !spaces::internal())
                spaces::rememberPin(window, window->m_pinned);
            return result;
        }

        bool deferBackgroundFocus(PHLWINDOW window) {
            if (!spaces::backgroundContext())
                return false;
            if (window && !window->m_isUrgent) {
                window->m_isUrgent = true;
                Event::bus()->m_events.window.urgent.emit(window);
                if (g_pEventManager)
                    g_pEventManager->postEvent({.event = "urgent", .data = std::format("{:x}", reinterpret_cast<uintptr_t>(window.get()))});
            }
            return true;
        }

        ActionResult focus(PHLWINDOW window) {
            using Fn = ActionResult (*)(PHLWINDOW);
            if (deferBackgroundFocus(window))
                return Config::Actions::SActionResult{};
            return reinterpret_cast<Fn>(focusActionHook->m_original)(window);
        }

        void fullFocus(Desktop::CFocusState* self, PHLWINDOW window, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface, bool cycle) {
            using Fn = void (*)(Desktop::CFocusState*, PHLWINDOW, Desktop::eFocusReason, SP<CWLSurfaceResource>, bool);
            if (!deferBackgroundFocus(window))
                reinterpret_cast<Fn>(fullFocusHook->m_original)(self, window, reason, surface, cycle);
        }

        void rawFocus(Desktop::CFocusState* self, PHLWINDOW window, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface) {
            using Fn = void (*)(Desktop::CFocusState*, PHLWINDOW, Desktop::eFocusReason, SP<CWLSurfaceResource>);
            if (!deferBackgroundFocus(window))
                reinterpret_cast<Fn>(rawFocusHook->m_original)(self, window, reason, surface);
        }

        void monitorFocus(Desktop::CFocusState* self, PHLMONITOR monitor) {
            using Fn = void (*)(Desktop::CFocusState*, PHLMONITOR);
            if (!spaces::backgroundContext())
                reinterpret_cast<Fn>(monitorFocusHook->m_original)(self, monitor);
        }

        void persistent(const State::CWorkspacePlacementController* self, const Rules& rules, PHLWORKSPACE workspace, const MoveWorkspace& move) {
            using Fn            = void (*)(const State::CWorkspacePlacementController*, const Rules&, PHLWORKSPACE, const MoveWorkspace&);
            const auto original = reinterpret_cast<Fn>(persistentHook->m_original);
            if (!spaces::enabled() || spaces::internal() || spaces::nativeDefault()) {
                original(self, rules, workspace, move);
                return;
            }
            Rules mapped;
            // One combined call is essential: the native implementation also
            // downgrades persistent workspaces missing from its complete set.
            for (const auto& space : spaces::list()) {
                if (workspace && spaces::owner(workspace) != space.id)
                    continue;
                spaces::CSpaceScope context(space.id);
                for (const auto& rule : rules) {
                    if (!rule || !rule->isEnabled() || !rule->m_isPersistent.value_or(false))
                        continue;
                    if (workspace && !matches(workspace.get(), rule->m_workspaceString))
                        continue;
                    if (workspace) {
                        auto copy               = makeShared<Config::CWorkspaceRule>(*rule);
                        copy->m_workspaceId     = workspace->m_id;
                        copy->m_workspaceName   = workspace->m_name;
                        copy->m_workspaceString = workspace->m_isSpecialWorkspace ? workspace->m_name : "name:" + workspace->m_name;
                        mapped.push_back(std::move(copy));
                        continue;
                    }
                    const auto resolved = spaces::resolve(rule->m_workspaceString, workspace ? workspace->m_monitor.lock() : nullptr);
                    if (!resolved) {
                        mapped.push_back(makeShared<Config::CWorkspaceRule>(*rule));
                        continue;
                    }
                    if (resolved->id == WORKSPACE_INVALID || resolved->id == WORKSPACE_NOT_CHANGED)
                        continue;
                    auto copy               = makeShared<Config::CWorkspaceRule>(*rule);
                    copy->m_workspaceId     = workspace ? workspace->m_id : resolved->id;
                    copy->m_workspaceName   = workspace ? workspace->m_name : resolved->name;
                    copy->m_workspaceString = copy->m_workspaceName.starts_with("special:") ? copy->m_workspaceName : "name:" + copy->m_workspaceName;
                    mapped.push_back(std::move(copy));
                }
            }
            spaces::CNativeScope native;
            original(self, mapped, workspace, move);
        }

        std::string boundMonitor(Config::CWorkspaceRuleManager* self, const std::string& name) {
            using Fn            = std::string (*)(Config::CWorkspaceRuleManager*, const std::string&);
            const auto original = reinterpret_cast<Fn>(boundMonitorHook->m_original);
            if (!spaces::enabled() || spaces::nativeDefault())
                return original(self, name);
            const auto logical = spaces::logicalSelector(name);
            if (!logical)
                return original(self, name);
            for (const auto& rule : self->getAllWorkspaceRules()) {
                if (!rule || !rule->isEnabled())
                    continue;
                if (trim(rule->m_workspaceString) == *logical)
                    return rule->m_monitor;
            }
            return "";
        }
    } // namespace

    SDispatchResult dispatch(const std::string& name, std::string args, const std::function<SDispatchResult(std::string)>& original) {
        if (spaces::enabled() && !spaces::internal() && !spaces::nativeDefault() && name == "renameworkspace") {
            // The legacy translator uses stoi/query directly, before reaching
            // the native rename action. Adapt its first token to the backing ID.
            const auto separator = args.find_first_of(" \t");
            const auto selector  = args.substr(0, separator);
            const auto resolved  = spaces::resolve(selector);
            if (resolved) {
                if (resolved->id == WORKSPACE_INVALID)
                    return {.success = false, .error = "hyprspace: no workspace to rename"};
                args = std::to_string(resolved->id) + (separator == std::string::npos ? "" : " " + trim(std::string_view(args).substr(separator)));
            }
        }
        return original(std::move(args));
    }

    void install() {
        try {
            parserHook     = hooks::attach("getWorkspaceIDNameFromString", "getWorkspaceIDNameFromString(", reinterpret_cast<void*>(parse));
            matcherHook    = hooks::attach("matchesStaticSelector", "CWorkspace::matchesStaticSelector(", reinterpret_cast<void*>(matches));
            activateHook   = hooks::attach("changeWorkspace", "Monitor::CMonitor::changeWorkspace(Hyprutils::Memory::CSharedPointer<CWorkspace> const&",
                                           reinterpret_cast<void*>(activate));
            specialHook    = hooks::attach("setSpecialWorkspace", "Monitor::CMonitor::setSpecialWorkspace(Hyprutils::Memory::CSharedPointer<CWorkspace> const&",
                                           reinterpret_cast<void*>(activateSpecial));
            changeHook     = hooks::attach("changeWorkspace", "Config::Actions::changeWorkspace(std::", reinterpret_cast<void*>(changeWorkspace));
            renameHook     = hooks::attach("renameWorkspace", "Config::Actions::renameWorkspace(", reinterpret_cast<void*>(renameWorkspace));
            renumberHook   = hooks::attach("changeWorkspaceID", "Config::Actions::changeWorkspaceID(", reinterpret_cast<void*>(renumberWorkspace));
            pinHook        = hooks::attach("pinWindow", "Config::Actions::pinWindow(", reinterpret_cast<void*>(pinWindow));
            persistentHook = hooks::attach("ensurePersistentWorkspacesPresent", "State::CWorkspacePlacementController::ensurePersistentWorkspacesPresent(std::vector<",
                                           reinterpret_cast<void*>(persistent));
            boundMonitorHook =
                hooks::attach("getBoundMonitorStringForWS", "Config::CWorkspaceRuleManager::getBoundMonitorStringForWS(", reinterpret_cast<void*>(boundMonitor));
            focusActionHook  = hooks::attach("focus", "Config::Actions::focus(", reinterpret_cast<void*>(focus));
            fullFocusHook    = hooks::attach("fullWindowFocus", "Desktop::CFocusState::fullWindowFocus(", reinterpret_cast<void*>(fullFocus));
            rawFocusHook     = hooks::attach("rawWindowFocus", "Desktop::CFocusState::rawWindowFocus(", reinterpret_cast<void*>(rawFocus));
            monitorFocusHook = hooks::attach("rawMonitorFocus", "Desktop::CFocusState::rawMonitorFocus(", reinterpret_cast<void*>(monitorFocus));
        } catch (...) {
            uninstall();
            throw;
        }
    }

    void uninstall() {
        for (auto** hook : {&monitorFocusHook, &rawFocusHook, &fullFocusHook, &focusActionHook, &boundMonitorHook, &persistentHook, &pinHook, &renumberHook, &renameHook,
                            &changeHook, &specialHook, &activateHook, &matcherHook, &parserHook}) {
            if (*hook)
                HyprlandAPI::removeFunctionHook(PHANDLE, *hook);
            *hook = nullptr;
        }
    }
} // namespace hyprspace::spacehooks
