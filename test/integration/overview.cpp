#include "generation.hpp"
#include "resources.hpp"
#include "capture_fixture.hpp"

// Test-only state transitions executed inside the private compositor.
#include "../../src/Overview.hpp"
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include "../../src/CompositorHooks.hpp"
#include <nlohmann/json.hpp>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace hyprspace;

namespace {
    PHLMONITORREF clockMonitor;
    float         originalRefresh = 0;
    void          resetClock() {
        if (const auto monitor = clockMonitor.lock())
            monitor->m_refreshRate = originalRefresh;
        clockMonitor.reset();
    }
    SDispatchResult resizeClock(std::string args) {
        resetClock();
        if (args == "reset")
            return {};
        if (args != "hold")
            return {.success = false, .error = "expected hold or reset"};
        const auto monitor = g_pHyprRenderer->m_mostHzMonitor.lock();
        if (!monitor)
            return {.success = false, .error = "no renderer clock monitor"};
        clockMonitor           = monitor;
        originalRefresh        = monitor->m_refreshRate;
        monitor->m_refreshRate = 5;
        return {};
    }
    // Explicit instantiation permits access without changing the class layout
    // or exposing test dispatchers in the installed plugin.
    auto& entries(COverview& view);
    template <auto Member> struct CEntryAccess {
        friend auto& entries(COverview& view) {
            return view.*Member;
        }
    };
    template struct CEntryAccess<&COverview::m_entries>;

    auto& inspectionProgress(COverview& view);
    template <auto Member> struct CInspectionAccess {
        friend auto& inspectionProgress(COverview& view) {
            return view.*Member;
        }
    };
    template struct CInspectionAccess<&COverview::m_inspectionProgress>;

    auto& cursorOverrides(Pointer::Cursor::CShapeOverrideController& controller);
    template <auto Member> struct CCursorOverridesAccess {
        friend auto& cursorOverrides(Pointer::Cursor::CShapeOverrideController& controller) {
            return controller.*Member;
        }
    };
    template struct CCursorOverridesAccess<&Pointer::Cursor::CShapeOverrideController::m_overrides>;

    auto& cursorShape(Pointer::Cursor::CShapeOverrideController& controller);
    template <auto Member> struct CCursorShapeAccess {
        friend auto& cursorShape(Pointer::Cursor::CShapeOverrideController& controller) {
            return controller.*Member;
        }
    };
    template struct CCursorShapeAccess<&Pointer::Cursor::CShapeOverrideController::m_overrideShape>;

    SDispatchResult cursorProbe(std::string) {
        using namespace Pointer::Cursor;
        if (session().live())
            return {.success = false, .error = "error: cursor probe requires a closed overview"};
        auto&                         overrides = cursorOverrides(*overrideController);
        const auto                    previous  = overrides;
        Hyprutils::Utils::CScopeGuard restore{[previous] {
            hooks::ownCursor(false);
            for (size_t group = 0; group < previous.size(); ++group)
                overrideController->setOverride(previous[group], static_cast<eCursorShapeOverrideGroup>(group));
        }};
        try {
            overrideController->setOverride("crosshair", CURSOR_OVERRIDE_UNKNOWN);
            overrideController->setOverride("n-resize", CURSOR_OVERRIDE_WINDOW_EDGE);
            hooks::ownCursor(true);
            hooks::setCursor("grabbing");
            requireResource(overrides[CURSOR_OVERRIDE_UNKNOWN] == "grabbing" && overrides[CURSOR_OVERRIDE_WINDOW_EDGE].empty(), "cursor pickup failed");
            hooks::ownCursor(true);
            hooks::setCursor("se-resize");
            requireResource(overrides[CURSOR_OVERRIDE_UNKNOWN] == "se-resize", "cursor shape change failed");
            hooks::ownCursor(false);
            requireResource(overrides[CURSOR_OVERRIDE_UNKNOWN] == "crosshair" && overrides[CURSOR_OVERRIDE_WINDOW_EDGE] == "n-resize", "cursor restoration failed");

            hooks::ownCursor(true);
            hooks::setCursor("default");
            overrideController->setOverride("grabbing", CURSOR_OVERRIDE_UNKNOWN);
            overrideController->setOverride("crosshair", CURSOR_OVERRIDE_WINDOW_EDGE);
            hooks::ownCursor(true);
            hooks::setCursor("grabbing");
            hooks::ownCursor(true);
            hooks::setCursor("default");
            hooks::ownCursor(false);
            requireResource(overrides[CURSOR_OVERRIDE_UNKNOWN] == "grabbing" && overrides[CURSOR_OVERRIDE_WINDOW_EDGE] == "crosshair",
                            "newer external cursor was overwritten");
            return {};
        } catch (const std::exception& error) {
            return {.success = false, .error = std::string("error: ") + error.what()};
        }
    }

    SDispatchResult warpInspection(std::string arguments) {
        std::istringstream input(arguments);
        float              progress = 0;
        if (!(input >> progress) || !(input >> std::ws).eof() || !std::isfinite(progress))
            return {.success = false, .error = "expected one finite inspection progress"};
        if (!session().live())
            return {.success = false, .error = "test requires an open overview"};
        for (const auto& view : session().views)
            inspectionProgress(*view)->setValueAndWarp(progress);
        return {.success = true};
    }

    auto& transitionProgress(COverview& view);
    template <auto Member> struct CProgressAccess {
        friend auto& transitionProgress(COverview& view) {
            return view.*Member;
        }
    };
    template struct CProgressAccess<&COverview::m_progress>;

    SDispatchResult transitionFrame(std::string args) {
        float remaining = 0;
        try {
            remaining = std::stof(args);
        } catch (...) {
            return {.success = false, .error = "expected remaining close progress"};
        }
        if (!std::isfinite(remaining) || remaining < 0 || remaining > 1 || session().views.empty() ||
            std::ranges::any_of(session().views, [](const auto& view) { return !view->closing(); }))
            return {.success = false, .error = "expected closing overviews and progress between zero and one"};
        for (const auto& view : session().views) {
            transitionProgress(*view)->setValueAndWarp(remaining);
            view->damage();
        }
        return {};
    }

    using LayoutMethod = void (COverview::*)();
    LayoutMethod computeLayout();
    template <LayoutMethod Method> struct CLayoutAccess {
        friend LayoutMethod computeLayout() {
            return Method;
        }
    };
    template struct CLayoutAccess<&COverview::computeLayout>;

    SDispatchResult axisInput(std::string arguments) {
        std::istringstream input(arguments);
        int                source = 0, axis = 0, direction = 0;
        double             delta    = 0;
        int64_t            value120 = 0;
        if (!(input >> source >> axis >> delta >> value120 >> direction) || !(input >> std::ws).eof() || !std::isfinite(delta) ||
            delta < -std::numeric_limits<int32_t>::max() / 8.0 || delta > std::numeric_limits<int32_t>::max() / 8.0 || source < 0 || source > 3 || axis < 0 || axis > 1 ||
            direction < 0 || direction > 1 || value120 < -static_cast<int64_t>(std::numeric_limits<int32_t>::max()) || value120 > std::numeric_limits<int32_t>::max())
            return {.success = false, .error = "expected source(0..3) axis(0..1) finite-delta int32-value120 direction(0..1)"};
        if (!g_pInputManager || g_pInputManager->m_pointers.empty() || !g_pInputManager->m_pointers.front())
            return {.success = false, .error = "test requires a live private pointer"};

        // The virtual-pointer protocol only supports integer detents and has
        // no relative-direction request. Exercise both through the native
        // handler so the plugin's device scaling and routing hooks still run.
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        g_pInputManager->onMouseWheel({.timeMs            = static_cast<uint32_t>(milliseconds),
                                       .source            = static_cast<wl_pointer_axis_source>(source),
                                       .axis              = static_cast<wl_pointer_axis>(axis),
                                       .relativeDirection = static_cast<wl_pointer_axis_relative_direction>(direction),
                                       .delta             = delta,
                                       .deltaDiscrete     = static_cast<int32_t>(value120),
                                       .mouse             = true},
                                      g_pInputManager->m_pointers.front());
        g_pInputManager->onPointerFrame();
        return {.success = true};
    }

    SDispatchResult axisContinuity(std::string arguments) {
        if (!session().live() || !session().zoomHeld())
            return {.success = false, .error = "test requires held overview zoom"};
        std::vector<std::vector<SOverviewTarget>> before;
        for (const auto& view : session().views)
            before.push_back(view->inspectTargets());

        // Measure in one callback: IPC round trips otherwise include ordinary
        // animation frames, obscuring whether wheel takeover itself jumps.
        const auto result = axisInput(std::move(arguments));
        if (!result.success)
            return result;
        if (session().views.size() != before.size())
            return {.success = false, .error = "wheel changed the overview view count"};
        for (size_t i = 0; i < before.size(); ++i) {
            const auto after = session().views[i]->inspectTargets();
            if (after.size() != before[i].size())
                return {.success = false, .error = "wheel changed the preview count"};
            for (size_t j = 0; j < after.size(); ++j) {
                const auto &left = before[i][j], &right = after[j];
                const auto &a = left.preview, &b = right.preview;
                if (left.workspace != right.workspace || left.window.lock() != right.window.lock() || !(std::abs(a.x - b.x) <= 0.0001) ||
                    !(std::abs(a.y - b.y) <= 0.0001) || !(std::abs(a.w - b.w) <= 0.0001) || !(std::abs(a.h - b.h) <= 0.0001))
                    return {.success = false, .error = "wheel changed displayed preview geometry synchronously"};
            }
        }
        return {.success = true};
    }

    SDispatchResult zoomReleaseContinuity(std::string arguments) {
        uint32_t           keycode = 0;
        std::istringstream input(arguments);
        if (!(input >> keycode) || !(input >> std::ws).eof() || !session().live() || !session().zoomHeld())
            return {.success = false, .error = "test requires held overview zoom and one keycode"};
        const auto keyboard =
            std::ranges::find_if(g_pInputManager->m_keyboards, [](const auto& device) { return device && device->m_enabled && device->m_allowed && device->m_active; });
        if (keyboard == g_pInputManager->m_keyboards.end())
            return {.success = false, .error = "test requires an active private keyboard"};
        std::vector<std::vector<SOverviewTarget>> before;
        for (const auto& view : session().views)
            before.push_back(view->inspectTargets());

        // Deliver the captured release through the native keyboard input hook.
        // Sampling in one callback excludes normal animation advancement between
        // separate status requests, while still exercising the real zoom token.
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        g_pInputManager->onKeyboardKey({.timeMs = static_cast<uint32_t>(milliseconds), .keycode = keycode, .state = WL_KEYBOARD_KEY_STATE_RELEASED}, *keyboard);
        if (session().zoomHeld() || session().views.size() != before.size())
            return {.success = false, .error = "native release did not end the captured zoom hold"};
        for (size_t i = 0; i < before.size(); ++i) {
            const auto after = session().views[i]->inspectTargets();
            if (after.size() != before[i].size())
                return {.success = false, .error = "zoom release changed the preview count"};
            for (size_t j = 0; j < after.size(); ++j) {
                const auto& left  = before[i][j];
                const auto& right = after[j];
                const auto& a     = left.preview;
                const auto& b     = right.preview;
                if (left.workspace != right.workspace || left.window.lock() != right.window.lock() || std::abs(a.x - b.x) > 0.001 || std::abs(a.y - b.y) > 0.001 ||
                    std::abs(a.w - b.w) > 0.001 || std::abs(a.h - b.h) > 0.001)
                    return {.success = false, .error = "native zoom release changed displayed geometry synchronously"};
            }
        }
        return {.success = true};
    }

    SDispatchResult panContinuity(std::string arguments) {
        if (!session().live() || !session().zoomHeld() || !g_pInputManager || g_pInputManager->m_pointers.empty() || !g_pInputManager->m_pointers.front())
            return {.success = false, .error = "test requires held overview zoom and a live private pointer"};

        if (!arguments.empty()) {
            std::istringstream input(arguments);
            float              progress = 0;
            if (!(input >> progress) || !(input >> std::ws).eof() || !std::isfinite(progress) || progress < 0 || progress > 1)
                return {.success = false, .error = "expected optional inspection progress in 0..1"};
            const auto target = session().zoomTarget();
            for (const auto& view : session().views)
                if (target && view->monitor() == target->monitor.lock()) {
                    inspectionProgress(*view)->setValueAndWarp(progress);
                    *inspectionProgress(*view) = 1.f;
                }
        }

        std::vector<std::vector<SOverviewTarget>> before;
        for (const auto& view : session().views)
            before.push_back(view->inspectTargets());

        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        g_pInputManager->onMouseButton({.timeMs = static_cast<uint32_t>(milliseconds), .button = 273, .state = WL_POINTER_BUTTON_STATE_PRESSED, .mouse = true},
                                       g_pInputManager->m_pointers.front());
        g_pInputManager->onPointerFrame();

        if (session().views.size() != before.size())
            return {.success = false, .error = "pan pickup changed the overview view count"};
        for (size_t i = 0; i < before.size(); ++i) {
            const auto after = session().views[i]->inspectTargets();
            if (after.size() != before[i].size())
                return {.success = false, .error = "pan pickup changed the preview count"};
            for (size_t j = 0; j < after.size(); ++j) {
                const auto &left = before[i][j], &right = after[j];
                const auto &a = left.preview, &b = right.preview;
                if (left.workspace != right.workspace || left.window.lock() != right.window.lock() || !(std::abs(a.x - b.x) <= 0.0001) ||
                    !(std::abs(a.y - b.y) <= 0.0001) || !(std::abs(a.w - b.w) <= 0.0001) || !(std::abs(a.h - b.h) <= 0.0001))
                    return {.success = false, .error = "pan pickup changed displayed preview geometry synchronously"};
            }
        }
        return {.success = true};
    }

    SDispatchResult cursorOverrideProbe(std::string arguments) {
        using namespace Pointer::Cursor;
        std::istringstream input(arguments);
        std::string        operation;
        int                group = -1;
        if (!(input >> operation))
            return {.success = false, .error = "expected probe, set group name, or clear group"};
        if (operation != "probe") {
            if (!(input >> group) || group < 0 || group >= CURSOR_OVERRIDE_END)
                return {.success = false, .error = "expected cursor override group in 0..3"};
            if (operation == "set") {
                std::string name;
                if (!(input >> name) || !(input >> std::ws).eof())
                    return {.success = false, .error = "expected one cursor name"};
                overrideController->setOverride(name, static_cast<eCursorShapeOverrideGroup>(group));
            } else if (operation == "clear" && (input >> std::ws).eof())
                overrideController->unsetOverride(static_cast<eCursorShapeOverrideGroup>(group));
            else
                return {.success = false, .error = "expected probe, set group name, or clear group"};
        } else if (!(input >> std::ws).eof())
            return {.success = false, .error = "probe takes no arguments"};

        const auto& overrides = cursorOverrides(*overrideController);
        const auto  result    = nlohmann::json{{"unknown", overrides[CURSOR_OVERRIDE_UNKNOWN]},
                                               {"window_edge", overrides[CURSOR_OVERRIDE_WINDOW_EDGE]},
                                               {"shape", cursorShape(*overrideController)},
                                               {"rendered", g_pHyprRenderer->shouldRenderCursor()}};
        return {.success = false, .error = result.dump()};
    }

    SDispatchResult pointerState(std::string arguments) {
        if (!arguments.empty() || !g_pInputManager)
            return {.success = false, .error = "pointer-state takes no arguments and requires input manager"};
        auto result = nlohmann::json::array();
        for (const auto& pointer : g_pInputManager->m_pointers)
            if (pointer) {
                const auto& name            = pointer->m_hlName;
                const auto& manager         = Config::mgr();
                const bool  configured      = manager && manager->deviceConfigExists(name);
                const bool  explicitEnabled = configured && manager->deviceConfigExplicitlySet(name, "enabled");
                result.push_back({{"name", name},
                                  {"identity", std::format("0x{:x}", reinterpret_cast<uintptr_t>(pointer.get()))},
                                  {"connected", pointer->m_connected},
                                  {"has_config", configured},
                                  {"enabled_explicit", explicitEnabled},
                                  {"enabled", explicitEnabled ? manager->getDeviceInt(name, "enabled") : 1}});
            }
        return {.success = false, .error = result.dump()};
    }

    auto& workspaceRefs(State::CWorkspaceStateTracker& tracker);
    template <auto Member> struct CWorkspaceAccess {
        friend auto& workspaceRefs(State::CWorkspaceStateTracker& tracker) {
            return tracker.*Member;
        }
    };
    template struct CWorkspaceAccess<&State::CWorkspaceStateTracker::m_workspaces>;

    SDispatchResult replaceWorkspace(std::string args) {
        const auto workspace = State::workspaceState()->query().id(std::stoll(args)).run();
        const auto monitor   = workspace ? workspace->m_monitor.lock() : nullptr;
        if (!workspace || !monitor || workspace->getWindowCount() || monitor->m_activeWorkspace == workspace || monitor->m_activeSpecialWorkspace == workspace)
            return {.success = false, .error = "replacement requires an empty inactive workspace"};
        const auto id   = workspace->m_id;
        const auto name = workspace->m_name;
        workspace->setPersistent(false);
        workspace->markInert();
        std::erase(workspaceRefs(*State::workspaceState()), PHLWORKSPACEREF{workspace});
        auto replacement = State::workspaceState()->create(id, monitor->m_id, name);
        replacement->setPersistent(true);
        return {};
    }

    SDispatchResult transitionClose(std::string) {
        if (!session().live())
            return {.success = false, .error = "transition probe requires an open overview"};
        const auto snapshot = [] {
            nlohmann::json result = nlohmann::json::array();
            for (const auto& view : session().views)
                for (const auto& target : view->inspectTargets())
                    if (!target.window)
                        result.push_back(
                            {{"workspace", target.workspace.id}, {"x", target.preview.x}, {"y", target.preview.y}, {"w", target.preview.w}, {"h", target.preview.h}});
            return result;
        };
        const auto before   = snapshot();
        auto*      selected = session().keyboardView();
        if (selected)
            selected->close(true);
        for (const auto& view : session().views)
            if (view.get() != selected)
                view->close(false);
        session().stopInput();
        return {.success = false, .error = nlohmann::json{{"before", before}, {"after", snapshot()}}.dump()};
    }

    SDispatchResult emptyRefresh(std::string) {
        if (!session().live())
            return {.success = false, .error = "test requires an open overview"};

        for (const auto& view : session().views) {
            const auto selected = view->selectedTarget();
            if (!selected)
                return {.success = false, .error = "test requires a selected workspace"};

            // Reproduce collect() returning no workspaces after a populated
            // frame, exactly the state captured in the reported crash.
            auto previous = std::move(entries(*view));
            entries(*view).clear();
            (view.get()->*computeLayout())();
            view->selectTarget(*selected);
            const bool empty = view->empty() && !view->selectedTarget();

            entries(*view) = std::move(previous);
            (view.get()->*computeLayout())();
            view->selectTarget(*selected);
            if (!empty || !view->selectedTarget())
                return {.success = false, .error = "empty overview did not clear and restore selection"};
        }
        return {.success = true};
    }
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        throw std::runtime_error("overview test fixture ABI mismatch");
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:empty-refresh", emptyRefresh);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:resources", resourceProbe);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:axis", axisInput);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:axis-continuity", axisContinuity);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:inspection-progress", warpInspection);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:pan-continuity", panContinuity);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:zoom-release-continuity", zoomReleaseContinuity);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:cursor-overrides", cursorOverrideProbe);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:pointer-state", pointerState);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:capture", captureProbe);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:replace-workspace", replaceWorkspace);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:transition-close", transitionClose);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:transition-frame", transitionFrame);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:resize-clock", resizeClock);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:cursor", cursorProbe);
    return {"hyprspace-overview-test", "Private overview regression fixture", "hyprspace", "1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    resetClock();
}
