#include "generation.hpp"
#include "resources.hpp"

// Test-only state transitions executed inside the private compositor.
#include "../../src/Overview.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace hyprspace;

namespace {
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
        const auto  result    = nlohmann::json{
            {"unknown", overrides[CURSOR_OVERRIDE_UNKNOWN]}, {"window_edge", overrides[CURSOR_OVERRIDE_WINDOW_EDGE]}, {"shape", cursorShape(*overrideController)}};
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
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:cursor-overrides", cursorOverrideProbe);
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:pointer-state", pointerState);
    return {"hyprspace-overview-test", "Private overview regression fixture", "hyprspace", "1"};
}

APICALL EXPORT void PLUGIN_EXIT() {}
