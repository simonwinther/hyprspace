#include "Spaces.hpp"

#include "Config.hpp"
#include "Geometry.hpp"
#include "OverlayPolicy.hpp"
#include "OverviewSession.hpp"
#include "SpaceModel.hpp"
#include "SpaceStore.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspacePlacementController.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace hyprspace::spaces {
    namespace {
        std::unique_ptr<CSpaceModel> model;
        std::unique_ptr<CSpaceStore> store;
        bool                         ready = false, switching = false, topologyPending = false, topologyChanging = false, membershipDirty = true;
        bool                         startupRestore = false;
        unsigned                     nativeDepth    = 0;
        std::string                  context, pendingSwitch, lastError;
        std::function<void()>        beforeTransition, afterTransition;
        std::unordered_map<const CWorkspace*, std::pair<PHLWORKSPACEREF, std::string>> identities;
        std::unordered_map<std::string, PHLWORKSPACEREF>                               natives;
        std::unordered_map<int64_t, std::string>                                       constructing;
        struct SPin {
            PHLWINDOWREF window;
            bool         desired = false;
        };
        std::unordered_map<const Desktop::View::CWindow*, SPin>    pins;
        std::unordered_map<std::string, PHLWINDOWREF>              focused;
        std::map<std::pair<std::string, std::string>, std::string> previous;
        std::vector<PHLWORKSPACEREF>                               activeWorkspaces;
        std::vector<PHLWINDOWREF>                                  activeWindows;
        uint64_t                                                   membershipBuilds = 0, generation = 0, switches = 0;
        uint64_t                                                   lastSwitchGeneration = 0;
        double                                                     lastSwitchStart      = 0;
        std::map<std::string, double>                              lastSwitchFrames;
        std::vector<CHyprSignalListener>                           listeners;
        UP<SEventLoopDoLaterLock>                                  changeNotification;
        bool                                                       changeNotificationQueued = false;

        SDispatchResult fail(std::string message) {
            lastError = std::move(message);
            return {.success = false, .error = "hyprspace: " + lastError};
        }

        std::optional<int64_t> number(std::string_view text) {
            int64_t value           = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        std::string monitorKey(PHLMONITOR monitor) {
            if (!monitor)
                return {};
            const auto output = monitor->m_output;
            if (output && !output->serial.empty())
                return "serial:" + output->make + ":" + output->model + ":" + output->serial;
            return monitor->m_name;
        }

        std::vector<PHLMONITOR> monitors() {
            std::vector<PHLMONITOR> result;
            for (const auto& monitor : State::monitorState()->monitors())
                if (monitor && monitor->m_enabled && !monitor->isMirror())
                    result.push_back(monitor);
            return result;
        }

        PHLMONITOR defaultMonitor() {
            const auto current   = Desktop::focusState()->monitor();
            const auto connected = monitors();
            if (std::ranges::find(connected, current) != connected.end())
                return current;
            return connected.empty() ? nullptr : connected.front();
        }

        const std::string& destinationSpace() {
            return model && model->space(context) ? context : model->active;
        }

        void notifyChanged() {
            if (!ready || changeNotificationQueued || !g_pEventLoopManager)
                return;
            changeNotificationQueued = true;
            changeNotification       = g_pEventLoopManager->doLaterLock([] {
                changeNotificationQueued = false;
                if (ready && model && g_pEventManager)
                    g_pEventManager->postEvent({.event = "custom", .data = "hyprspace:spacechanged," + activeId()});
            });
        }

        void invalidate() {
            ++generation;
            membershipDirty = true;
            activeWorkspaces.clear();
            activeWindows.clear();
            notifyChanged();
        }

        void persist() {
            if (store && model)
                store->submit(*model);
            notifyChanged();
        }

        std::string pinKey(PHLWINDOW window) {
            const auto signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
            return std::string(signature ? signature : "") + ":" + std::to_string(window->m_stableID) + "/" +
                   std::format("{:x}", reinterpret_cast<uintptr_t>(window.get())) + "/" + std::to_string(window->getPID());
        }

        void savePin(PHLWINDOW window, bool desired) {
            pins[window.get()] = {window, desired};
            if (desired)
                model->runtimePins[pinKey(window)] = true;
            else
                model->runtimePins.erase(pinKey(window));
        }

        void assignAlias(SSpaceWorkspace& item, std::string name) {
            if (item.special && name.starts_with("special:"))
                name.erase(0, 8);
            if (!validName(name))
                return;
            const auto base = name;
            for (size_t suffix = 2;; ++suffix) {
                const auto other = model->named(item.space, name, item.special);
                if (!other || other->id == item.id)
                    break;
                const auto tail   = " (" + std::to_string(suffix) + ")";
                size_t     length = std::min(base.size(), 256 - tail.size());
                while (length < base.size() && length && (static_cast<unsigned char>(base[length]) & 0xc0) == 0x80)
                    --length;
                name = base.substr(0, length) + tail;
            }
            item.name = std::move(name);
        }

        SSpaceWorkspace* record(PHLWORKSPACE workspace, bool adopt = true) {
            if (!model || !workspace)
                return nullptr;
            const auto cached = identities.find(workspace.get());
            if (cached != identities.end() && cached->second.first.lock() == workspace) {
                const auto result = model->workspace(cached->second.second);
                if (result && result->nativeName != workspace->m_name) {
                    const bool managed = result->nativeName.starts_with("hyprspace:") || result->nativeName.starts_with("special:hyprspace:");
                    assignAlias(*result, workspace->m_name);
                    if (!managed)
                        result->nativeName = workspace->m_name;
                }
                return result;
            }
            SSpaceWorkspace* result = nullptr;
            if (const auto pending = constructing.find(workspace->m_id); pending != constructing.end())
                result = model->workspace(pending->second);
            for (auto& candidate : model->workspaces)
                if (!result)
                    if (!candidate.nativeName.empty() && candidate.nativeName == workspace->m_name && candidate.special == workspace->m_isSpecialWorkspace &&
                        (workspace->m_id <= 0 || candidate.nativeId == workspace->m_id)) {
                        result = &candidate;
                        break;
                    }
            if (!result) {
                const auto candidate = model->byNative(workspace->m_id);
                if (candidate && (candidate->nativeName == workspace->m_name || workspace->m_id > 0))
                    result = candidate;
                else if (candidate && workspace->m_id < 0) {
                    const auto previous = natives.find(candidate->id);
                    const auto live     = previous == natives.end() ? nullptr : previous->second.lock();
                    if (!valid(live))
                        candidate->nativeId = 0;
                }
            }
            if (!result && adopt) {
                auto        local = workspace->m_id > 0 ? workspace->m_id : 0;
                std::string name  = workspace->m_name;
                if (workspace->m_isSpecialWorkspace && name.starts_with("special:"))
                    name.erase(0, 8);
                if (name.empty())
                    name = local > 0 ? std::to_string(local) : "special";
                const auto owner = destinationSpace();
                if (local > 0 && model->local(owner, local))
                    local = model->lowestNumber(owner);
                const auto original = name;
                for (unsigned suffix = 2; model->named(owner, name, workspace->m_isSpecialWorkspace); ++suffix)
                    name = original + " (" + std::to_string(suffix) + ")";
                const auto id =
                    model->addWorkspace(owner, local, name, workspace->m_isSpecialWorkspace, workspace->m_id, workspace->m_name, monitorKey(workspace->m_monitor.lock()));
                if (id)
                    result = model->workspace(*id);
            }
            if (result) {
                if (result->nativeName != workspace->m_name) {
                    if (!startupRestore || workspace->m_id <= 0 || workspace->m_name != std::to_string(workspace->m_id))
                        assignAlias(*result, workspace->m_name);
                    if (!result->nativeName.starts_with("hyprspace:") && !result->nativeName.starts_with("special:hyprspace:"))
                        result->nativeName = workspace->m_name;
                }
                result->nativeId            = workspace->m_id;
                identities[workspace.get()] = {workspace, result->id};
                natives[result->id]         = workspace;
            }
            return result;
        }

        PHLWORKSPACE native(const std::string& id) {
            const auto found = natives.find(id);
            if (found != natives.end()) {
                const auto workspace = found->second.lock();
                if (valid(workspace))
                    return workspace;
            }
            const auto item = model->workspace(id);
            if (!item)
                return nullptr;
            for (const auto& reference : State::workspaceState()->workspaces()) {
                const auto workspace = reference.lock();
                if (valid(workspace) && workspace->m_name == item->nativeName && workspace->m_isSpecialWorkspace == item->special) {
                    record(workspace);
                    return workspace;
                }
            }
            return nullptr;
        }

        bool allocateNative(const std::string& id) {
            auto* item = model->workspace(id);
            if (!item)
                return false;
            if (const auto existing = native(id)) {
                item->nativeId = existing->m_id;
                return true;
            }
            if (item->nativeName.empty())
                item->nativeName = (item->special ? "special:" : "") + managedWorkspaceName(id);
            if (item->nativeId && !State::workspaceState()->query().id(item->nativeId).run() &&
                (item->special ? State::workspaceState()->isSpecial(item->nativeId) : !State::workspaceState()->isSpecial(item->nativeId)) &&
                (!model->byNative(item->nativeId) || model->byNative(item->nativeId)->id == id))
                return true;
            if (item->special) {
                auto candidate = State::workspaceState()->newSpecialID();
                while (State::workspaceState()->isSpecial(candidate) && model->byNative(candidate) && model->byNative(candidate)->id != id)
                    ++candidate;
                if (!State::workspaceState()->isSpecial(candidate) || candidate == WORKSPACE_INVALID)
                    return false;
                item->nativeId = candidate;
            } else {
                // Retain an adopted numeric identity when it is still free.
                if (item->nativeId > 0 && !State::workspaceState()->query().id(item->nativeId).run())
                    return true;
                auto candidate = State::workspaceState()->nextAvailableNamedWorkspace();
                while (model->byNative(candidate) && model->byNative(candidate)->id != id)
                    --candidate;
                item->nativeId = candidate;
            }
            return true;
        }

        PHLWORKSPACE materialize(const std::string& id, PHLMONITOR monitor, bool isEmpty = true) {
            if (const auto existing = native(id))
                return existing;
            if (!monitor)
                monitor = defaultMonitor();
            if (!monitor || !allocateNative(id))
                return nullptr;
            auto*      item       = model->workspace(id);
            const auto nativeId   = item->nativeId;
            const auto nativeName = item->nativeName;
            if (item->preferredMonitor.empty())
                item->preferredMonitor = monitorKey(monitor);
            CNativeScope guard;
            constructing[nativeId] = id;
            const Hyprutils::Utils::CScopeGuard finish([nativeId] { constructing.erase(nativeId); });
            const auto                          result = State::workspaceState()->create(nativeId, monitor->m_id, nativeName, isEmpty);
            if (valid(result)) {
                record(result);
                if ((nativeName.starts_with("hyprspace:") || nativeName.starts_with("special:hyprspace:")) && result->m_name != nativeName &&
                    !result->m_isSpecialWorkspace)
                    result->rename(nativeName);
            }
            return result;
        }

        std::optional<std::string> ensureRecord(const std::string& owner, int64_t local, const std::string& name, bool special, PHLMONITOR monitor) {
            if (const auto existing = local > 0 ? model->local(owner, local) : model->named(owner, name, special))
                return existing->id;
            auto id = model->addWorkspace(owner, local, name, special, 0, {}, monitorKey(monitor));
            if (id) {
                auto* item       = model->workspace(*id);
                item->nativeName = (special ? "special:" : "") + managedWorkspaceName(*id);
                if (owner == model->defaultSpace && local > 0 && !State::workspaceState()->query().id(local).run() && !model->byNative(local)) {
                    item->nativeId   = local;
                    item->nativeName = std::to_string(local);
                }
                if (!allocateNative(*id)) {
                    model->forgetWorkspace(*id);
                    return std::nullopt;
                }
                invalidate();
                persist();
            }
            return id;
        }

        void rememberSelection(PHLWORKSPACE workspace) {
            const auto item = record(workspace);
            if (!item || !workspace->m_monitor)
                return;
            const auto id = item->id, owner = item->space;
            const auto key = (item->special ? "special:" : "") + monitorKey(workspace->m_monitor.lock());
            if (auto* space = model->space(owner)) {
                if (!item->special) {
                    const auto old = space->selections.find(key);
                    if (old != space->selections.end() && old->second != id)
                        previous[{owner, key}] = old->second;
                }
                space->selections[key] = id;
            }
        }

        void rebuildMembership() {
            if (!membershipDirty)
                return;
            membershipDirty = false;
            ++membershipBuilds;
            activeWorkspaces.clear();
            activeWindows.clear();
            for (const auto& workspace : State::workspaceState()->workspaces())
                if (inActive(workspace.lock()))
                    activeWorkspaces.push_back(workspace);
            for (const auto& window : Desktop::windowState()->windows())
                if (window && window->m_isMapped && inActive(window->m_workspace))
                    activeWindows.emplace_back(window);
        }

        nlohmann::json arguments(const std::string& args) {
            if (!args.empty() && args.front() == '{')
                return nlohmann::json::parse(args);
            const auto     split  = args.find(' ');
            const auto     action = args.substr(0, split);
            const auto     rest   = split == std::string::npos ? "" : args.substr(split + 1);
            nlohmann::json result{{"action", action}};
            if (action == "create")
                result["name"] = rest;
            else if (action == "rename" || action == "delete" || action == "remove") {
                const auto next                                = rest.find(' ');
                result["id"]                                   = rest.substr(0, next);
                result[action == "rename" ? "name" : "target"] = next == std::string::npos ? "" : rest.substr(next + 1);
            } else
                result["id"] = rest;
            return result;
        }

        std::string spaceReference(const std::string& value) {
            if (model->space(value))
                return value;
            if (const auto named = model->spaceNamed(value))
                return named->id;
            return {};
        }

        void pin(PHLWINDOW window, bool value) {
            CNativeScope guard;
            (void)Config::Actions::pinWindow(value ? Config::Actions::TOGGLE_ACTION_ENABLE : Config::Actions::TOGGLE_ACTION_DISABLE, window);
        }

        void damage() {
            invalidate();
            for (const auto& monitor : monitors())
                g_pHyprRenderer->damageMonitor(monitor);
            if (g_overviewSession)
                session().damage();
        }
    } // namespace

    CNativeScope::CNativeScope() {
        ++nativeDepth;
    }
    CNativeScope::~CNativeScope() {
        --nativeDepth;
    }
    CSpaceScope::CSpaceScope(std::string id) : m_previous(std::move(context)) {
        context = std::move(id);
    }
    CSpaceScope::~CSpaceScope() {
        context = std::move(m_previous);
    }
    bool internal() {
        return nativeDepth != 0;
    }
    bool enabled() {
        return ready && config::spacesEnabled();
    }
    bool nativeDefault() {
        return model && model->spaces.size() == 1 && std::ranges::none_of(model->workspaces, [](const auto& item) {
                   return item.nativeName.starts_with("hyprspace:") || item.nativeName.starts_with("special:hyprspace:") ||
                          (item.number > 0 && item.nativeId != item.number) ||
                          (!item.nativeName.empty() && item.nativeName != (item.special ? "special:" + item.name : item.name));
               });
    }
    bool backgroundContext() {
        return enabled() && !context.empty() && model->space(context) && context != activeId();
    }
    const std::string& activeId() {
        static const std::string empty;
        return model ? model->active : empty;
    }
    uint64_t revision() {
        return generation;
    }
    void rendered(int64_t monitor, uint64_t frameGeneration, double timeMs) {
        if (lastSwitchGeneration && frameGeneration == lastSwitchGeneration)
            lastSwitchFrames.try_emplace(std::to_string(monitor), timeMs);
    }

    void observe(PHLWORKSPACE workspace) {
        if (ready && workspace) {
            const auto item = record(workspace);
            if (item && !workspace->m_isSpecialWorkspace && item->nativeName.starts_with("hyprspace:") && workspace->m_name != item->nativeName) {
                const auto   backing = item->nativeName;
                CNativeScope guard;
                workspace->rename(backing);
            }
            invalidate();
        }
    }

    std::string owner(PHLWORKSPACE workspace) {
        if (!ready)
            return {};
        const auto item = record(workspace);
        return item ? item->space : "";
    }

    bool inActive(PHLWORKSPACE workspace) {
        return !enabled() || (workspace && owner(workspace) == activeId());
    }

    int64_t localNumber(PHLWORKSPACE workspace) {
        const auto item = enabled() ? record(workspace) : nullptr;
        return item ? item->number : workspace ? workspace->m_id : 0;
    }

    std::string localName(PHLWORKSPACE workspace) {
        const auto item = enabled() ? record(workspace) : nullptr;
        return item ? item->name : workspace ? workspace->m_name : "";
    }

    std::string label(PHLWORKSPACE workspace) {
        if (!workspace)
            return {};
        const auto item = enabled() ? record(workspace) : nullptr;
        if (!item)
            return workspaceLabel(workspace->m_id, workspace->m_name);
        if (item->special)
            return "special:" + item->name;
        return workspaceLabel(item->number, item->name);
    }

    std::vector<SSpaceInfo> list() {
        std::vector<SSpaceInfo> result;
        if (model)
            for (const auto& space : model->spaces)
                result.push_back({space.id, space.name, space.id == activeId(), space.icon, space.color});
        return result;
    }

    const std::vector<PHLWORKSPACEREF>& workspaceRefs() {
        rebuildMembership();
        return activeWorkspaces;
    }

    const std::vector<PHLWINDOWREF>& windowRefs() {
        rebuildMembership();
        return activeWindows;
    }

    nlohmann::json snapshot() {
        nlohmann::json result{{"enabled", enabled()},
                              {"active", activeId()},
                              {"spaces", nlohmann::json::array()},
                              {"generation", generation},
                              {"switches", switches},
                              {"membership_rebuilds", membershipBuilds},
                              {"error", store && !store->error().empty() ? store->error() : lastError}};
        result["last_switch"] = {{"generation", lastSwitchGeneration}, {"start_ms", lastSwitchStart}, {"space", activeId()}, {"renders", lastSwitchFrames}};
        if (!model)
            return result;
        for (const auto& space : model->spaces) {
            nlohmann::json value{{"id", space.id},
                                 {"name", space.name},
                                 {"active", space.id == activeId()},
                                 {"icon", space.icon},
                                 {"color", space.color},
                                 {"workspaces", nlohmann::json::array()},
                                 {"selections", space.selections}};
            for (const auto& item : model->workspaces) {
                if (item.space != space.id)
                    continue;
                const auto workspace = native(item.id);
                value["workspaces"].push_back({{"id", item.id},
                                               {"number", item.number},
                                               {"name", item.name},
                                               {"special", item.special},
                                               {"native_id", workspace ? workspace->m_id : item.nativeId},
                                               {"native_name", item.nativeName},
                                               {"monitor", workspace && workspace->m_monitor ? workspace->m_monitor->m_name : ""},
                                               {"preferred_monitor", item.preferredMonitor},
                                               {"windows", workspace ? workspace->getWindowCount() : 0}});
            }
            result["spaces"].push_back(std::move(value));
        }
        return result;
    }

    nlohmann::json previewRemove(const std::string& id, const std::string& destination) {
        nlohmann::json result{{"workspaces", nlohmann::json::array()}, {"windows", 0}};
        if (!model)
            return result;
        auto        copy = *model;
        std::string error;
        if (!copy.erase(id, destination, &error)) {
            result["error"] = error;
            return result;
        }
        for (const auto& item : model->workspaces) {
            if (item.space != id)
                continue;
            const auto after     = copy.workspace(item.id);
            const auto workspace = native(item.id);
            const auto count     = workspace ? workspace->getWindowCount() : 0;
            result["windows"]    = result["windows"].get<int>() + count;
            result["workspaces"].push_back(
                {{"number", item.number}, {"new_number", after->number}, {"name", item.name}, {"new_name", after->name}, {"special", item.special}, {"windows", count}});
        }
        result["total"] = result["workspaces"].size();
        return result;
    }

    PHLWORKSPACE workspace(int64_t local, PHLMONITOR monitor, bool create) {
        if (!enabled())
            return State::workspaceState()->query().id(local).run();
        const auto owner = destinationSpace();
        const auto item  = model->local(owner, local);
        const auto id    = item ? std::optional{item->id} : create ? ensureRecord(owner, local, std::to_string(local), false, monitor) : std::nullopt;
        if (id && create && monitor && !internal() && !native(*id))
            model->workspace(*id)->preferredMonitor = monitorKey(monitor);
        return id ? (create ? materialize(*id, monitor) : native(*id)) : nullptr;
    }

    std::optional<std::string> logicalSelector(const std::string& input) {
        if (!enabled())
            return std::nullopt;
        const auto text    = input.starts_with("name:") ? input.substr(5) : input;
        const auto numeric = number(text);
        for (const auto& item : model->workspaces)
            if ((!item.nativeName.empty() && item.nativeName == text) || (numeric && item.nativeId == *numeric))
                return item.special ? "special:" + item.name : item.number > 0 ? std::to_string(item.number) : "name:" + item.name;
        return std::nullopt;
    }

    std::optional<SWorkspaceIDName> resolve(const std::string& input, PHLMONITOR monitor) {
        if (!enabled() || internal() || nativeDefault())
            return std::nullopt;
        const auto text = trimName(input);
        if (text.starts_with("name:hyprspace:") || text.starts_with("special:hyprspace:"))
            return std::nullopt;
        if (!monitor)
            monitor = defaultMonitor();
        if (!monitor || text.empty())
            return SWorkspaceIDName{WORKSPACE_INVALID};
        const auto space   = destinationSpace();
        const auto invalid = [] { return SWorkspaceIDName{WORKSPACE_INVALID}; };
        const auto encode  = [&](const std::string& identity) -> SWorkspaceIDName {
            if (!allocateNative(identity))
                return invalid();
            const auto item = model->workspace(identity);
            return item ? SWorkspaceIDName{item->nativeId, item->nativeName, !item->special && item->nativeId < -1} : invalid();
        };
        int64_t     local = 0;
        std::string name;
        bool        special = false;
        if (const auto numeric = number(text); numeric && text.front() != '+' && text.front() != '-') {
            if (*numeric > INT32_MAX)
                return invalid();
            local = std::max<int64_t>(1, *numeric);
            name  = std::to_string(local);
        } else if (text.starts_with("name:")) {
            name = text.substr(5);
            if (const auto existing = model->named(space, name))
                return encode(existing->id);
        } else if (text == "special" || text.starts_with("special:")) {
            special = true;
            name    = text.size() > 8 ? text.substr(8) : "special";
        } else if (text.starts_with("previous") || text == "prev") {
            const auto found = previous.find({space, monitorKey(monitor)});
            const auto item  = found == previous.end() ? nullptr : model->workspace(found->second);
            return item && item->space == space ? encode(item->id) : invalid();
        } else if (text == "next" || text.starts_with("empty") || text.front() == '+' || text.front() == '-' ||
                   (text.size() > 1 && (text.front() == 'm' || text.front() == 'e' || text.front() == 'r') &&
                    std::string_view("+-~").find(text[1]) != std::string_view::npos)) {
            const auto    current   = record(monitor->m_activeWorkspace);
            const auto    currentId = current && current->space == space ? current->id : std::string{};
            const int64_t base      = current && current->space == space ? current->number : 0;
            // Treat named workspaces as native negative IDs, then number-based
            // workspaces by their logical number rather than their backing ID.
            std::vector<std::string> choices, named;
            for (const auto& candidate : model->workspaces) {
                if (candidate.space != space || candidate.special)
                    continue;
                const auto ws = native(candidate.id);
                if (!ws || (text.front() != 'e' && ws->m_monitor != monitor))
                    continue;
                choices.push_back(candidate.id);
            }
            std::ranges::sort(choices, [&](const auto& left, const auto& right) {
                const auto a = model->workspace(left), b = model->workspace(right);
                const auto key = [](const auto* item) { return std::pair{item->number > 0, item->number > 0 ? item->number : item->nativeId}; };
                return key(a) < key(b);
            });
            for (const auto& id : choices)
                if (model->workspace(id)->number == 0)
                    named.push_back(id);

            if (text.front() == 'm' || text.front() == 'e') {
                const auto offset = number(text.substr(2));
                if (!offset || choices.empty() || *offset < 0)
                    return invalid();
                const int64_t count = choices.size();
                int64_t       index = 0;
                if (text[1] == '~')
                    index = *offset <= 1 ? 0 : std::min<int64_t>(*offset - 1, count - 1);
                else {
                    const auto found = std::ranges::find(choices, currentId);
                    index            = found == choices.end() ? -1 : found - choices.begin();
                    const auto delta = *offset % count;
                    index            = (index + (text[1] == '-' ? -delta : delta)) % count;
                    if (index < 0)
                        index += count;
                }
                return encode(choices[index]);
            }

            // Interval arithmetic keeps r~N and large relative steps bounded
            // even when rules reserve a large contiguous range of numbers.
            std::vector<std::pair<int64_t, int64_t>> excluded;
            const auto                               exclude = [&](int64_t first, int64_t last) {
                first = std::max<int64_t>(1, first);
                last  = std::min<int64_t>(INT32_MAX, last);
                if (first <= last)
                    excluded.emplace_back(first, last);
            };
            if (text.front() == 'r' || (text.starts_with("empty") && text.substr(5).contains('m'))) {
                for (const auto& candidate : model->workspaces) {
                    if (candidate.space != space || candidate.special || candidate.number <= 0)
                        continue;
                    if (const auto ws = native(candidate.id); ws && ws->m_monitor != monitor)
                        exclude(candidate.number, candidate.number);
                }
                for (const auto& rule : Config::workspaceRuleMgr()->getAllWorkspaceRules()) {
                    if (!rule || !rule->isEnabled() || rule->m_monitor.empty())
                        continue;
                    const auto assigned = State::monitorState()->query().relativeTo(monitor).configString(rule->m_monitor).run();
                    if (!assigned || assigned == monitor)
                        continue;
                    const auto selector = trimName(rule->m_workspaceString);
                    if (const auto numeric = number(selector); numeric && *numeric > 0)
                        exclude(*numeric, *numeric);
                    else if (selector.starts_with("name:")) {
                        const auto item = model->named(space, selector.substr(5));
                        if (item && item->number > 0)
                            exclude(item->number, item->number);
                    } else if (selector.starts_with("r[") && selector.ends_with(']')) {
                        const auto range = selector.substr(2, selector.size() - 3);
                        const auto split = range.find('-');
                        if (split != std::string::npos) {
                            const auto first = number(range.substr(0, split)), last = number(range.substr(split + 1));
                            if (first && last)
                                exclude(*first, *last);
                        }
                    }
                }
            }
            std::ranges::sort(excluded);
            std::vector<std::pair<int64_t, int64_t>> intervals;
            for (const auto& range : excluded) {
                if (!intervals.empty() && range.first <= intervals.back().second + 1)
                    intervals.back().second = std::max(intervals.back().second, range.second);
                else
                    intervals.push_back(range);
            }
            const auto ordinal = [&](int64_t value) {
                int64_t result = std::clamp<int64_t>(value, 0, INT32_MAX);
                for (const auto& [first, last] : intervals)
                    if (first <= value)
                        result -= std::min(last, value) - first + 1;
                return result;
            };
            const auto numericAt = [&](int64_t index) {
                if (index < 1 || index > INT32_MAX)
                    return int64_t{0};
                for (const auto& [first, last] : intervals)
                    if (first <= index) {
                        index += last - first + 1;
                        if (index > INT32_MAX)
                            return int64_t{0};
                    }
                return index;
            };
            if (text.starts_with("empty")) {
                const auto suffix = text.substr(5);
                local             = suffix.contains('n') ? base + 1 : 1;
                while (local <= INT32_MAX) {
                    if (suffix.contains('m')) {
                        for (const auto& [first, last] : intervals)
                            if (local >= first && local <= last)
                                local = last + 1;
                    }
                    if (local > INT32_MAX)
                        return invalid();
                    const auto existing = model->local(space, local);
                    const auto ws       = existing ? native(existing->id) : nullptr;
                    if (!ws || ws->getWindowCount() == 0)
                        break;
                    ++local;
                }
            } else if (text == "next")
                local = std::max<int64_t>(1, base + 1);
            else if (text.front() == 'r') {
                const auto offset = number(text.substr(2));
                if (!offset || *offset < 0 || *offset > INT32_MAX)
                    return invalid();
                int64_t index = 0;
                if (text[1] == '~')
                    index = *offset;
                else {
                    const auto found = std::ranges::find(named, currentId);
                    index            = found == named.end() ? static_cast<int64_t>(named.size()) + ordinal(base) : found - named.begin() + 1;
                    index += text[1] == '-' ? -*offset : *offset;
                    index = std::max<int64_t>(1, index);
                }
                if (index < 1)
                    return invalid();
                if (index <= static_cast<int64_t>(named.size()))
                    return encode(named[index - 1]);
                local = numericAt(index - named.size());
            } else {
                const auto delta = text.front() == '+' ? number(text.substr(1)) : number(text);
                if (!delta || *delta > INT32_MAX - base)
                    return invalid();
                local = *delta <= -base ? 1 : base + *delta;
            }
            if (local <= 0 || local > INT32_MAX)
                return invalid();
            name = std::to_string(local);
        } else {
            // Native dispatchers also accept a previously created name without
            // the name: prefix; an unknown bare name must not create a target.
            const auto existing = model->named(space, text);
            return existing ? encode(existing->id) : invalid();
        }
        if (name.empty() || name.starts_with("hyprspace:"))
            return invalid();
        const auto id = ensureRecord(space, local, name, special, monitor);
        return id ? encode(*id) : invalid();
    }

    bool matchesIdentity(PHLWORKSPACE workspace, const std::string& input, bool& handled) {
        handled = false;
        if (!enabled())
            return false;
        const auto item = record(workspace, false);
        if (!item)
            return false;
        const auto text = trimName(input);
        if (const auto value = number(text)) {
            handled = true;
            return item->number > 0 && item->number == *value;
        }
        if (text.starts_with("name:")) {
            handled         = true;
            const auto name = text.substr(5);
            return item->name == name || workspace->m_name == name;
        }
        if (text.starts_with("special")) {
            handled = true;
            return item->special && (text == "special" || text.substr(8) == item->name || text == workspace->m_name);
        }
        if (text.starts_with("r[") && text.ends_with(']')) {
            handled          = true;
            const auto range = text.substr(2, text.size() - 3);
            const auto split = range.find('-');
            const auto first = number(range.substr(0, split));
            const auto last  = split == std::string::npos ? first : number(range.substr(split + 1));
            return first && last && item->number > 0 && item->number >= *first && item->number <= *last;
        }
        if (text.starts_with("n[") && text.ends_with(']')) {
            handled          = true;
            const auto value = text.substr(2, text.size() - 3);
            if (value.starts_with("s:"))
                return item->name.starts_with(value.substr(2));
            if (value.starts_with("e:"))
                return item->name.ends_with(value.substr(2));
            const bool named = item->number == 0 && !item->special;
            return value == "true" || value == "1" ? named : !named;
        }
        return false;
    }

    void setTransitionCallbacks(std::function<void()> before, std::function<void()> after) {
        beforeTransition = std::move(before);
        afterTransition  = std::move(after);
    }

    void rememberPin(PHLWINDOW window, bool value) {
        if (!ready || !window || internal())
            return;
        savePin(window, value);
        persist();
    }

    bool desiredPin(PHLWINDOW window) {
        const auto found = window ? pins.find(window.get()) : pins.end();
        return found != pins.end() && found->second.window.lock() == window ? found->second.desired : window && window->m_pinned;
    }

    SDispatchResult switchTo(const std::string& reference) {
        const auto started = std::chrono::steady_clock::now();
        if (!enabled())
            return fail("spaces are disabled");
        const auto destination = spaceReference(reference);
        if (destination.empty())
            return fail("space does not exist");
        if (!overlaysAllowed())
            return fail("spaces cannot be changed while the session is locked");
        if (switching) {
            pendingSwitch = destination;
            return {.success = true};
        }
        const auto connected = monitors();
        if (connected.empty())
            return fail("no connected monitor");
        if (destination == activeId())
            return {.success = true};
        const auto                                       outgoing = activeId();
        std::vector<std::pair<PHLMONITOR, PHLWORKSPACE>> saved, planned;
        std::vector<std::pair<PHLMONITOR, PHLWORKSPACE>> savedSpecials;
        std::vector<std::pair<PHLWORKSPACE, PHLMONITOR>> placements;
        const auto                                       oldFocus = Desktop::focusState()->window();
        const auto                                       oldPins  = pins;
        for (const auto& reference : State::workspaceState()->workspaces())
            if (const auto workspace = reference.lock(); workspace && workspace->m_monitor)
                placements.emplace_back(workspace, workspace->m_monitor.lock());
        std::set<std::string> used;
        for (const auto& monitor : connected) {
            saved.emplace_back(monitor, monitor->m_activeWorkspace);
            savedSpecials.emplace_back(monitor, monitor->m_activeSpecialWorkspace);
            rememberSelection(monitor->m_activeWorkspace);
            if (monitor->m_activeSpecialWorkspace)
                rememberSelection(monitor->m_activeSpecialWorkspace);
            std::string id;
            if (const auto selected = model->selection(destination, monitorKey(monitor)); selected && !selected->special && !used.contains(selected->id))
                id = selected->id;
            if (id.empty())
                for (const auto& candidate : model->workspaces)
                    if (candidate.space == destination && !candidate.special && candidate.preferredMonitor == monitorKey(monitor) && !used.contains(candidate.id)) {
                        id = candidate.id;
                        break;
                    }
            if (id.empty()) {
                const auto local   = model->lowestNumber(destination);
                const auto created = ensureRecord(destination, local, std::to_string(local), false, monitor);
                if (!created) {
                    persist();
                    return fail("cannot allocate destination workspace");
                }
                id = *created;
            }
            const auto target = materialize(id, monitor);
            if (!target) {
                persist();
                return fail("cannot create destination workspace");
            }
            used.insert(id);
            planned.emplace_back(monitor, target);
        }
        const auto beforeMutation = *model;
        switching                 = true;
        focused[outgoing]         = Desktop::focusState()->window();
        if (beforeTransition)
            beforeTransition();
        try {
            CNativeScope guard;
            for (const auto& window : Desktop::windowState()->windows())
                if (window && window->m_isMapped && inActive(window->m_workspace) && window->m_pinned) {
                    savePin(window, true);
                    pin(window, false);
                }
            for (const auto& monitor : connected)
                monitor->setSpecialWorkspace(nullptr);
            model->active = destination;
            invalidate();
            // Restore preferred placement for all destination workspaces, not
            // merely the currently visible workspace on each output.
            std::vector<std::string> destinationIds;
            for (const auto& item : model->workspaces)
                if (item.space == destination)
                    destinationIds.push_back(item.id);
            for (const auto& id : destinationIds) {
                const auto item = model->workspace(id);
                const auto ws   = native(id);
                if (!ws)
                    continue;
                auto       desired = std::ranges::find_if(connected, [&](const auto& monitor) { return monitorKey(monitor) == item->preferredMonitor; });
                const auto monitor = desired == connected.end() ? defaultMonitor() : *desired;
                if (monitor && ws->m_monitor != monitor)
                    State::workspacePlacementController()->moveWorkspaceToMonitor(ws, monitor, true);
            }
            for (const auto& [monitor, ws] : planned) {
                if (ws->m_monitor != monitor)
                    State::workspacePlacementController()->moveWorkspaceToMonitor(ws, monitor, true);
                monitor->changeWorkspace(ws, false, true, true);
                if (monitor->m_activeWorkspace != ws)
                    throw std::runtime_error("destination monitor did not activate its workspace");
                model->select(destination, monitorKey(monitor), record(ws)->id);
            }
            std::erase_if(pins, [](const auto& entry) { return !entry.second.window; });
            for (const auto& [_, state] : pins)
                if (const auto window = state.window.lock(); window && state.desired && inActive(window->m_workspace))
                    pin(window, true);
            for (const auto& monitor : connected) {
                const auto* space    = model->space(destination);
                const auto  selected = space->selections.find("special:" + monitorKey(monitor));
                if (selected != space->selections.end())
                    if (const auto ws = native(selected->second); ws && ws->m_isSpecialWorkspace && ws->m_monitor == monitor)
                        monitor->setSpecialWorkspace(ws);
            }
            auto window = focused[destination].lock();
            if (!window || !window->m_isMapped || !inActive(window->m_workspace) || !window->m_workspace->isVisible())
                window = defaultMonitor()->m_activeWorkspace->getFocusCandidate();
            if (window)
                (void)Config::Actions::focus(window);
            else
                Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_KEYBIND);
            Desktop::globalWindowController()->updateSuspendedStates();
            ++switches;
            lastError.clear();
        } catch (const std::exception& error) {
            CNativeScope guard;
            *model = beforeMutation;
            try {
                for (const auto& window : Desktop::windowState()->windows())
                    if (window && window->m_pinned)
                        pin(window, false);
                for (const auto& [ws, monitor] : placements)
                    if (valid(ws) && monitor->m_enabled && ws->m_monitor != monitor)
                        State::workspacePlacementController()->moveWorkspaceToMonitor(ws, monitor, true);
                for (const auto& [monitor, ws] : saved)
                    if (monitor->m_enabled && valid(ws))
                        monitor->changeWorkspace(ws, false, true, true);
                pins = oldPins;
                for (const auto& [_, state] : pins)
                    if (const auto window = state.window.lock(); window && state.desired && inActive(window->m_workspace))
                        pin(window, true);
                for (const auto& [monitor, ws] : savedSpecials)
                    if (monitor->m_enabled)
                        monitor->setSpecialWorkspace(ws);
                if (oldFocus && oldFocus->m_isMapped)
                    (void)Config::Actions::focus(oldFocus);
                Desktop::globalWindowController()->updateSuspendedStates();
            } catch (...) {
                topologyPending = true;
            }
            switching = false;
            damage();
            persist();
            if (afterTransition)
                afterTransition();
            return fail(error.what());
        }
        switching = false;
        damage();
        lastSwitchGeneration = generation;
        lastSwitchStart      = std::chrono::duration<double, std::milli>(started.time_since_epoch()).count();
        lastSwitchFrames.clear();
        persist();
        if (afterTransition)
            afterTransition();
        if (topologyPending) {
            topologyPending = false;
            topologyChanged();
        }
        if (!pendingSwitch.empty()) {
            auto next = std::exchange(pendingSwitch, {});
            return switchTo(next);
        }
        return {.success = true};
    }

    SDispatchResult renameWorkspace(PHLWORKSPACE ws, const std::string& name) {
        auto* item = record(ws);
        if (!item || !validName(trimName(name)))
            return fail("invalid workspace name");
        if (const auto other = model->named(item->space, trimName(name), item->special); other && other->id != item->id)
            return fail("workspace name already exists in this space");
        item->name = trimName(name);
        damage();
        persist();
        return {.success = true};
    }

    SDispatchResult renumberWorkspace(PHLWORKSPACE ws, int64_t local) {
        auto* item = record(ws);
        if (!item || item->special || local <= 0 || local > INT32_MAX)
            return fail("invalid workspace number");
        if (const auto other = model->local(item->space, local); other && other->id != item->id)
            return fail("workspace number already exists in this space");
        if (item->name == std::to_string(item->number))
            item->name = std::to_string(local);
        item->number = local;
        damage();
        persist();
        return {.success = true};
    }

    SDispatchResult dispatch(const std::string& args) {
        if (!enabled())
            return fail("spaces are disabled");
        if (!overlaysAllowed())
            return fail("spaces cannot be changed while the session is locked");
        try {
            const auto request = arguments(args);
            const auto action  = request.value("action", "");
            if (action == "switch")
                return switchTo(request.value("id", ""));
            if (action == "next" || action == "prev") {
                const auto current = std::ranges::find(model->spaces, activeId(), &SSpace::id) - model->spaces.begin();
                const auto count   = static_cast<int64_t>(model->spaces.size());
                return switchTo(model->spaces[(current + (action == "prev" ? count - 1 : 1)) % count].id);
            }
            std::string error;
            if (action == "create") {
                const auto id = model->create(request.value("name", ""), &error, request.value("icon", std::string(DEFAULT_SPACE_ICON)),
                                              request.value("color", std::string(DEFAULT_SPACE_COLOR)));
                if (!id)
                    return fail(error);
                const auto result = switchTo(*id);
                persist();
                return result;
            }
            const auto id = spaceReference(request.value("id", ""));
            if (action == "reorder") {
                const auto target   = spaceReference(request.value("target", ""));
                const auto position = request.value("position", "");
                if (position != "before" && position != "after")
                    return fail("reorder position must be before or after");
                if (!model->reorder(id, target, position == "after", &error))
                    return fail(error);
            } else if (action == "rename" || action == "update") {
                const auto field = [&](const char* name) -> std::optional<std::string> {
                    if (!request.contains(name))
                        return std::nullopt;
                    return request.at(name).get<std::string>();
                };
                auto name = field("name");
                if (action == "rename" && !name)
                    name = "";
                const auto icon = field("icon"), color = field("color");
                if (!model->update(id, std::move(name), icon, color, &error))
                    return fail(error);
            } else if (action == "delete" || action == "remove") {
                const auto destination = spaceReference(request.value("target", ""));
                auto       transfer    = *model;
                if (!transfer.erase(id, destination, &error))
                    return fail(error);
                if (id == activeId()) {
                    const auto result = switchTo(destination);
                    if (!result.success)
                        return result;
                    // Switching materializes selections, so recompute the
                    // transfer against that now-current state.
                    transfer = *model;
                    if (!transfer.erase(id, destination, &error))
                        return fail(error);
                }
                *model = std::move(transfer);
                focused.erase(id);
            } else
                return fail("space expects create, switch, rename, update, reorder, delete, next or prev");
            damage();
            persist();
            return {.success = true};
        } catch (const std::exception& error) {
            return fail(std::string("invalid space request: ") + error.what());
        }
    }

    SDispatchResult move(const std::string& args) {
        if (!enabled() || !overlaysAllowed())
            return fail("spaces are unavailable");
        try {
            const auto request     = nlohmann::json::parse(args);
            const auto destination = spaceReference(request.value("space_id", ""));
            if (destination.empty())
                return fail("destination space does not exist");
            const auto kind   = request.value("target", "workspace");
            auto       window = Desktop::focusState()->window();
            auto       ws     = defaultMonitor() ? defaultMonitor()->m_activeWorkspace : nullptr;
            if (g_overviewSession && session().live())
                if (const auto selection = session().selection.command()) {
                    ws     = session().workspace(*selection);
                    window = selection->window.lock();
                }
            if (request.contains("workspace"))
                ws = State::workspaceState()->query().id(request.at("workspace").get<int64_t>()).run();
            if (request.contains("window")) {
                window.reset();
                const auto address = request.at("window").get<std::string>();
                for (const auto& candidate : Desktop::windowState()->windows())
                    if (std::format("0x{:x}", reinterpret_cast<uintptr_t>(candidate.get())) == address)
                        window = candidate;
            }
            if (kind == "workspace") {
                const auto item = record(ws);
                if (!item)
                    return fail("select a workspace to move");
                const auto id = item->id;
                if (item->space == destination)
                    return {.success = true};
                // Vacate an outgoing visible workspace before changing its
                // owner, preserving the current desktop's isolation.
                if (inActive(ws) && ws->m_monitor && (ws->m_monitor->m_activeWorkspace == ws || ws->m_monitor->m_activeSpecialWorkspace == ws)) {
                    CNativeScope guard;
                    if (ws->m_isSpecialWorkspace)
                        ws->m_monitor->setSpecialWorkspace(nullptr);
                    else {
                        const auto local         = model->lowestNumber(activeId());
                        const auto replacementId = ensureRecord(activeId(), local, std::to_string(local), false, ws->m_monitor.lock());
                        const auto replacement   = replacementId ? materialize(*replacementId, ws->m_monitor.lock()) : nullptr;
                        if (!replacement)
                            return fail("cannot prepare a replacement workspace");
                        for (const auto& candidate : Desktop::windowState()->windows())
                            if (candidate->m_workspace == ws && candidate->m_pinned) {
                                savePin(candidate, true);
                                pin(candidate, false);
                            }
                        ws->m_monitor->changeWorkspace(replacement, false, true, true);
                    }
                }
                std::string error;
                if (!model->moveWorkspace(id, destination, &error))
                    return fail(error);
            } else if (kind == "window") {
                if (!window || !window->m_isMapped)
                    return fail("select a window to move");
                if (owner(window->m_workspace) == destination)
                    return {.success = true};
                const auto monitor = window->m_monitor.lock();
                const auto local   = model->lowestNumber(destination);
                const auto id      = ensureRecord(destination, local, std::to_string(local), false, monitor);
                const auto target  = id ? materialize(*id, monitor) : nullptr;
                if (!target)
                    return fail("cannot prepare destination workspace");
                CNativeScope guard;
                if (window->m_pinned) {
                    savePin(window, true);
                    pin(window, false);
                }
                const auto moved = Config::Actions::moveToWorkspace(target, true, window);
                if (!moved)
                    return fail(moved.error().message);
            } else
                return fail("move target must be window or workspace");
            for (const auto& [_, state] : pins)
                if (const auto window = state.window.lock(); window && state.desired && inActive(window->m_workspace) && !window->m_pinned)
                    pin(window, true);
            Desktop::globalWindowController()->updateSuspendedStates();
            damage();
            persist();
            return {.success = true};
        } catch (const std::exception& error) {
            return fail(std::string("invalid space move: ") + error.what());
        }
    }

    void changed(PHLWORKSPACE workspace) {
        if (!ready || !workspace)
            return;
        const auto item = record(workspace);
        if (item && nativeDefault()) {
            item->nativeId   = workspace->m_id;
            item->nativeName = workspace->m_name;
            item->number     = workspace->m_id > 0 ? workspace->m_id : 0;
            item->name       = workspace->m_isSpecialWorkspace && workspace->m_name.starts_with("special:") ? workspace->m_name.substr(8) : workspace->m_name;
        }
        if (item && !internal() && !topologyChanging)
            item->preferredMonitor = monitorKey(workspace->m_monitor.lock());
        invalidate();
        persist();
    }

    void reconcile() {
        if (!ready)
            return;
        for (const auto& workspace : State::workspaceState()->workspaces())
            observe(workspace.lock());
        for (const auto& monitor : monitors())
            if (!startupRestore && !topologyChanging && monitor->m_activeWorkspace && inActive(monitor->m_activeWorkspace))
                rememberSelection(monitor->m_activeWorkspace);
        invalidate();
        persist();
    }

    void topologyChanged() {
        if (!enabled())
            return;
        if (switching) {
            topologyPending = true;
            return;
        }
        const auto connected = monitors();
        if (connected.empty())
            return;
        CNativeScope guard;
        const auto   savedSelections   = model->space(activeId())->selections;
        const bool   restoreSelections = startupRestore || topologyChanging;
        for (const auto& window : Desktop::windowState()->windows())
            if (window && window->m_isMapped && window->m_pinned && !inActive(window->m_workspace)) {
                savePin(window, true);
                pin(window, false);
            }
        for (const auto& monitor : connected)
            if (monitor->m_activeSpecialWorkspace && !inActive(monitor->m_activeSpecialWorkspace))
                monitor->setSpecialWorkspace(nullptr);
        std::vector<std::string> ids;
        for (const auto& item : model->workspaces)
            ids.push_back(item.id);
        for (const auto& id : ids) {
            const auto item = model->workspace(id);
            const auto ws   = native(id);
            if (!ws)
                continue;
            const auto preferred = std::ranges::find_if(connected, [&](const auto& monitor) { return monitorKey(monitor) == item->preferredMonitor; });
            const auto target    = preferred == connected.end() ? defaultMonitor() : *preferred;
            if (target && ws->m_monitor != target)
                State::workspacePlacementController()->moveWorkspaceToMonitor(ws, target, true);
        }
        std::set<std::string> used;
        for (const auto& monitor : connected) {
            std::string id;
            const auto  saved = savedSelections.find(monitorKey(monitor));
            if ((restoreSelections || !monitor->m_activeWorkspace || !inActive(monitor->m_activeWorkspace)) && saved != savedSelections.end())
                if (const auto selected = model->workspace(saved->second);
                    selected && selected->space == activeId() && !selected->special && !used.contains(selected->id))
                    id = selected->id;
            if (id.empty() && monitor->m_activeWorkspace && inActive(monitor->m_activeWorkspace))
                if (const auto item = record(monitor->m_activeWorkspace); item && !used.contains(item->id))
                    id = item->id;
            if (id.empty())
                for (const auto& item : model->workspaces)
                    if (item.space == activeId() && !item.special && item.preferredMonitor == monitorKey(monitor) && !used.contains(item.id)) {
                        id = item.id;
                        break;
                    }
            if (id.empty()) {
                const auto local   = model->lowestNumber(activeId());
                const auto created = ensureRecord(activeId(), local, std::to_string(local), false, monitor);
                if (created)
                    id = *created;
            }
            const auto ws = id.empty() ? nullptr : materialize(id, monitor);
            if (!ws) {
                lastError = "could not restore a workspace on " + monitor->m_name;
                continue;
            }
            if (ws->m_monitor != monitor)
                State::workspacePlacementController()->moveWorkspaceToMonitor(ws, monitor, true);
            if (monitor->m_activeWorkspace != ws)
                monitor->changeWorkspace(ws, false, true, true);
            used.insert(id);
            rememberSelection(ws);
        }
        for (const auto& [_, state] : pins)
            if (const auto window = state.window.lock(); window && state.desired && inActive(window->m_workspace) && !window->m_pinned)
                pin(window, true);
        for (const auto& monitor : connected) {
            const auto saved = savedSelections.find("special:" + monitorKey(monitor));
            if (saved != savedSelections.end())
                if (const auto ws = native(saved->second); ws && ws->m_isSpecialWorkspace && ws->m_monitor == monitor && inActive(ws))
                    monitor->setSpecialWorkspace(ws);
        }
        const auto focus = Desktop::focusState()->window();
        if (!focus || !focus->m_workspace || !inActive(focus->m_workspace) || !focus->m_workspace->isVisible()) {
            const auto monitor   = defaultMonitor();
            const auto ws        = monitor ? monitor->getCurrentWorkspace() : nullptr;
            const auto candidate = ws ? ws->getFocusCandidate() : nullptr;
            if (candidate)
                (void)Config::Actions::focus(candidate);
            else
                Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_KEYBIND);
        }
        damage();
        topologyChanging = false;
        startupRestore   = false;
        persist();
    }

    void initialize() {
        if (ready)
            return;
        const auto file   = spaceStatePath(std::getenv("XDG_STATE_HOME"), std::getenv("HOME"));
        auto       loaded = load(file);
        startupRestore    = loaded.model.has_value();
        model             = loaded.model ? std::make_unique<CSpaceModel>(std::move(*loaded.model)) : std::make_unique<CSpaceModel>();
        lastError         = loaded.error;
        store             = std::make_unique<CSpaceStore>(file);
        ready             = true;
        // Runtime numeric IDs can be reused by a new compositor process. Match
        // live native names first, then regenerate IDs when materialized.
        for (auto& item : model->workspaces)
            if (item.nativeId < 0)
                item.nativeId = 0;
        {
            CSpaceScope initial(model->defaultSpace);
            for (const auto& workspace : State::workspaceState()->workspaces())
                observe(workspace.lock());
        }
        auto& events = Event::bus()->m_events;
        listeners.push_back(events.workspace.created.listen([](PHLWORKSPACEREF ws) {
            observe(ws.lock());
            persist();
        }));
        listeners.push_back(events.monitor.preRemoved.listen([](PHLMONITOR) { topologyChanging = true; }));
        listeners.push_back(events.monitor.preAdded.listen([](PHLMONITOR) { topologyChanging = true; }));
        listeners.push_back(events.workspace.removed.listen([](PHLWORKSPACEREF) { invalidate(); }));
        listeners.push_back(events.workspace.active.listen([](PHLWORKSPACE ws) {
            if (ready) {
                if (!internal() && !topologyChanging && !startupRestore)
                    rememberSelection(ws);
                invalidate();
                if (!internal())
                    persist();
            }
        }));
        listeners.push_back(events.workspace.moveToMonitor.listen([](PHLWORKSPACE ws, PHLMONITOR) { changed(ws); }));
        listeners.push_back(events.workspace.specialActive.listen([](PHLWORKSPACE ws, PHLMONITOR monitor) {
            if (!ready || internal() || topologyChanging || startupRestore)
                return;
            if (ws)
                rememberSelection(ws);
            else if (auto* space = model->space(activeId()))
                space->selections.erase("special:" + monitorKey(monitor));
            persist();
        }));
        listeners.push_back(events.window.open.listen([](PHLWINDOW) { invalidate(); }));
        listeners.push_back(events.window.openLate.listen([](PHLWINDOW) { invalidate(); }));
        listeners.push_back(events.window.close.listen([](PHLWINDOW window) {
            model->runtimePins.erase(pinKey(window));
            pins.erase(window.get());
            invalidate();
            persist();
        }));
        listeners.push_back(events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE ws) {
            observe(ws);
            invalidate();
        }));
        listeners.push_back(events.window.pin.listen([](PHLWINDOW window) {
            if (window)
                rememberPin(window, window->m_pinned);
            invalidate();
        }));
        std::set<std::string> livePins;
        for (const auto& window : Desktop::windowState()->windows())
            if (window && window->m_isMapped) {
                const auto key = pinKey(window);
                if (window->m_pinned || model->runtimePins.contains(key)) {
                    savePin(window, true);
                    livePins.insert(key);
                }
            }
        std::erase_if(model->runtimePins, [&](const auto& entry) { return !livePins.contains(entry.first); });
        if (!startupRestore)
            reconcile();
        else {
            invalidate();
            persist();
        }
    }

    void shutdown() {
        if (!ready)
            return;
        persist();
        listeners.clear();
        changeNotification.reset();
        changeNotificationQueued = false;
        ready                    = false;
        beforeTransition         = {};
        afterTransition          = {};
        store.reset();
        model.reset();
        identities.clear();
        natives.clear();
        constructing.clear();
        pins.clear();
        focused.clear();
        previous.clear();
        activeWorkspaces.clear();
        activeWindows.clear();
        switching = topologyPending = false;
        startupRestore = topologyChanging = false;
        membershipDirty                   = true;
        pendingSwitch.clear();
        context.clear();
    }
} // namespace hyprspace::spaces
