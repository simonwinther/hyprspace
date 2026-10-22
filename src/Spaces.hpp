#pragma once

#include "globals.hpp"
#include "SpaceAppearance.hpp"

#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace hyprspace::spaces {
    struct SSpaceInfo {
        std::string id, name;
        bool        active = false;
        std::string icon{DEFAULT_SPACE_ICON}, color{DEFAULT_SPACE_COLOR};
    };

    void                    initialize();
    void                    shutdown();
    bool                    enabled();
    bool                    internal();
    bool                    nativeDefault();
    bool                    backgroundContext();
    const std::string&      activeId();
    uint64_t                revision();
    void                    rendered(int64_t monitor, uint64_t generation, double timeMs);
    std::string             owner(PHLWORKSPACE workspace);
    bool                    inActive(PHLWORKSPACE workspace);
    int64_t                 localNumber(PHLWORKSPACE workspace);
    std::string             localName(PHLWORKSPACE workspace);
    std::string             label(PHLWORKSPACE workspace);
    std::vector<SSpaceInfo> list();
    nlohmann::json          snapshot();
    nlohmann::json          previewRemove(const std::string& id, const std::string& destination);

    // Nullopt leaves parsing to Hyprland. Managed destinations are registered
    // before native construction, so initialization-time rules see local IDs.
    std::optional<SWorkspaceIDName> resolve(const std::string& selector, PHLMONITOR monitor = nullptr);
    std::optional<std::string>      logicalSelector(const std::string& nativeNameOrId);
    PHLWORKSPACE                    workspace(int64_t number, PHLMONITOR monitor, bool create = true);
    void                            observe(PHLWORKSPACE workspace);
    void                            reconcile();
    void                            topologyChanged();
    void                            changed(PHLWORKSPACE workspace);
    void                            setTransitionCallbacks(std::function<void()> before, std::function<void()> after);

    SDispatchResult                     dispatch(const std::string& args);
    SDispatchResult                     move(const std::string& args);
    SDispatchResult                     switchTo(const std::string& id);
    SDispatchResult                     renameWorkspace(PHLWORKSPACE workspace, const std::string& name);
    SDispatchResult                     renumberWorkspace(PHLWORKSPACE workspace, int64_t number);
    bool                                matchesIdentity(PHLWORKSPACE workspace, const std::string& selector, bool& handled);
    void                                rememberPin(PHLWINDOW window, bool pinned);
    bool                                desiredPin(PHLWINDOW window);
    const std::vector<PHLWORKSPACEREF>& workspaceRefs();
    const std::vector<PHLWINDOWREF>&    windowRefs();

    class CNativeScope {
      public:
        CNativeScope();
        ~CNativeScope();
        CNativeScope(const CNativeScope&)            = delete;
        CNativeScope& operator=(const CNativeScope&) = delete;
    };

    class CSpaceScope {
      public:
        explicit CSpaceScope(std::string id);
        ~CSpaceScope();
        CSpaceScope(const CSpaceScope&)            = delete;
        CSpaceScope& operator=(const CSpaceScope&) = delete;

      private:
        std::string m_previous;
    };
} // namespace hyprspace::spaces
