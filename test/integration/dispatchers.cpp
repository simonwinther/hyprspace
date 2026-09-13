#include "generation.hpp"

// Independent library: unloading it must invalidate its dispatcher code.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <stdexcept>

namespace {
    HANDLE handle;

    SDispatchResult raw(std::string) {
        return {.success = false, .error = "fixture:raw"};
    }

    void replace(const std::string& name, const std::string& value) {
        HyprlandAPI::addDispatcherV2(handle, name, [value](std::string) { return SDispatchResult{.success = false, .error = "fixture:" + value}; });
    }
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE plugin) {
    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        throw std::runtime_error("dispatcher test fixture ABI mismatch");
    handle = plugin;
    replace("hyprspace-test:ping", "original");
    HyprlandAPI::addDispatcher(handle, "hyprspace-test:legacy", [](std::string) {});
    g_pKeybindManager->m_dispatchers["hyprspace-test:raw"]     = raw;
    g_pKeybindManager->m_dispatchers["hyprspace-test:closure"] = [](std::string) { return raw({}); };
    HyprlandAPI::addDispatcherV2(handle, "hyprspace-test:replace", [](std::string value) {
        replace("submap", value);
        return SDispatchResult{};
    });
    return {"hyprspace-dispatcher-test", "Private dispatcher lifetime fixture", "hyprspace", "1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pKeybindManager->m_dispatchers.erase("hyprspace-test:raw");
    g_pKeybindManager->m_dispatchers.erase("hyprspace-test:closure");
}
