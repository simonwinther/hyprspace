#include "Launch.hpp"

#include "CompositorHooks.hpp"
#include "Overview.hpp"
#include "OverviewSession.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/TokenManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/protocols/XDGActivation.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

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
        template struct CAccess<SManagers, &CXDGActivationProtocol::m_managers>;
        template struct CAccess<SRequests, &CXdgActivationV1::requests>;
        template struct CAccess<STokens, &CXDGActivationProtocol::m_sentTokens>;

        using Clock      = std::chrono::steady_clock;
        using Env        = std::vector<std::pair<std::string, std::string>>;
        using Activation = std::function<void(CXdgActivationV1*, const char*, wl_resource*)>;
        struct SContext {
            SOverviewTarget           target;
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

        std::string capture(const std::optional<SOverviewTarget>& target) {
            if (!session().live() || g_pSessionLockManager->isSessionLocked() || !target)
                return {};
            SContext context{.target = *target};
            for (const auto& w : Desktop::windowState()->windows())
                if (w->m_isMapped)
                    context.existing.emplace_back(w);
            const auto token = g_pTokenManager->getRandomUUID();
            return captures.capture(token, std::move(context), Clock::now()) ? token : "";
        }

        std::string consume(const std::string& token) {
            prune();
            if (launches.size() >= CLaunchContexts<SContext>::LIMIT || g_pSessionLockManager->isSessionLocked())
                return {};
            auto context = captures.consume(token, Clock::now());
            if (!context || !session().workspace(context->target))
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
            auto& context        = it->second.context;
            it->second.completed = true;
            if (std::ranges::any_of(context.existing, [&](const auto& old) { return old == w; }))
                return; // existing-window activation always retains native behavior
            it->second.placed = w;
            const auto& rules = w->m_ruleApplicator->static_;
            if (!rules.workspace.empty() || !rules.monitor.empty())
                return;
            const auto ws = session().workspace(context.target);
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
                    const auto size = target->position().size();
                    g_layoutManager->setTargetGeom({point - size / 2, size}, target);
                }
                target->warpPositionSize();
            });
            if (session().live() && session().covers(w->m_monitor.lock()))
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
                    const std::string id      = token;
                    const auto&       tokens  = PROTO::activation.get()->*member(STokens{});
                    const bool        valid   = std::ranges::any_of(tokens, [&](const auto& sent) { return sent.token == id; });
                    const auto        surface = CWLSurfaceResource::fromResource(resource);
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
            const auto env = self->getEnv();
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
            if (const auto token = consume(capture(commandTarget)); !token.empty()) {
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

        std::string request(const std::string& line) {
            if (line == "active")
                return session().live() && !g_pSessionLockManager->isSessionLocked() ? "1" : "";
            if (line == "capture")
                return capture(session().selection.command());
            if (line.starts_with("consume "))
                return consume(line.substr(8));
            if (line == "status") {
                nlohmann::json result{{"live", session().live()}, {"dragging", session().drag.active()}, {"views", nlohmann::json::array()}};
                result["modifiers"]      = g_pInputManager->getModsFromAllKBs();
                result["keyboard_owned"] = hooks::keyboardOwned();
                result["cursor_owned"]   = session().cursorOwned();
                if (session().drag.active()) {
                    const auto& drag    = session().drag;
                    const auto  boxJSON = [](const SBoxF& box) { return nlohmann::json{{"x", box.x}, {"y", box.y}, {"w", box.w}, {"h", box.h}}; };
                    result["drag"]      = {{"resize", drag.mode == SOverviewDrag::RESIZE},
                                           {"workspace", drag.source.workspace.id},
                                           {"box", boxJSON(drag.box)},
                                           {"clip", boxJSON(drag.source.previewClip)}};
                    for (const auto& view : session().views)
                        if (view->monitor() == drag.source.monitor)
                            for (const auto& target : view->inspectTargets())
                                if (!target.window && target.workspace == drag.source.workspace)
                                    result["drag"]["clip"] = boxJSON(target.preview);
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
                        result["views"].push_back({{"monitor", mon->m_name}, {"tiles", tiles}});
                    }
                }
                result["windows"] = nlohmann::json::array();
                for (const auto& w : Desktop::windowState()->windows())
                    if (w->m_isMapped)
                        result["windows"].push_back(
                            {{"address", std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get()))}, {"alpha", w->alpha(Desktop::View::WINDOW_ALPHA_FADE)->goal()}});
                return result.dump();
            }
            return {};
        }

        int onClient(int fd, uint32_t mask, void* data) {
            auto*      client = static_cast<SClient*>(data);
            char       buffer[256];
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
    }
} // namespace hyprspace::launch
