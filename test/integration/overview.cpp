// Test-only state transitions executed inside the private compositor.
#include "../../src/Overview.hpp"

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

    using LayoutMethod = void (COverview::*)();
    LayoutMethod computeLayout();
    template <LayoutMethod Method> struct CLayoutAccess {
        friend LayoutMethod computeLayout() {
            return Method;
        }
    };
    template struct CLayoutAccess<&COverview::computeLayout>;

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
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        throw std::runtime_error("overview test fixture ABI mismatch");
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:empty-refresh", emptyRefresh);
    return {"hyprspace-overview-test", "Private overview regression fixture", "hyprspace", "1"};
}

APICALL EXPORT void PLUGIN_EXIT() {}
