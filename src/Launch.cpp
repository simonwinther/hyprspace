#include "Launch.hpp"
#include "Spaces.hpp"
#include "SpaceSelector.hpp"
#include "InputController.hpp"
#include "Config.hpp"
#include "Diagnostics.hpp"
#include "LaunchGeometry.hpp"
#include "OverlayPolicy.hpp"
#include "Texture.hpp"
#include "PreviewFilter.hpp"

#include "CompositorHooks.hpp"
#include "Overview.hpp"
#include "WindowBoard.hpp"
#include "HelpOverlay.hpp"
#include "OverviewSession.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/TokenManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/protocols/XDGActivation.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>

#include <filesystem>
#include <unordered_map>

namespace hyprspace::launch {
    namespace {
        // Explicit instantiation gives narrowly scoped access to the callbacks
        // of this exact ABI. The native validator is retained and restored.
        template <typename Tag, auto Member> struct CAccess {
            friend auto member(Tag) {
                return Member;
            }
        };
        struct SManagers {
            friend auto member(SManagers);
        };
        struct SRequests {
            friend auto member(SRequests);
        };
        struct STokens {
            friend auto member(STokens);
        };
        struct SSuspended {
            friend auto member(SSuspended);
        };
        template struct CAccess<SManagers, &CXDGActivationProtocol::m_managers>;
        template struct CAccess<SRequests, &CXdgActivationV1::requests>;
        template struct CAccess<STokens, &CXDGActivationProtocol::m_sentTokens>;
        template struct CAccess<SSuspended, &Desktop::View::CWindow::m_suspended>;

        using Clock      = std::chrono::steady_clock;
        using Env        = std::vector<std::pair<std::string, std::string>>;
        using Activation = std::function<void(CXdgActivationV1*, const char*, wl_resource*)>;
        struct SContext {
            std::string     space;
            SOverviewTarget target;
            PHLWORKSPACEREF workspace;
            // Only prepared overview destinations need ownership beyond the
            // session. Ordinary captures retain their weak identity semantics.
            PHLWORKSPACE              preparedLifetime;
            bool                      hasWorkspace = false;
            std::vector<PHLWINDOWREF> existing;
        };
        struct SLaunch {
            SContext          context;
            Clock::time_point expires;
            PHLWINDOWREF      placed;
            bool              completed = false;
        };
        struct SPending {
            WP<CWLSurfaceResource> surface;
            std::string            token;
        };

        CLaunchContexts<SContext>                         captures;
        std::unordered_map<std::string, SLaunch>          launches;
        std::unordered_map<CXdgActivationV1*, Activation> callbacks;
        std::vector<SPending>                             pending;
        CFunctionHook *                                   bindHook = nullptr, *destroyHook = nullptr, *mapHook = nullptr, *envHook = nullptr;
        int                                               socketFd     = -1;
        bool                                              socketBound  = false;
        wl_event_source*                                  socketSource = nullptr;
        std::string                                       socketPath;
        struct SClient {
            int               fd;
            wl_event_source*  source;
            Clock::time_point expires;
        };
        std::vector<std::unique_ptr<SClient>> clients;
        SP<CEventLoopTimer>                   timer;
        std::optional<SOverviewTarget>        commandTarget;

        void prune() {
            const auto now = Clock::now();
            captures.prune(now);
            auto& tokens = PROTO::activation.get()->*member(STokens{});
            std::erase_if(launches, [&](const auto& launch) {
                if (launch.second.expires > now)
                    return false;
                std::erase_if(tokens, [&](const auto& token) { return token.token == launch.first; });
                g_pTokenManager->removeToken(g_pTokenManager->getToken(launch.first));
                return true;
            });
            std::erase_if(pending, [](const auto& entry) { return !entry.surface || !launches.contains(entry.token); });
        }

        std::string capture(const std::optional<SOverviewTarget>& target, bool desktop = false) {
            if ((!session().live() && !desktop) || !overlaysAllowed() || !target)
                return {};
            SContext context{.space = spaces::activeId(), .target = *target};
            if (const auto ws = State::workspaceState()->query().id(target->workspace.id).run()) {
                if (!valid(ws) || ws->m_name != target->workspace.name)
                    return {};
                context.workspace        = ws;
                context.hasWorkspace     = true;
                context.preparedLifetime = desktop ? ws : session().preparedLifetime(ws);
                context.space            = spaces::owner(ws);
            }
            for (const auto& w : Desktop::windowState()->windows())
                if (w->m_isMapped)
                    context.existing.emplace_back(w);
            const auto token = g_pTokenManager->getRandomUUID();
            return captures.capture(token, std::move(context), Clock::now()) ? token : "";
        }

        PHLWORKSPACE destination(SContext& context) {
            auto ws = context.hasWorkspace ? context.workspace.lock() : session().workspace(context.target);
            // An existing workspace is an object identity, not a reusable ID.
            // A captured empty placeholder may materialize once at consumption.
            if (!valid(ws) || State::workspaceState()->query().id(ws->m_id).run() != ws || !ws->m_monitor || !ws->m_monitor->m_enabled)
                return nullptr;
            context.workspace        = ws;
            context.hasWorkspace     = true;
            context.target.workspace = {ws->m_id, ws->m_name};
            return ws;
        }

        std::string consume(const std::string& token) {
            prune();
            if (launches.size() >= CLaunchContexts<SContext>::LIMIT || !overlaysAllowed())
                return {};
            auto context = captures.consume(token, Clock::now());
            if (!context || !destination(*context))
                return {};
            const auto activation = g_pTokenManager->registerNewToken({}, std::chrono::minutes(2));
            // This compositor-authorized launch token uses the same one-shot
            // validation path as a token requested by a Wayland launcher.
            (PROTO::activation.get()->*member(STokens{})).push_back({activation, nullptr});
            launches.emplace(activation, SLaunch{std::move(*context), Clock::now() + std::chrono::minutes(2), {}});
            return activation;
        }

        void place(const std::string& token, PHLWINDOW w) {
            const auto it = launches.find(token);
            if (it == launches.end() || !w || !w->m_isMapped || !w->m_workspace || it->second.completed || it->second.expires <= Clock::now())
                return;
            auto&               context         = it->second.context;
            const auto          originWorkspace = context.workspace.lock();
            spaces::CSpaceScope originScope(originWorkspace ? spaces::owner(originWorkspace) : context.space);
            it->second.completed = true;
            const Hyprutils::Utils::CScopeGuard releasePrepared([&context] { context.preparedLifetime.reset(); });
            if (std::ranges::any_of(context.existing, [&](const auto& old) { return old == w; }))
                return; // existing-window activation always retains native behavior
            it->second.placed = w;
            const auto& rules = w->m_ruleApplicator->static_;
            if (!rules.workspace.empty() || !rules.monitor.empty())
                return;
            const auto ws = destination(context);
            if (!ws)
                return;
            const auto target = w->layoutTarget();
            const auto point  = session().desktopPoint(context.target);
            hooks::atDesktopPoint(point, [&] {
                if (target->space() != ws->m_space)
                    target->assignToSpace(ws->m_space, point);
                else if (!target->floating()) {
                    // Reinsert only in the algorithm. CSpace::move would append
                    // a duplicate target to the workspace's membership list.
                    ws->m_space->algorithm()->removeTarget(target);
                    ws->m_space->algorithm()->moveTarget(target, point);
                }
                if (target->floating() && !rules.position && !rules.center.value_or(false)) {
                    const auto size    = target->position().size();
                    const auto work    = ws->m_monitor->logicalBoxMinusReserved();
                    const auto extents = w->getFullWindowReservedArea();
                    const auto border  = static_cast<double>(w->getRealBorderSize());
                    if (const auto box = boundedLaunchPlacement({point.x - size.x / 2, point.y - size.y / 2, size.x, size.y}, {work.x, work.y, work.w, work.h},
                                                                {std::max(extents.topLeft.x, border), std::max(extents.topLeft.y, border)},
                                                                {std::max(extents.bottomRight.x, border), std::max(extents.bottomRight.y, border)}))
                        g_layoutManager->setTargetGeom({box->x, box->y, box->w, box->h}, target);
                }
                target->warpPositionSize();
            });
            if (session().live() && spaces::inActive(w->m_workspace) && session().covers(w->m_monitor.lock()))
                session().hideWindow(w);
            session().damage();
        }

        void wrapManagers() {
            const auto& managers = PROTO::activation.get()->*member(SManagers{});
            std::erase_if(callbacks,
                          [&](const auto& entry) { return std::ranges::none_of(managers, [&](const auto& manager) { return manager.get() == entry.first; }); });
            for (const auto& manager : managers) {
                auto* raw = manager.get();
                if (callbacks.contains(raw))
                    continue;
                auto& request = (raw->*member(SRequests{})).activate;
                callbacks.emplace(raw, request);
                request = [native = request](CXdgActivationV1* self, const char* token, wl_resource* resource) {
                    const std::string   id              = token;
                    const auto&         tokens          = PROTO::activation.get()->*member(STokens{});
                    const bool          valid           = std::ranges::any_of(tokens, [&](const auto& sent) { return sent.token == id; });
                    const auto          surface         = CWLSurfaceResource::fromResource(resource);
                    const auto          launch          = launches.find(id);
                    const auto          originWorkspace = launch != launches.end() ? launch->second.context.workspace.lock() : nullptr;
                    const auto          window = surface ? Desktop::viewState()->query().type(Desktop::View::VIEW_TYPE_WINDOW).surface(surface).runWindow() : nullptr;
                    spaces::CSpaceScope spaceScope(window && window->m_isMapped ? spaces::owner(window->m_workspace)
                                                   : originWorkspace            ? spaces::owner(originWorkspace)
                                                                                : "");
                    // Run native validation and activation unchanged, even for
                    // unsupported or already-existing application windows.
                    native(self, token, resource);
                    if (!valid || !launches.contains(id) || !surface)
                        return;
                    const auto w = Desktop::viewState()->query().type(Desktop::View::VIEW_TYPE_WINDOW).surface(surface).runWindow();
                    if (w && w->m_isMapped)
                        place(id, w);
                    else
                        pending.push_back({surface, id});
                };
            }
        }

        void bind(CXDGActivationProtocol* self, wl_client* client, void* data, uint32_t version, uint32_t id) {
            using Fn = void (*)(CXDGActivationProtocol*, wl_client*, void*, uint32_t, uint32_t);
            reinterpret_cast<Fn>(bindHook->m_original)(self, client, data, version, id);
            wrapManagers();
        }

        void destroyManager(CXDGActivationProtocol* self, wl_resource* resource) {
            using Fn = void (*)(CXDGActivationProtocol*, wl_resource*);
            for (const auto& manager : self->*member(SManagers{}))
                if (manager->resource() == resource)
                    callbacks.erase(manager.get());
            reinterpret_cast<Fn>(destroyHook->m_original)(self, resource);
        }

        void map(Desktop::View::CWindow* self) {
            using Fn = void (*)(Desktop::View::CWindow*);
            // Reading this exact process's environment does not consume the
            // next arbitrary window and does not affect existing processes.
            const auto env      = self->getEnv();
            const auto origin   = env.find("HYPRSPACE_SPACE_ID");
            auto       originId = origin == env.end() ? "" : origin->second;
            if (const auto token = env.find("HYPRSPACE_LAUNCH_TOKEN"); token != env.end())
                if (const auto launch = launches.find(token->second); launch != launches.end()) {
                    const auto capturedWorkspace = launch->second.context.workspace.lock();
                    originId                     = capturedWorkspace ? spaces::owner(capturedWorkspace) : launch->second.context.space;
                }
            spaces::CSpaceScope spaceScope(originId);
            reinterpret_cast<Fn>(mapHook->m_original)(self);
            const auto w = self->m_self.lock();
            if (const auto it = env.find("HYPRSPACE_LAUNCH_TOKEN"); it != env.end())
                place(it->second, w);
            for (auto it = pending.begin(); it != pending.end();) {
                if (w && w->wlSurface() && it->surface == w->wlSurface()->resource()) {
                    place(it->token, w);
                    it = pending.erase(it);
                } else
                    ++it;
            }
        }

        Env launchEnv(Config::Supplementary::CExecutor* self, PHLWORKSPACE ws) {
            using Fn = Env (*)(Config::Supplementary::CExecutor*, PHLWORKSPACE);
            auto env = reinterpret_cast<Fn>(envHook->m_original)(self, ws);
            if (spaces::enabled())
                env.emplace_back("HYPRSPACE_SPACE_ID", ws ? spaces::owner(ws) : spaces::activeId());
            auto       target  = commandTarget;
            const bool desktop = spaces::enabled() && !spaces::nativeDefault() && !session().live();
            if (!target && desktop) {
                const auto monitor     = ws ? ws->m_monitor.lock() : Desktop::focusState()->monitor();
                const auto destination = ws ? ws : monitor ? monitor->m_activeWorkspace : nullptr;
                if (destination && monitor) {
                    const auto box = monitor->logicalBoxMinusReserved();
                    target =
                        SOverviewTarget{.workspace = {destination->m_id, destination->m_name}, .monitor = monitor, .desktop = {box.x + box.w / 2, box.y + box.h / 2}};
                }
            }
            if (const auto token = consume(capture(target, desktop)); !token.empty()) {
                // Do not let a stale initial-workspace token compete with
                // explicit window rules. Placement follows native rule parsing.
                for (const auto& [name, value] : env)
                    if (name == "HL_INITIAL_WORKSPACE_TOKEN")
                        g_pTokenManager->removeToken(g_pTokenManager->getToken(value));
                std::erase_if(env, [](const auto& pair) { return pair.first == "HL_INITIAL_WORKSPACE_TOKEN"; });
                env.emplace_back("HL_INITIAL_WORKSPACE_TOKEN", "");
                env.emplace_back("HYPRSPACE_LAUNCH_TOKEN", token);
                env.emplace_back("XDG_ACTIVATION_TOKEN", token);
                Dl_info library{};
                if (dladdr(reinterpret_cast<void*>(launchEnv), &library) && library.dli_fname) {
                    auto directory = std::filesystem::absolute(library.dli_fname).parent_path() / "launch-bin";
                    if (!std::filesystem::is_directory(directory)) {
                        if (const auto data = getenv("XDG_DATA_HOME"); data && *data)
                            directory = std::filesystem::path(data) / "hyprspace/launch-bin";
                        else if (const auto home = getenv("HOME"))
                            directory = std::filesystem::path(home) / ".local/share/hyprspace/launch-bin";
                    }
                    env.emplace_back("PATH", directory.string() + ":" + (getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin"));
                }
            }
            return env;
        }

        nlohmann::json spaceStatus() {
            const auto     state = spaces::snapshot();
            nlohmann::json result{{"type", "spaces"},
                                  {"schema", 1},
                                  {"enabled", state.value("enabled", false)},
                                  {"active", state.value("active", std::string{})},
                                  {"generation", state.value("generation", uint64_t{0})},
                                  {"spaces", nlohmann::json::array()},
                                  {"workspaces", nlohmann::json::array()},
                                  {"monitors", nlohmann::json::array()}};
            const auto     focused = Desktop::focusState()->monitor();
            for (const auto& space : state.at("spaces")) {
                size_t windows = 0, occupied = 0;
                for (const auto& workspace : space.at("workspaces")) {
                    const auto count = workspace.value("windows", size_t{0});
                    windows += count;
                    occupied += count > 0 && !workspace.value("special", false);
                    if (!space.value("active", false))
                        continue;
                    auto       value   = workspace;
                    const auto native  = State::workspaceState()->query().id(workspace.at("native_id").get<int64_t>()).run();
                    const auto monitor = valid(native) ? native->m_monitor.lock() : nullptr;
                    const bool visible = monitor && (monitor->m_activeWorkspace == native || monitor->m_activeSpecialWorkspace == native);
                    value["active"]    = visible;
                    value["visible"]   = visible;
                    value["focused"]   = visible && monitor == focused;
                    value["occupied"]  = count > 0;
                    result["workspaces"].push_back(std::move(value));
                }
                result["spaces"].push_back({{"id", space.at("id")},
                                            {"name", space.at("name")},
                                            {"icon", space.value("icon", "grid")},
                                            {"color", space.value("color", "#89b4fa")},
                                            {"active", space.at("active")},
                                            {"windows", windows},
                                            {"occupied_workspaces", occupied}});
            }
            for (const auto& monitor : State::monitorState()->monitors()) {
                if (!monitor || !monitor->m_enabled || monitor->isMirror())
                    continue;
                const auto  workspace = monitor->m_activeWorkspace;
                std::string identity;
                if (workspace)
                    for (const auto& value : result["workspaces"])
                        if (value["native_id"] == workspace->m_id) {
                            identity = value["id"].get<std::string>();
                            break;
                        }
                result["monitors"].push_back({{"name", monitor->m_name},
                                              {"workspace_uuid", identity},
                                              {"number", workspace && spaces::inActive(workspace) ? spaces::localNumber(workspace) : 0},
                                              {"focused", monitor == focused}});
            }
            return result;
        }

        std::string workspaceRequest(const std::string& text) {
            const auto reply = [](bool success, const std::string& error = "") { return nlohmann::json{{"success", success}, {"error", error}}.dump(); };
            try {
                const auto value = nlohmann::json::parse(text);
                if (!value.is_object() || !value.contains("space") || !value["space"].is_string() || !value.contains("monitor") || !value["monitor"].is_string() ||
                    !value.contains("workspace") || !value["workspace"].is_string())
                    return reply(false, "Expected space, monitor and workspace strings");
                if (!overlaysAllowed())
                    return reply(false, "Session is locked");
                if (!spaces::enabled())
                    return reply(false, "Spaces are disabled");
                if (value["space"].get<std::string>() != spaces::activeId())
                    return reply(false, "The active space changed; refresh the workspace strip");
                const auto monitor = State::monitorState()->query().name(value["monitor"].get<std::string>()).run();
                if (!monitor || !monitor->m_enabled || monitor->isMirror())
                    return reply(false, "Monitor is not connected");
                const auto selector = value["workspace"].get<std::string>();
                if (selector.empty() || selector.size() > 256 || selector.find_first_of("\n\r\0", 0, 3) != std::string::npos || selector.starts_with("name:hyprspace:") ||
                    selector.starts_with("special:hyprspace:"))
                    return reply(false, "Invalid local workspace selector");
                const auto previous = Desktop::focusState()->monitor();
                (void)Config::Actions::focusMonitor(monitor);
                const auto destination = getWorkspaceIDNameFromString(selector);
                if (destination.id == WORKSPACE_INVALID) {
                    if (previous)
                        (void)Config::Actions::focusMonitor(previous);
                    return reply(false, "Workspace does not resolve inside the active space");
                }
                const auto native = State::workspaceState()->query().id(destination.id).run();
                if (valid(native) && !spaces::inActive(native)) {
                    if (previous)
                        (void)Config::Actions::focusMonitor(previous);
                    return reply(false, "Workspace belongs to another space");
                }
                // Clicking the selected slot is idempotent, even when native
                // workspace back-and-forth is enabled.
                if (valid(native) && native->m_monitor && native->m_monitor->m_activeWorkspace == native) {
                    (void)Config::Actions::focusMonitor(native->m_monitor.lock());
                    // No native workspace event fires for an active slot.
                    // Its click must still replace a hovered overview target.
                    if (session().live() && !session().gestureActive() && !session().pendingResize)
                        session().followKeyboardFocus();
                    return reply(true);
                }
                const auto dispatcher = g_pKeybindManager->m_dispatchers.find("workspace");
                if (dispatcher == g_pKeybindManager->m_dispatchers.end())
                    return reply(false, "Workspace dispatcher is unavailable");
                const auto result = dispatcher->second(selector);
                return reply(result.success, result.error);
            } catch (const std::exception& error) {
                return reply(false, error.what());
            }
        }

        std::string request(const std::string& line) {
            if (line == "spaces")
                return spaceStatus().dump();
            if (line.starts_with("workspace "))
                return workspaceRequest(line.substr(10));
            if (line == "active")
                return session().live() && overlaysAllowed() ? "1" : "";
            if (line == "capture")
                return capture(session().selection.command());
            if (line.starts_with("consume "))
                return consume(line.substr(8));
            if (line == "status") {
                nlohmann::json result{{"live", session().live()}, {"dragging", session().gestureActive()}, {"views", nlohmann::json::array()}};
                result["spaces"]             = spaces::snapshot();
                result["space_selector"]     = spaceSelector().inspect();
                result["input"]              = inputController().inspect();
                result["modifiers"]          = g_pInputManager->getModsFromAllKBs();
                result["keyboard_owned"]     = hooks::keyboardOwned();
                result["cursor_owned"]       = session().cursorOwned();
                result["overview_cursor"]    = hooks::cursorName();
                const auto       zoomTarget  = session().zoomTarget();
                const auto       zoomMonitor = zoomTarget ? zoomTarget->monitor.lock() : nullptr;
                const COverview* zoomView    = nullptr;
                for (const auto& view : session().views)
                    if (!view->closing() && view->monitor() == zoomMonitor)
                        zoomView = view.get();
                const auto* inspectionBoard = session().windowViewActive() ? session().windowBoard() : nullptr;
                result["zoom"] = {{"held", session().zoomHeld()},
                                  {"workspace", zoomTarget ? zoomTarget->workspace.id : 0},
                                  {"monitor", zoomMonitor ? zoomMonitor->m_name : ""},
                                  {"returning", session().zoomLocked() && !session().zoomHeld()},
                                  {"extra_factor", inspectionBoard ? inspectionBoard->inspectionFactor()
                                                   : zoomView      ? zoomView->inspectionFactor()
                                                                   : 1.0},
                                  {"extra_goal", inspectionBoard ? inspectionBoard->inspectionGoal()
                                                 : zoomView      ? zoomView->inspectionGoal()
                                                                 : 1.0},
                                  {"inspection_ready", session().zoomHeld() && (inspectionBoard ? inspectionBoard->inspecting() && config::overviewWheelZoom()
                                                                                                : zoomView && zoomView->inspectionReady())},
                                  {"camera_settled", session().zoomHeld() && !session().cursorFollowSettling() &&
                                                         (inspectionBoard ? inspectionBoard->inspectionSettled() : zoomView && zoomView->zoomNavigationReady())},
                                  {"inspection_transitioning", inspectionBoard ? inspectionBoard->inspecting() && !inspectionBoard->inspectionSettled()
                                                                               : zoomView && zoomView->inspectionTransitioning()},
                                  {"cursor_follow_enabled", session().cursorFollowEnabled()},
                                  {"cursor_follow_hint", session().cursorFollowHint()},
                                  {"cursor_follow_settling", session().cursorFollowSettling()}};
                result["zoom"]["pan_held"]      = session().panHeld();
                result["zoom"]["panning"]       = session().panning();
                result["zoom"]["pan_available"] = session().panAvailable();
                result["cursor_shape"]          = hooks::cursorShape();
                result["pending_resize"]        = session().pendingResize.has_value();
                result["help"]                  = {{"active", session().helpActive()}, {"query", ""}, {"search_focused", false}};
                if (const auto* help = session().helpOverlay(); help && help->active()) {
                    const auto  mon       = help->monitor();
                    const auto& browser   = help->browser();
                    const auto  globalBox = [&](const SBoxF& box) {
                        return nlohmann::json{{"x", box.x + (mon ? mon->m_position.x : 0)}, {"y", box.y + (mon ? mon->m_position.y : 0)}, {"w", box.w}, {"h", box.h}};
                    };
                    auto entries = nlohmann::json::array();
                    for (size_t i = 0; i < browser.count(); ++i) {
                        const auto& entry = browser.entry(i);
                        entries.push_back({{"id", entry.id}, {"title", entry.title}, {"keys", entry.keys}, {"topic", HELP_TOPICS[entry.topic]}});
                    }
                    const auto selected = browser.selection();
                    result["help"]      = {{"active", true},
                                           {"monitor", mon ? mon->m_name : ""},
                                           {"generation", help->generation()},
                                           {"query", help->search().text()},
                                           {"query_cursor", help->search().cursor()},
                                           {"search_focused", help->searchFocused()},
                                           {"topic", HELP_TOPICS[browser.topic()]},
                                           {"count", browser.count()},
                                           {"total", browser.total()},
                                           {"selected_index", browser.selected()},
                                           {"selected_id", selected ? selected->id : ""},
                                           {"selected_description", selected ? selected->description : ""},
                                           {"scroll", help->scrollOffset()},
                                           {"entries", entries},
                                           {"panel", globalBox(help->layout().panel)},
                                           {"search_box", globalBox(help->layout().search)},
                                           {"results_box", globalBox(help->layout().results)},
                                           {"detail_box", globalBox(help->layout().detail)},
                                           {"row_height", help->layout().rowHeight}};
                }
                result["window_board"] = nullptr;
                if (const auto* board = session().windowBoard()) {
                    const auto host    = board->monitor();
                    const auto chosen  = board->selectedTarget();
                    const auto boxJSON = [](const SBoxF& box) { return nlohmann::json{{"x", box.x}, {"y", box.y}, {"w", box.w}, {"h", box.h}}; };
                    auto&      state   = result["window_board"];
                    state              = {{"active", board->active()},
                                          {"monitor", host ? host->m_name : ""},
                                          {"grouping", windowGroupingName(board->grouping())},
                                          {"sort", board->recent() ? "recent" : "location"},
                                          {"query", board->query()},
                                          {"search_focused", board->searchFocused()},
                                          {"search_cursor", board->searchCursor()},
                                          {"search_selection", {board->searchSelection().first, board->searchSelection().second}},
                                          {"search_offset", board->searchOffset()},
                                          {"search_box", boxJSON(board->searchBox())},
                                          {"search_caret", boxJSON(board->searchCaretBox())},
                                          {"search_caret_visible", board->searchCaretVisible()},
                                          {"count", board->layout().cards.size()},
                                          {"scroll", board->scrollOffset()},
                                          {"viewport", boxJSON(board->viewport())},
                                          {"selected_window", std::format("0x{:x}", reinterpret_cast<uintptr_t>(chosen ? chosen->window.lock().get() : nullptr))},
                                          {"inspecting", board->inspecting()},
                                          {"inspection_factor", board->inspectionFactor()},
                                          {"inspection_goal", board->inspectionGoal()},
                                          {"groups", nlohmann::json::array()},
                                          {"previews", nlohmann::json::array()}};
                    for (const auto& heading : board->layout().headings)
                        state["groups"].push_back({{"label", heading.label}, {"count", heading.count}});
                    for (const auto& target : board->inspectTargets()) {
                        auto box              = boxJSON(target.preview);
                        box["window"]         = std::format("0x{:x}", reinterpret_cast<uintptr_t>(target.window.lock().get()));
                        box["workspace"]      = target.workspace.id;
                        const auto source     = target.monitor.lock();
                        box["source_monitor"] = source ? source->m_name : "";
                        state["previews"].push_back(std::move(box));
                    }
                }
                if (session().drag.active()) {
                    const auto& drag    = session().drag;
                    const auto  boxJSON = [](const SBoxF& box) { return nlohmann::json{{"x", box.x}, {"y", box.y}, {"w", box.w}, {"h", box.h}}; };
                    result["drag"]      = {{"kind", drag.mode == SOverviewDrag::RESIZE ? "resize" : "move"},
                                           {"resize", drag.mode == SOverviewDrag::RESIZE},
                                           {"moved", drag.moved},
                                           {"threshold", drag.threshold},
                                           {"workspace", drag.source.workspace.id},
                                           {"box", boxJSON(drag.box)},
                                           {"clip", boxJSON(drag.source.previewClip)}};
                    for (const auto& view : session().views)
                        if (view->monitor() == drag.source.monitor)
                            for (const auto& target : view->inspectTargets())
                                if (!target.window && target.workspace == drag.source.workspace)
                                    result["drag"]["clip"] = boxJSON(target.preview);
                }
                if (session().workspaceDrag) {
                    const auto& drag   = *session().workspaceDrag;
                    const auto  source = drag.sourceMonitor.lock(), destination = drag.targetMonitor.lock();
                    const auto  boxJSON = [](const SBoxF& box) { return nlohmann::json{{"x", box.x}, {"y", box.y}, {"w", box.w}, {"h", box.h}}; };
                    result["drag"]      = {{"kind", "workspace"},
                                           {"resize", false},
                                           {"moved", drag.moved},
                                           {"threshold", drag.threshold},
                                           {"workspace", drag.identity.id},
                                           {"box", boxJSON(drag.box)},
                                           {"clip", boxJSON(drag.preview.cell)},
                                           {"source_monitor", source ? source->m_name : ""},
                                           {"target_monitor", destination ? destination->m_name : ""}};
                }
                if (const auto picture = session().workspaceSettlePreview()) {
                    const auto& settle         = *session().workspaceSettle;
                    const auto  monitor        = settle.monitor.lock();
                    const auto  boxJSON        = [](const SBoxF& box) { return nlohmann::json{{"x", box.x}, {"y", box.y}, {"w", box.w}, {"h", box.h}}; };
                    result["workspace_settle"] = {{"workspace", settle.identity.id},
                                                  {"monitor", monitor->m_name},
                                                  {"progress", picture->progress},
                                                  {"box", boxJSON(picture->cell)},
                                                  {"windows", nlohmann::json::array()}};
                    for (const auto& view : session().views)
                        if (view->monitor() == monitor)
                            if (auto destination = view->workspacePreview(settle.identity))
                                result["workspace_settle"]["destination"] = boxJSON(destination->cell);
                    for (const auto& slot : picture->windows)
                        result["workspace_settle"]["windows"].push_back({{"box", boxJSON(slot.box)}, {"clip", boxJSON(slot.clip)}, {"visibility", slot.visibility}});
                }
                result["layout_targets_unique"] = true;
                for (const auto& workspace : State::workspaceState()->workspaces()) {
                    if (!workspace->m_space)
                        continue;
                    std::vector<SP<Layout::ITarget>> seen;
                    for (const auto& weak : workspace->m_space->targets()) {
                        const auto target = weak.lock();
                        if (target && std::ranges::contains(seen, target))
                            result["layout_targets_unique"] = false;
                        if (target)
                            seen.push_back(target);
                    }
                }
                if (auto target = session().selection.command())
                    result["target"] = {{"workspace", target->workspace.id},
                                        {"name", target->workspace.name},
                                        {"x", target->desktop.x},
                                        {"y", target->desktop.y},
                                        {"window", std::format("0x{:x}", reinterpret_cast<uintptr_t>(target->window.lock().get()))}};
                for (const auto& view : session().views) {
                    if (auto mon = view->monitor()) {
                        nlohmann::json tiles = nlohmann::json::array();
                        for (const auto& target : view->inspectTargets()) {
                            const auto& box = target.preview;
                            tiles.push_back({{"workspace", target.workspace.id},
                                             {"window", std::format("0x{:x}", reinterpret_cast<uintptr_t>(target.window.lock().get()))},
                                             {"x", box.x},
                                             {"y", box.y},
                                             {"w", box.w},
                                             {"h", box.h}});
                        }
                        const auto&    counts      = view->layoutCounters();
                        nlohmann::json emptyButton = nullptr;
                        if (const auto box = view->emptyWorkspaceButton())
                            emptyButton = {{"x", box->x}, {"y", box->y}, {"w", box->w}, {"h", box->h}};
                        nlohmann::json edges      = nlohmann::json::array();
                        nlohmann::json followHint = nullptr;
                        if (const auto box = view->cursorFollowHintBox())
                            followHint = {{"x", mon->m_position.x + box->x}, {"y", mon->m_position.y + box->y}, {"w", box->w}, {"h", box->h}};
                        for (const auto& hint : view->zoomEdgeHints()) {
                            const char* direction = "";
                            switch (hint.direction) {
                            case EDirection::LEFT:
                                direction = "left";
                                break;
                            case EDirection::RIGHT:
                                direction = "right";
                                break;
                            case EDirection::UP:
                                direction = "up";
                                break;
                            case EDirection::DOWN:
                                direction = "down";
                                break;
                            }
                            edges.push_back({{"direction", direction},
                                             {"workspace", hint.workspace.id},
                                             {"pending", hint.pending},
                                             {"x", mon->m_position.x + hint.box.x},
                                             {"y", mon->m_position.y + hint.box.y},
                                             {"w", hint.box.w},
                                             {"h", hint.box.h}});
                        }
                        result["views"].push_back({{"monitor", mon->m_name},
                                                   {"tiles", tiles},
                                                   {"empty_workspace_button", emptyButton},
                                                   {"empty_workspace_error", view->emptyWorkspaceError()},
                                                   {"zoom_edges", edges},
                                                   {"cursor_follow_hint", followHint},
                                                   {"layout",
                                                    {{"frames", counts.frames},
                                                     {"window_updates", counts.windowUpdates},
                                                     {"window_layouts", counts.windowLayouts},
                                                     {"spread_layouts", counts.spreadLayouts},
                                                     {"grid_layouts", counts.gridLayouts}}}});
                    }
                }
                const auto& captures = captureResources();
                const auto& tex      = CTextureCache::resources();
                const auto& filters  = previewFilterResources();
                diagnostics::enable(config::diagnosticsEnabled());
                result["diagnostics"] = diagnostics::snapshot();
                result["resources"]   = {{"captures",
                                          {{"bytes", captures.bytes},
                                           {"peak_bytes", captures.peakBytes},
                                           {"limit_bytes", SCaptureResources::MAX_BYTES},
                                           {"per_capture_bytes", SCaptureResources::MAX_CAPTURE_BYTES},
                                           {"limit_entries", SCaptureResources::MAX_CAPTURES},
                                           {"attempts", captures.attempts},
                                           {"downsampled", captures.downsampled},
                                           {"failures", captures.failures},
                                           {"fallbacks", captures.fallbacks},
                                           {"previous", captures.previous},
                                           {"omitted", captures.omitted}}},
                                         {"preview_filter",
                                          {{"mode", config::overviewPreviewFilter()},
                                           {"entries", filters.entries},
                                           {"bytes", filters.bytes},
                                           {"peak_bytes", filters.peakBytes},
                                           {"passes", filters.passes},
                                           {"hits", filters.hits},
                                           {"allocations", filters.allocations},
                                           {"animated_bypasses", filters.animatedBypasses},
                                           {"failures", filters.failures}}},
                                         {"textures",
                                          {{"entries", textures().size()},
                                           {"peak_entries", tex.peakEntries},
                                           {"limit_entries", CTextureCache::SResources::MAX_ENTRIES},
                                           {"bytes", tex.bytes},
                                           {"peak_bytes", tex.peakBytes},
                                           {"limit_bytes", CTextureCache::SResources::MAX_BYTES},
                                           {"hits", tex.hits},
                                           {"misses", tex.misses},
                                           {"evictions", tex.evictions},
                                           {"failures", tex.failures}}}};
                result["windows"]     = nlohmann::json::array();
                for (const auto& w : Desktop::windowState()->windows())
                    if (w->m_isMapped)
                        result["windows"].push_back({{"address", std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get()))},
                                                     {"alpha", w->alpha(Desktop::View::WINDOW_ALPHA_FADE)->goal()},
                                                     {"suspended", w.get()->*member(SSuspended{})},
                                                     {"space", spaces::owner(w->m_workspace)}});
                return result.dump();
            }
            return {};
        }

        int onClient(int fd, uint32_t mask, void* data) {
            auto*      client = static_cast<SClient*>(data);
            char       buffer[8192];
            const auto count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (count < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
                return 0;
            std::string reply;
            if (count > 0 && count < static_cast<ssize_t>(sizeof(buffer))) {
                try {
                    reply = request(std::string(buffer, count));
                } catch (...) {
                    reply.clear();
                }
            }
            send(fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            wl_event_source_remove(client->source);
            close(fd);
            std::erase_if(clients, [&](const auto& entry) { return entry.get() == client; });
            return 0;
        }

        int acceptClient(int fd, uint32_t, void*) {
            for (int n = 0; n < 16; ++n) {
                const int peer = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (peer < 0)
                    break;
                ucred     credentials{};
                socklen_t length = sizeof(credentials);
                if (clients.size() >= 32 || getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || credentials.uid != getuid()) {
                    close(peer);
                    continue;
                }
                // Live preview diagnostics can exceed the kernel's default
                // single-packet send budget.
                const int replyBudget = 1024 * 1024;
                setsockopt(peer, SOL_SOCKET, SO_SNDBUF, &replyBudget, sizeof(replyBudget));
                auto client    = std::make_unique<SClient>(SClient{peer, nullptr, Clock::now() + std::chrono::seconds(2)});
                client->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, peer, WL_EVENT_READABLE, onClient, client.get());
                if (!client->source) {
                    close(peer);
                    continue;
                }
                clients.push_back(std::move(client));
            }
            return 0;
        }
    } // namespace

    void duringCommand(const std::optional<SOverviewTarget>& target, const std::function<void()>& action) {
        const auto previous = commandTarget;
        commandTarget       = target;
        try {
            action();
        } catch (...) {
            commandTarget = previous;
            throw;
        }
        commandTarget = previous;
    }

    void clear() {
        commandTarget.reset();
        captures.clear();
        auto& tokens = PROTO::activation.get()->*member(STokens{});
        for (const auto& [id, context] : launches) {
            std::erase_if(tokens, [&](const auto& token) { return token.token == id; });
            g_pTokenManager->removeToken(g_pTokenManager->getToken(id));
        }
        launches.clear();
        pending.clear();
    }

    void install() {
        try {
            bindHook    = hooks::attach("bindManager", "CXDGActivationProtocol::bindManager(", reinterpret_cast<void*>(bind));
            destroyHook = hooks::attach("onManagerResourceDestroy", "CXDGActivationProtocol::onManagerResourceDestroy(", reinterpret_cast<void*>(destroyManager));
            mapHook     = hooks::attach("mapWindow", "Desktop::View::CWindow::mapWindow(", reinterpret_cast<void*>(map));
            envHook     = hooks::attach("getHyprlandLaunchEnv", "Config::Supplementary::CExecutor::getHyprlandLaunchEnv(", reinterpret_cast<void*>(launchEnv));
            wrapManagers();
            const auto runtime   = getenv("XDG_RUNTIME_DIR");
            const auto signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
            if (!runtime || !signature)
                throw std::runtime_error("[hyprspace] missing compositor runtime directory");
            socketPath = std::string(runtime) + "/hypr/" + signature + "/hyprspace.sock";
            sockaddr_un address{.sun_family = AF_UNIX};
            if (socketPath.size() >= sizeof(address.sun_path))
                throw std::runtime_error("[hyprspace] runtime socket path too long");
            std::copy(socketPath.begin(), socketPath.end(), address.sun_path);
            socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (socketFd < 0 || ::bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                throw std::runtime_error("[hyprspace] cannot create private launch socket");
            socketBound = true;
            if (chmod(socketPath.c_str(), 0600) != 0 || listen(socketFd, 16) != 0)
                throw std::runtime_error("[hyprspace] cannot listen on private launch socket");
            socketSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, socketFd, WL_EVENT_READABLE, acceptClient, nullptr);
            if (!socketSource)
                throw std::runtime_error("[hyprspace] cannot register private launch socket");
            timer = makeShared<CEventLoopTimer>(
                std::chrono::seconds(1),
                [](SP<CEventLoopTimer> self, void*) {
                    prune();
                    std::erase_if(clients, [](const auto& client) {
                        if (client->expires > Clock::now())
                            return false;
                        wl_event_source_remove(client->source);
                        close(client->fd);
                        return true;
                    });
                    self->updateTimeout(std::chrono::seconds(1));
                },
                nullptr);
            g_pEventLoopManager->addTimer(timer);
            if (g_pEventManager)
                g_pEventManager->postEvent({.event = "custom", .data = "hyprspace:ready"});
        } catch (...) {
            uninstall();
            throw;
        }
    }

    void uninstall() {
        clear();
        for (const auto& manager : PROTO::activation.get()->*member(SManagers{}))
            if (auto saved = callbacks.find(manager.get()); saved != callbacks.end())
                (manager.get()->*member(SRequests{})).activate = std::move(saved->second);
        callbacks.clear();
        for (auto handle : {bindHook, destroyHook, mapHook, envHook})
            if (handle)
                HyprlandAPI::removeFunctionHook(PHANDLE, handle);
        bindHook = destroyHook = mapHook = envHook = nullptr;
        if (timer)
            g_pEventLoopManager->removeTimer(timer);
        timer.reset();
        for (const auto& client : clients) {
            wl_event_source_remove(client->source);
            close(client->fd);
        }
        clients.clear();
        if (socketSource)
            wl_event_source_remove(socketSource);
        socketSource = nullptr;
        if (socketFd >= 0)
            close(socketFd);
        if (socketBound)
            unlink(socketPath.c_str());
        socketBound = false;
        socketFd    = -1;
        if (g_pEventManager)
            g_pEventManager->postEvent({.event = "custom", .data = "hyprspace:unavailable"});
    }
} // namespace hyprspace::launch
