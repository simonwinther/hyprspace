#include "CompositorHooks.hpp"

#include "Overview.hpp"
#include "Launch.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>

#include <any>
#include <stdexcept>

namespace hyprspace::hooks {
    namespace {
        CFunctionHook *                          keyHook = nullptr, *inputHook = nullptr, *focusHook = nullptr, *coordsHook = nullptr, *warpHook = nullptr;
        std::optional<Vector2D>                  desktopPoint;
        std::function<bool()>                    ownsKeyboard;
        std::function<bool()>                    launchEnabled;
        decltype(CKeybindManager::m_dispatchers) dispatchers;
        int                                      dispatchDepth = 0;

        struct SKey {
            WP<IKeyboard> keyboard;
            uint32_t      code;
            bool          reserved;
            bool          suppressed;
        };
        std::vector<SKey> keys;
        std::vector<SKey> ignoredKeys;
        template <typename Tag, auto Member> struct CAccess {
            friend auto member(Tag) {
                return Member;
            }
        };
        struct SCursorOverrides {
            friend auto member(SCursorOverrides);
        };
        template struct CAccess<SCursorOverrides, &Pointer::Cursor::CShapeOverrideController::m_overrides>;
        struct SIgnoreKeyboard {
            friend auto member(SIgnoreKeyboard);
        };
        template struct CAccess<SIgnoreKeyboard, &CInputManager::shouldIgnoreVirtualKeyboard>;
        struct SRenderLayer {
            friend auto member(SRenderLayer);
        };
        template struct CAccess<SRenderLayer, &Render::IHyprRenderer::renderLayer>;
        std::optional<std::array<std::string, 2>> savedCursor;
        SP<CEventLoopTimer>                       resizeTimer;
        WP<Layout::ITarget>                       resizeTarget;
        Vector2D                                  resizePickup;
        WP<CWLSurfaceResource>                    layerKeyboard;

        class CKeepLayerKeyboard {
          public:
            explicit CKeepLayerKeyboard(bool keep) {
                if (!keep)
                    return;
                layerKeyboard = g_pSeatManager->m_state.keyboardFocus;
                exclusive.swap(g_pInputManager->m_exclusiveLSes);
                active = true;
            }
            ~CKeepLayerKeyboard() {
                if (!active)
                    return;
                for (const auto& layer : exclusive)
                    if (layer && layer->m_mapped && !std::ranges::contains(g_pInputManager->m_exclusiveLSes, layer))
                        g_pInputManager->m_exclusiveLSes.push_back(layer);
                layerKeyboard.reset();
            }

          private:
            decltype(CInputManager::m_exclusiveLSes) exclusive;
            bool                                     active = false;
        };

        void keyboardFocus(CSeatManager* self, SP<CWLSurfaceResource> surface) {
            using Fn = void (*)(CSeatManager*, SP<CWLSurfaceResource>);
            if (!layerKeyboard.expired())
                return;
            reinterpret_cast<Fn>(focusHook->m_original)(self, surface);
        }

        bool reserved(xkb_keysym_t sym, uint32_t mods) {
            if (mods == HL_MODIFIER_SHIFT)
                return sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab;
            if (mods != 0)
                return false;
            if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)
                return true;
            switch (sym) {
            case XKB_KEY_Escape:
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space:
            case XKB_KEY_Tab:
            case XKB_KEY_Left:
            case XKB_KEY_Right:
            case XKB_KEY_Up:
            case XKB_KEY_Down:
            case XKB_KEY_h:
            case XKB_KEY_j:
            case XKB_KEY_k:
            case XKB_KEY_l:
            case XKB_KEY_Home:
            case XKB_KEY_End:
                return true;
            default:
                return false;
            }
        }

        void keyboardInput(CInputManager* self, const IKeyboard::SKeyEvent& event, SP<IKeyboard> keyboard) {
            using Fn = void (*)(CInputManager*, const IKeyboard::SKeyEvent&, SP<IKeyboard>);
            std::erase_if(ignoredKeys, [](const auto& key) { return key.keyboard.expired(); });
            const auto key = std::ranges::find_if(ignoredKeys, [&](const auto& saved) { return saved.keyboard == keyboard && saved.code == event.keycode; });
            if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED && key != ignoredKeys.end()) {
                ignoredKeys.erase(key);
                return;
            }
            // IME-originated virtual keyboards intentionally bypass the native
            // binding matcher. They must not bypass overview input ownership.
            if (ownsKeyboard && ownsKeyboard() && keyboard->isVirtual() && (self->*member(SIgnoreKeyboard{}))(keyboard)) {
                if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED && key == ignoredKeys.end())
                    ignoredKeys.push_back({keyboard, event.keycode, false, true});
                return;
            }
            reinterpret_cast<Fn>(inputHook->m_original)(self, event, keyboard);
        }

        bool onKey(CKeybindManager* self, std::any event, SP<IKeyboard> keyboard) {
            using Fn            = bool (*)(CKeybindManager*, std::any, SP<IKeyboard>);
            const auto original = reinterpret_cast<Fn>(keyHook->m_original);
            const auto e        = std::any_cast<IKeyboard::SKeyEvent>(event);
            const bool pressed  = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
            const bool owned    = ownsKeyboard && ownsKeyboard();
            std::erase_if(keys, [](const auto& key) { return key.keyboard.expired(); });
            auto it = std::ranges::find_if(keys, [&](const auto& key) { return key.keyboard == keyboard && key.code == e.keycode; });
            if (!pressed && it != keys.end()) {
                const auto saved = *it;
                keys.erase(it);
                if (saved.reserved)
                    return false;
                const bool pass = original(self, event, keyboard);
                return pass && !saved.suppressed;
            }
            const auto sym        = keyboard->m_xkbSymState ? xkb_state_key_get_one_sym(keyboard->m_xkbSymState, e.keycode + 8) : XKB_KEY_NoSymbol;
            const auto mods       = g_pInputManager->getModsFromAllKBs();
            const bool navigation = owned && (reserved(sym, mods) || (sym == XKB_KEY_Escape && session().drag.active()));
            if (pressed)
                keys.push_back({keyboard, e.keycode, navigation, owned});
            if (navigation) {
                if (pressed) {
                    if (auto view = session().keyboardView()) {
                        view->onKey(sym, mods, true);
                        if (sym != XKB_KEY_Escape)
                            session().keyboard(*view);
                        if (view->closing()) {
                            for (const auto& other : session().views)
                                other->close(false);
                            session().stopInput();
                        }
                    }
                }
                return false;
            }
            // Exactly one call to the real matcher. Its device, submap,
            // repeat, long-press and release bookkeeping stays authoritative.
            const bool pass = original(self, event, keyboard);
            return pass && !owned;
        }

        Vector2D mouseCoords(CInputManager* self) {
            using Fn = Vector2D (*)(CInputManager*);
            if (desktopPoint)
                return *desktopPoint;
            return reinterpret_cast<Fn>(coordsHook->m_original)(self);
        }

        void warp(const Pointer::CPointerController* self, const Vector2D& point, bool force) {
            using Fn = void (*)(const Pointer::CPointerController*, const Vector2D&, bool);
            if (desktopPoint || dispatchDepth > 0)
                return;
            reinterpret_cast<Fn>(warpHook->m_original)(self, point, force);
        }

        CFunctionHook* hook(const std::string& name, const std::string& signature, void* callback) {
            const auto matches = HyprlandAPI::findFunctionsByName(PHANDLE, name);
            const auto match   = std::ranges::find_if(matches, [&](const auto& candidate) {
                return candidate.demangled.starts_with(signature) || candidate.demangled.starts_with(signature.substr(0, signature.size() - 1) + "[abi:cxx11](");
            });
            if (match == matches.end())
                throw std::runtime_error("[hyprspace] missing compositor hook: " + signature);
            auto result = HyprlandAPI::createFunctionHook(PHANDLE, match->address, callback);
            if (!result || !result->hook())
                throw std::runtime_error("[hyprspace] failed compositor hook: " + signature);
            return result;
        }
    } // namespace

    void atDesktopPoint(const Vector2D& point, const std::function<void()>& action) {
        const auto previous = desktopPoint;
        desktopPoint        = point;
        try {
            action();
        } catch (...) {
            desktopPoint = previous;
            throw;
        }
        desktopPoint = previous;
    }

    void ownCursor(bool own) {
        using namespace Pointer::Cursor;
        auto& overrides = overrideController.get()->*member(SCursorOverrides{});
        if (own && !savedCursor) {
            savedCursor = {overrides[CURSOR_OVERRIDE_UNKNOWN], overrides[CURSOR_OVERRIDE_WINDOW_EDGE]};
            overrideController->unsetOverride(CURSOR_OVERRIDE_WINDOW_EDGE);
            overrideController->setOverride("default", CURSOR_OVERRIDE_UNKNOWN);
        } else if (!own && savedCursor) {
            if (overrides[CURSOR_OVERRIDE_UNKNOWN] == "default")
                overrideController->setOverride((*savedCursor)[0], CURSOR_OVERRIDE_UNKNOWN);
            if (overrides[CURSOR_OVERRIDE_WINDOW_EDGE].empty())
                overrideController->setOverride((*savedCursor)[1], CURSOR_OVERRIDE_WINDOW_EDGE);
            savedCursor.reset();
        }
    }

    bool keyboardOwned() {
        return ownsKeyboard && ownsKeyboard();
    }

    void renderPanels(PHLMONITOR monitor) {
        for (auto level : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM})
            for (const auto& layer : monitor->m_layerSurfaceLayers[level])
                if (layer && layer->m_namespace.starts_with("waybar"))
                    (g_pHyprRenderer.get()->*member(SRenderLayer{}))(layer.lock(), monitor, Time::steadyNow(), false, false);
    }

    CFunctionHook* attach(const std::string& name, const std::string& signature, void* callback) {
        return hook(name, signature, callback);
    }

    void cancelPlacement() {
        if (resizeTimer)
            g_pEventLoopManager->removeTimer(resizeTimer);
        resizeTimer.reset();
        if (auto target = resizeTarget.lock(); target && g_layoutManager->dragController()->target() == target)
            atDesktopPoint(resizePickup, [] { g_layoutManager->endDragTarget(); });
        resizeTarget.reset();
    }

    bool place(PHLWINDOW window, const SOverviewTarget& source, const SOverviewTarget& destination, bool resize) {
        if (!window || !window->m_isMapped || !window->m_workspace || g_layoutManager->dragController()->target())
            return false;
        const auto ws       = session().workspace(destination);
        const auto sourceWs = window->m_workspace;
        if (!ws || !ws->m_monitor || !sourceWs->m_monitor)
            return false;
        // Make native hit testing see the chosen workspace on each output.
        // No window or layout mutation has happened before this valid drop.
        const auto mon    = ws->m_monitor.lock();
        const auto pickup = session().desktopPoint(source);
        const auto drop   = session().desktopPoint(destination);
        atDesktopPoint(drop, [&] {
            if (ws->m_isSpecialWorkspace)
                mon->setSpecialWorkspace(ws);
            else {
                mon->setSpecialWorkspace(nullptr);
                mon->changeWorkspace(ws);
            }
            Desktop::focusState()->rawMonitorFocus(mon);
        });
        const auto target = window->layoutTarget();
        atDesktopPoint(pickup, [&] {
            g_layoutManager->beginDragTarget(target, resize ? MBIND_RESIZE : MBIND_MOVE);
            if (!g_layoutManager->dragController()->target())
                return;
            // Cross the configured threshold at pickup, then replay the final
            // motion. updateDragWindow reads the scoped pickup coordinate.
            const auto threshold = CConfigValue<Config::INTEGER>("binds:drag_threshold");
            if (*threshold > 0)
                g_layoutManager->moveMouse(pickup + Vector2D{static_cast<double>(*threshold) + 1, 0.0});
        });
        if (!g_layoutManager->dragController()->target())
            return false;
        if (resize) {
            // Native resizing coalesces motion to the output's refresh period.
            // Prime that clock without changing geometry, then flush the final
            // point after one frame. Never block the compositor with a sleep.
            atDesktopPoint(pickup, [&] { g_layoutManager->moveMouse(pickup); });
            resizeTarget  = target;
            resizePickup  = pickup;
            const auto hz = g_pHyprRenderer->m_mostHzMonitor ? g_pHyprRenderer->m_mostHzMonitor->m_refreshRate : 60.0;
            resizeTimer   = makeShared<CEventLoopTimer>(
                std::chrono::milliseconds(static_cast<int>(1000 / std::max(1.0, static_cast<double>(hz))) + 2),
                [point = drop](SP<CEventLoopTimer>, void*) {
                    if (auto target = resizeTarget.lock(); target && g_layoutManager->dragController()->target() == target)
                        atDesktopPoint(point, [&] {
                            g_layoutManager->moveMouse(point);
                            g_layoutManager->endDragTarget();
                        });
                    resizeTarget.reset();
                    cancelPlacement();
                    session().damage();
                },
                nullptr);
            g_pEventLoopManager->addTimer(resizeTimer);
            return true;
        }
        atDesktopPoint(drop, [&] {
            g_layoutManager->moveMouse(drop);
            // A floating window's centre may still be on the source output.
            // Its drop destination is the pointer's workspace, not that centre.
            if (!resize && target->space() != ws->m_space)
                target->assignToSpace(ws->m_space, drop);
            g_layoutManager->endDragTarget();
        });
        session().followKeyboardFocus();
        return true;
    }

    void install(std::function<bool()> owner, std::function<bool()> launching) {
        ownsKeyboard  = std::move(owner);
        launchEnabled = std::move(launching);
        try {
            keyHook    = hook("onKeyEvent", "CKeybindManager::onKeyEvent(", reinterpret_cast<void*>(onKey));
            inputHook  = hook("onKeyboardKey", "CInputManager::onKeyboardKey(", reinterpret_cast<void*>(keyboardInput));
            focusHook  = hook("setKeyboardFocus", "CSeatManager::setKeyboardFocus(", reinterpret_cast<void*>(keyboardFocus));
            coordsHook = hook("getMouseCoordsInternal", "CInputManager::getMouseCoordsInternal(", reinterpret_cast<void*>(mouseCoords));
            warpHook   = hook("warpTo", "Pointer::CPointerController::warpTo(", reinterpret_cast<void*>(warp));
            for (auto& [name, dispatcher] : g_pKeybindManager->m_dispatchers) {
                if (name.starts_with("hyprspace:") || name == "movecursor")
                    continue;
                dispatchers.emplace(name, dispatcher);
                dispatcher = [name, original = dispatcher](std::string args) {
                    if (dispatchDepth)
                        return original(std::move(args));
                    const bool owned = keyboardOwned();
                    if (!owned) {
                        if (!launchEnabled || !launchEnabled())
                            return original(std::move(args));
                        if (name == "exec" || name == "execr") {
                            SDispatchResult result;
                            launch::duringCommand(session().selection.command(), [&] { result = original(std::move(args)); });
                            return result;
                        }
                    }
                    const auto      point = session().selection.command();
                    PHLWINDOW       before;
                    PHLWORKSPACE    beforeWorkspace;
                    SDispatchResult result;
                    ++dispatchDepth;
                    try {
                        CKeepLayerKeyboard keepLayer(!owned);
                        launch::duringCommand(point, [&] {
                            const auto execute = [&] {
                                session().establishTarget();
                                before          = Desktop::focusState()->window();
                                const auto mon  = Desktop::focusState()->monitor();
                                beforeWorkspace = mon ? (mon->m_activeSpecialWorkspace ? mon->m_activeSpecialWorkspace : mon->m_activeWorkspace) : nullptr;
                                result          = original(std::move(args));
                            };
                            if (point)
                                atDesktopPoint(session().desktopPoint(*point), execute);
                            else
                                execute();
                        });
                    } catch (...) {
                        --dispatchDepth;
                        throw;
                    }
                    --dispatchDepth;
                    const auto mon            = Desktop::focusState()->monitor();
                    const auto afterWorkspace = mon ? (mon->m_activeSpecialWorkspace ? mon->m_activeSpecialWorkspace : mon->m_activeWorkspace) : nullptr;
                    if (Desktop::focusState()->window() != before || beforeWorkspace != afterWorkspace)
                        session().followKeyboardFocus();
                    return result;
                };
            }
        } catch (...) {
            uninstall();
            throw;
        }
    }

    void uninstall() {
        cancelPlacement();
        ownCursor(false);
        for (auto& [name, dispatcher] : dispatchers)
            g_pKeybindManager->m_dispatchers[name] = std::move(dispatcher);
        dispatchers.clear();
        for (auto handle : {keyHook, inputHook, focusHook, coordsHook, warpHook})
            if (handle)
                HyprlandAPI::removeFunctionHook(PHANDLE, handle);
        keyHook = inputHook = focusHook = coordsHook = warpHook = nullptr;
        desktopPoint.reset();
        ownsKeyboard  = {};
        launchEnabled = {};
        keys.clear();
        ignoredKeys.clear();
        layerKeyboard.reset();
    }
} // namespace hyprspace::hooks
