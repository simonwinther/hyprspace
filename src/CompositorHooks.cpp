#include "CompositorHooks.hpp"

#include "Config.hpp"
#include "Overview.hpp"
#include "Launch.hpp"
#include "OverlayPolicy.hpp"
#include "Diagnostics.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/protocols/InputMethodV2.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <any>
#include <dlfcn.h>
#include <stdexcept>
#include <type_traits>

namespace hyprspace::hooks {
    namespace {
        CFunctionHook *                 keyHook = nullptr, *inputHook = nullptr, *focusHook = nullptr, *coordsHook = nullptr, *warpHook = nullptr;
        CFunctionHook *                 pointerHook = nullptr, *imeModsHook = nullptr, *axisHook = nullptr, *mouseBindHook = nullptr;
        CFunctionHook*                  layoutPointerHook   = nullptr;
        CFunctionHook*                  cursorRenderingHook = nullptr;
        double                          axisScale           = 1.0;
        bool                            keyRelease          = false;
        std::optional<Vector2D>         desktopPoint;
        std::function<bool()>           ownsKeyboard;
        std::function<bool()>           launchEnabled;
        std::function<bool(PHLMONITOR)> promotePanels;

        using Dispatcher = decltype(CKeybindManager::m_dispatchers)::mapped_type;
        struct SDispatcherRegistration {
            Dispatcher original;
            uint64_t   id;
        };
        struct SDispatcherWrapper {
            Dispatcher      callback;
            uint64_t        registration;
            SDispatchResult operator()(std::string args) const {
                return callback(std::move(args));
            }
        };
        std::unordered_map<std::string, SDispatcherRegistration> dispatchers;
        uint64_t                                                 nextRegistration = 0;
        int                                                      dispatchDepth    = 0;

        bool compositorDispatcher(const std::string& name, const Dispatcher& dispatcher) {
            // Registration names also cover the legacy API, whose trampoline
            // lives in Hyprland but captures a plugin-owned callable.
            for (const auto& plugin : g_pPluginSystem->getAllPlugins())
                if (std::ranges::contains(plugin->m_registeredDispatchers, name))
                    return false;

            // Unregistered foreign replacements are not safe to retain either.
            // Native closures in the supported compositor have executable-owned
            // RTTI; a plain function pointer needs its code owner checked too.
            const void* owner = &dispatcher.target_type();
            if (const auto function = dispatcher.target<SDispatchResult (*)(std::string)>())
                owner = reinterpret_cast<const void*>(*function);
            Dl_info executable{}, provider{};
            return dladdr(reinterpret_cast<const void*>(__hyprland_api_get_hash), &executable) && dladdr(owner, &provider) && executable.dli_fbase == provider.dli_fbase;
        }

        enum class EInspectionKey { NONE, IN, OUT, GRAB, LEFT, RIGHT, UP, DOWN };

        struct SKey {
            WP<IKeyboard>           keyboard;
            uint32_t                code;
            bool                    reserved;
            bool                    suppressed;
            bool                    zoom = false;
            std::optional<uint64_t> zoomToken;
            bool                    emptyWorkspace       = false;
            EInspectionKey          inspection           = EInspectionKey::NONE;
            uint64_t                inspectionGeneration = 0;
            std::optional<uint64_t> panToken;
        };
        std::vector<SKey>   keys;
        std::vector<SKey>   ignoredKeys;
        SP<CEventLoopTimer> inspectionRepeatTimer;
        WP<IKeyboard>       inspectionRepeatKeyboard;
        uint32_t            inspectionRepeatCode = 0;

        void stopInspectionRepeat() {
            if (inspectionRepeatTimer)
                inspectionRepeatTimer->updateTimeout(std::nullopt);
            inspectionRepeatKeyboard.reset();
        }

        void inspectKey(EInspectionKey action) {
            switch (action) {
            case EInspectionKey::IN:
                session().zoomStep(1);
                break;
            case EInspectionKey::OUT:
                session().zoomStep(-1);
                break;
            case EInspectionKey::LEFT:
                session().panKey({40, 0});
                break;
            case EInspectionKey::RIGHT:
                session().panKey({-40, 0});
                break;
            case EInspectionKey::UP:
                session().panKey({0, 40});
                break;
            case EInspectionKey::DOWN:
                session().panKey({0, -40});
                break;
            default:
                break;
            }
        }

        EInspectionKey inspectionKey(xkb_keysym_t sym, uint32_t mods) {
            if (mods == HL_MODIFIER_SHIFT) {
                switch (sym) {
                case XKB_KEY_plus:
                case XKB_KEY_equal:
                    return EInspectionKey::IN;
                case XKB_KEY_Left:
                    return EInspectionKey::LEFT;
                case XKB_KEY_Right:
                    return EInspectionKey::RIGHT;
                case XKB_KEY_Up:
                    return EInspectionKey::UP;
                case XKB_KEY_Down:
                    return EInspectionKey::DOWN;
                default:
                    return EInspectionKey::NONE;
                }
            }
            if (mods != 0)
                return EInspectionKey::NONE;
            switch (sym) {
            case XKB_KEY_plus:
            case XKB_KEY_equal:
            case XKB_KEY_KP_Add:
                return EInspectionKey::IN;
            case XKB_KEY_minus:
            case XKB_KEY_KP_Subtract:
                return EInspectionKey::OUT;
            case XKB_KEY_space:
                return EInspectionKey::GRAB;
            default:
                return EInspectionKey::NONE;
            }
        }

        void startInspectionRepeat(SP<IKeyboard> keyboard, uint32_t code) {
            if (keyboard->m_repeatRate <= 0)
                return;
            if (!inspectionRepeatTimer) {
                inspectionRepeatTimer = makeShared<CEventLoopTimer>(
                    std::nullopt,
                    [](SP<CEventLoopTimer> self, void*) {
                        const auto keyboard = inspectionRepeatKeyboard.lock();
                        const auto key = std::ranges::find_if(keys, [&](const auto& entry) { return entry.keyboard == keyboard && entry.code == inspectionRepeatCode; });
                        if (!keyboard || keyboard->m_repeatRate <= 0 || key == keys.end() || !keyboardOwned() || !session().cursorOwned() ||
                            !config::overviewWheelZoom() || key->inspectionGeneration != session().zoomGeneration()) {
                            stopInspectionRepeat();
                            return;
                        }
                        inspectKey(key->inspection);
                        self->updateTimeout(std::chrono::milliseconds(std::max(1, 1000 / keyboard->m_repeatRate)));
                    },
                    nullptr);
                g_pEventLoopManager->addTimer(inspectionRepeatTimer);
            }
            inspectionRepeatKeyboard = keyboard;
            inspectionRepeatCode     = code;
            inspectionRepeatTimer->updateTimeout(std::chrono::milliseconds(std::max(1, keyboard->m_repeatDelay)));
        }

        void pruneKeys() {
            std::erase_if(keys, [](const auto& key) {
                if (!key.keyboard.expired())
                    return false;
                if (key.zoomToken && g_overviewSession)
                    session().zoomRelease(*key.zoomToken);
                if (key.panToken && g_overviewSession)
                    session().panRelease(*key.panToken);
                return true;
            });
            std::erase_if(ignoredKeys, [](const auto& key) { return key.keyboard.expired(); });
        }
        template <typename Tag, auto Member> struct CAccess {
            friend auto member(Tag) {
                return Member;
            }
        };
        struct SCursorOverrides {
            friend auto member(SCursorOverrides);
        };
        template struct CAccess<SCursorOverrides, &Pointer::Cursor::CShapeOverrideController::m_overrides>;
        struct SCursorHiddenConditions {
            friend auto member(SCursorHiddenConditions);
        };
        template struct CAccess<SCursorHiddenConditions, &Render::IHyprRenderer::m_cursorHiddenConditions>;
        struct SPointerListeners {
            friend auto member(SPointerListeners);
        };
        template struct CAccess<SPointerListeners, &Pointer::CPointerManager::m_pointerListeners>;
        struct SIgnoreKeyboard {
            friend auto member(SIgnoreKeyboard);
        };
        template struct CAccess<SIgnoreKeyboard, &CInputManager::shouldIgnoreVirtualKeyboard>;
        struct SRenderLayer {
            friend auto member(SRenderLayer);
        };
        template struct CAccess<SRenderLayer, &Render::IHyprRenderer::renderLayer>;
        struct SResizeCorner {
            friend auto member(SResizeCorner);
        };
        template struct CAccess<SResizeCorner, &Layout::Supplementary::CDragStateController::m_grabbedCorner>;
        struct SResizePosition {
            friend auto member(SResizePosition);
        };
        template struct CAccess<SResizePosition, &Layout::Supplementary::CDragStateController::m_beginDragPositionXY>;
        struct SResizeSize {
            friend auto member(SResizeSize);
        };
        template struct CAccess<SResizeSize, &Layout::Supplementary::CDragStateController::m_beginDragSizeXY>;
        struct SDragThreshold {
            friend auto member(SDragThreshold);
        };
        template struct CAccess<SDragThreshold, &Layout::Supplementary::CDragStateController::m_dragThresholdReached>;
        std::optional<std::array<std::string, 2>> savedCursor;
        std::string                               installedCursor;
        SP<CEventLoopTimer>                       resizeTimer;
        WP<Layout::ITarget>                       resizeTarget;
        Vector2D                                  resizePickup;
        WP<CWLSurfaceResource>                    layerKeyboard;
        bool                                      keyboardSuspended = false;

        bool windowSurface(const SP<CWLSurfaceResource>& surface) {
            const auto owner = Desktop::View::CWLSurface::fromResource(surface);
            return owner && Desktop::View::CWindow::fromView(owner->view());
        }

        bool mappedKeyboardLayer() {
            const auto owner = Desktop::View::CWLSurface::fromResource(g_pSeatManager->m_state.keyboardFocus.lock());
            const auto layer = owner ? Desktop::View::CLayerSurface::fromView(owner->view()) : nullptr;
            return layer && layer->m_mapped;
        }

        // The renderer promotes these panels for the overview. Give the native
        // pointer route the same order, including its fullscreen and popup
        // handling. Restore every compositor field before returning.
        class CPromotedPanels {
          public:
            explicit CPromotedPanels(PHLMONITOR monitor) : monitor(monitor) {
                if (!monitor || desktopPoint || dispatchDepth || !promotePanels || !promotePanels(monitor))
                    return;
                std::vector<PHLLSREF> promoted;
                for (auto level : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
                    auto& layers = monitor->m_layerSurfaceLayers[level];
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto layer = layers[i].lock();
                        if (!layer || !layer->m_mapped || !layer->m_namespace.starts_with("waybar"))
                            continue;
                        saved.push_back({layer, level, i, layer->m_aboveFullscreen});
                        promoted.emplace_back(layer);
                        layer->m_layer           = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
                        layer->m_aboveFullscreen = true;
                    }
                    std::erase_if(layers, [&](const auto& layer) { return std::ranges::contains(promoted, layer); });
                }
                auto& top = monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP];
                top.insert(top.begin(), promoted.begin(), promoted.end());
            }
            ~CPromotedPanels() {
                if (!monitor)
                    return;
                auto& top = monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP];
                for (const auto& entry : saved) {
                    std::erase(top, PHLLSREF{entry.layer});
                    entry.layer->m_layer           = entry.level;
                    entry.layer->m_aboveFullscreen = entry.aboveFullscreen;
                    auto& layers                   = monitor->m_layerSurfaceLayers[entry.level];
                    if (entry.layer->m_mapped && entry.layer->m_monitor == monitor && !std::ranges::contains(layers, entry.layer))
                        layers.insert(layers.begin() + std::min(entry.index, layers.size()), entry.layer);
                }
            }

          private:
            struct SSaved {
                PHLLS    layer;
                uint32_t level;
                size_t   index;
                bool     aboveFullscreen;
            };
            PHLMONITOR          monitor;
            std::vector<SSaved> saved;
        };

        void cursorRenderingMode(Render::IHyprRenderer* self) {
            using Fn               = void (*)(Render::IHyprRenderer*);
            const auto  original   = reinterpret_cast<Fn>(cursorRenderingHook->m_original);
            auto&       conditions = self->*member(SCursorHiddenConditions{});
            static auto hideOnKey  = CConfigValue<Config::INTEGER>("cursor:hide_on_key_press");
            if (!savedCursor || !overlaysAllowed() || !conditions.hiddenOnKeyboard || !*hideOnKey) {
                original(self);
                return;
            }
            const bool                          keyboardHidden = conditions.hiddenOnKeyboard;
            const Hyprutils::Utils::CScopeGuard restore([&] { conditions.hiddenOnKeyboard = keyboardHidden; });
            // Holding a picker key must not hide its pointer. Keep the native
            // condition for foreground handoff and preserve all other hiding
            // reasons, including invisible cursors, touch and input capture.
            conditions.hiddenOnKeyboard = false;
            original(self);
        }

        void pointerMove(CInputManager* self, uint32_t time, bool refocus, bool mouse, std::optional<Vector2D> overridePos) {
            using Fn = void (*)(CInputManager*, uint32_t, bool, bool, std::optional<Vector2D>);
            CPromotedPanels panels(State::monitorState()->query().vec(overridePos.value_or(self->getMouseCoordsInternal())).run());
            reinterpret_cast<Fn>(pointerHook->m_original)(self, time, refocus, mouse, overridePos);
        }

        void pointerAxis(CInputManager* self, IPointer::SAxisEvent event, SP<IPointer> pointer) {
            using Fn              = void (*)(CInputManager*, IPointer::SAxisEvent, SP<IPointer>);
            const double previous = axisScale;
            const double touchpad = *CConfigValue<Config::FLOAT>("input:touchpad:scroll_factor");
            axisScale             = touchpad <= 0 || event.source == WL_POINTER_AXIS_SOURCE_FINGER ? touchpad : *CConfigValue<Config::FLOAT>("input:scroll_factor");
            if (pointer && pointer->m_scrollFactor)
                axisScale = *pointer->m_scrollFactor;
            // The seat's last moving mouse can be a different device from the
            // wheel/touchpad producing this event. Use the native source.
            try {
                reinterpret_cast<Fn>(axisHook->m_original)(self, event, pointer);
            } catch (...) {
                axisScale = previous;
                throw;
            }
            axisScale = previous;
        }

        void imeModifiers(CInputMethodKeyboardGrabV2* self, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
            using Fn = void (*)(CInputMethodKeyboardGrabV2*, uint32_t, uint32_t, uint32_t, uint32_t);
            if (!keyboardOwned())
                reinterpret_cast<Fn>(imeModsHook->m_original)(self, depressed, latched, locked, group);
        }

        bool ensureMouseBindState(CKeybindManager* self) {
            using Fn = bool (*)(CKeybindManager*);
            // The mouse release already committed this resize. A subsequent
            // modifier release must not end its native drag before the final
            // coalesced motion runs on the next frame. Keep native key-release
            // matching, while other key presses retain native cancellation.
            if (keyRelease && resizeTimer && resizeTarget && g_layoutManager->dragController()->target() == resizeTarget)
                return false;
            return reinterpret_cast<Fn>(mouseBindHook->m_original)(self);
        }

        void layoutPointerMove(Layout::CLayoutManager* self, const Vector2D& point) {
            using Fn = void (*)(Layout::CLayoutManager*, const Vector2D&);
            // Released gestures resume normal panel hover. Only the mapped
            // final commit may move their still-pending native resize target.
            if (!desktopPoint && resizeTimer && resizeTarget && self->dragController()->target() == resizeTarget)
                return;
            reinterpret_cast<Fn>(layoutPointerHook->m_original)(self, point);
        }

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
            if (launchEnabled && launchEnabled() && windowSurface(surface)) {
                keyboardSuspended = true;
                if (!mappedKeyboardLayer())
                    reinterpret_cast<Fn>(focusHook->m_original)(self, nullptr);
                return;
            }
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
            case XKB_KEY_Page_Up:
            case XKB_KEY_Page_Down:
                return true;
            default:
                return false;
            }
        }

        void keyboardInput(CInputManager* self, const IKeyboard::SKeyEvent& event, SP<IKeyboard> keyboard) {
            using Fn = void (*)(CInputManager*, const IKeyboard::SKeyEvent&, SP<IKeyboard>);
            syncKeyboardFocus();
            const auto key = std::ranges::find_if(ignoredKeys, [&](const auto& saved) { return saved.keyboard == keyboard && saved.code == event.keycode; });
            if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED && key != ignoredKeys.end()) {
                ignoredKeys.erase(key);
                return;
            }
            // IME-originated virtual keyboards intentionally bypass the native
            // binding matcher. They must not bypass overview input ownership.
            if (ownsKeyboard && ownsKeyboard() && keyboard->isVirtual() && (self->*member(SIgnoreKeyboard{}))(keyboard)) {
                if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED && key == ignoredKeys.end())
                    ignoredKeys.push_back({keyboard, event.keycode, false, true, false, std::nullopt});
                return;
            }
            reinterpret_cast<Fn>(inputHook->m_original)(self, event, keyboard);
        }

        bool onKey(CKeybindManager* self, std::any event, SP<IKeyboard> keyboard) {
            using Fn                                     = bool (*)(CKeybindManager*, std::any, SP<IKeyboard>);
            const auto                          original = reinterpret_cast<Fn>(keyHook->m_original);
            const auto                          e        = std::any_cast<IKeyboard::SKeyEvent>(event);
            const bool                          pressed  = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
            const bool                          previous = std::exchange(keyRelease, !pressed);
            const Hyprutils::Utils::CScopeGuard restore([previous] { keyRelease = previous; });
            pruneKeys();
            const bool owned = ownsKeyboard && ownsKeyboard();
            if (owned && pressed && diagnostics::enabled)
                if (const auto* view = session().keyboardView())
                    if (const auto monitor = view->monitor())
                        diagnostics::input(monitor->m_id);
            auto it = std::ranges::find_if(keys, [&](const auto& key) { return key.keyboard == keyboard && key.code == e.keycode; });
            if (!pressed && it != keys.end()) {
                const auto saved = *it;
                keys.erase(it);
                if (saved.zoomToken)
                    session().zoomRelease(*saved.zoomToken);
                if (saved.panToken)
                    session().panRelease(*saved.panToken);
                if (inspectionRepeatKeyboard == keyboard && inspectionRepeatCode == e.keycode)
                    stopInspectionRepeat();
                if (saved.reserved)
                    return false;
                const bool pass = original(self, event, keyboard);
                return pass && !saved.suppressed;
            }
            const auto sym  = keyboard->m_xkbSymState ? xkb_state_key_get_one_sym(keyboard->m_xkbSymState, e.keycode + 8) : XKB_KEY_NoSymbol;
            const auto mods = g_pInputManager->getModsFromAllKBs();
            // Captured one-shot and zoom presses keep their route across repeats, config
            // reloads and close/reopen. An ignored first press stays ignored.
            if (pressed && it != keys.end() && (it->zoom || it->emptyWorkspace))
                return false;
            if (pressed && it != keys.end() && it->inspection != EInspectionKey::NONE) {
                if (owned && session().cursorOwned() && it->inspectionGeneration == session().zoomGeneration())
                    inspectKey(it->inspection);
                return false;
            }
            const bool navigation = it != keys.end() ? it->reserved : owned && (reserved(sym, mods) || (sym == XKB_KEY_Escape && session().drag.active()));
            if (pressed && it == keys.end()) {
                const auto zoomKey    = config::overviewZoomKey();
                const bool zoom       = owned && mods == 0 && zoomKey != XKB_KEY_NoSymbol && xkb_keysym_to_lower(sym) == zoomKey;
                const auto emptyKey   = config::overviewEmptyWorkspaceKey();
                const bool empty      = owned && mods == 0 && emptyKey != XKB_KEY_NoSymbol && xkb_keysym_to_lower(sym) == emptyKey;
                const auto inspection = owned && session().zoomHeld() && !zoom && !empty ? inspectionKey(sym, mods) : EInspectionKey::NONE;
                stopInspectionRepeat();
                keys.push_back({keyboard, e.keycode, navigation || zoom || empty || inspection != EInspectionKey::NONE, owned, zoom,
                                zoom ? session().zoomPress() : std::nullopt, empty, inspection, session().zoomGeneration()});
                it = std::prev(keys.end());
                if (zoom)
                    return false;
                if (empty) {
                    // This action selects the pointer's output, independent of
                    // keyboardView(). Ordinary navigation would select that
                    // previous view again after the action returned.
                    (void)session().emptyWorkspace();
                    return false;
                }
                if (inspection != EInspectionKey::NONE) {
                    if (inspection == EInspectionKey::GRAB)
                        it->panToken = session().panPress();
                    else {
                        inspectKey(inspection);
                        startInspectionRepeat(keyboard, e.keycode);
                    }
                    return false;
                }
            }
            if (navigation) {
                if (pressed && owned) {
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
            const bool suppressed = it != keys.end() && it->suppressed;
            const bool pass       = original(self, event, keyboard);
            return pass && !owned && !suppressed;
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
            if (!result || !result->hook()) {
                if (result)
                    HyprlandAPI::removeFunctionHook(PHANDLE, result);
                throw std::runtime_error("[hyprspace] failed compositor hook: " + signature);
            }
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

    bool mappingPointer() {
        return desktopPoint.has_value();
    }

    void ownCursor(bool own) {
        using namespace Pointer::Cursor;
        auto& overrides = overrideController.get()->*member(SCursorOverrides{});
        if (own && !savedCursor) {
            savedCursor = {overrides[CURSOR_OVERRIDE_UNKNOWN], overrides[CURSOR_OVERRIDE_WINDOW_EDGE]};
            overrideController->unsetOverride(CURSOR_OVERRIDE_WINDOW_EDGE);
            overrideController->setOverride("default", CURSOR_OVERRIDE_UNKNOWN);
            installedCursor = "default";
            g_pHyprRenderer->ensureCursorRenderingMode();
        } else if (!own && savedCursor) {
            if (overrides[CURSOR_OVERRIDE_UNKNOWN] == installedCursor)
                overrideController->setOverride((*savedCursor)[0], CURSOR_OVERRIDE_UNKNOWN);
            if (overrides[CURSOR_OVERRIDE_WINDOW_EDGE].empty())
                overrideController->setOverride((*savedCursor)[1], CURSOR_OVERRIDE_WINDOW_EDGE);
            savedCursor.reset();
            installedCursor.clear();
            g_pHyprRenderer->ensureCursorRenderingMode();
        }
    }

    void setCursor(const std::string& name) {
        using namespace Pointer::Cursor;
        if (!savedCursor || installedCursor == name)
            return;
        auto& overrides = overrideController.get()->*member(SCursorOverrides{});
        if (overrides[CURSOR_OVERRIDE_UNKNOWN] != installedCursor)
            return;
        overrideController->setOverride(name, CURSOR_OVERRIDE_UNKNOWN);
        installedCursor = name;
    }

    std::string cursorName() {
        using namespace Pointer::Cursor;
        return (overrideController.get()->*member(SCursorOverrides{}))[CURSOR_OVERRIDE_UNKNOWN];
    }

    std::function<bool()> pointerConnectionGuard() {
        const auto& listeners = Pointer::mgr().get()->*member(SPointerListeners{});
        using Listener        = std::remove_cvref_t<decltype(*listeners.front())>;
        std::vector<WP<Listener>> connections;
        connections.reserve(listeners.size());
        for (const auto& listener : listeners)
            if (listener && listener->pointer && listener->pointer->m_connected)
                connections.emplace_back(listener);

        // Native detach destroys the listener, even when the same device is
        // reattached before another event. Weak identities retain no device
        // and are sampled once per grip, without allocating during rendering.
        return [connections = std::move(connections)] {
            return !connections.empty() && std::ranges::all_of(connections, [](const auto& weak) {
                const auto listener = weak.lock();
                const auto pointer  = listener ? listener->pointer.lock() : nullptr;
                return pointer && pointer->m_connected;
            });
        };
    }

    std::string cursorShape() {
        const auto& overrides = Pointer::Cursor::overrideController.get()->*member(SCursorOverrides{});
        for (const auto& shape : overrides | std::views::reverse)
            if (!shape.empty())
                return shape;
        return {};
    }

    bool keyboardOwned() {
        return ownsKeyboard && ownsKeyboard();
    }

    namespace {
        std::expected<void, std::string> validateOverviewKey(const std::string& name, const std::string& setting) {
            if (name.empty())
                return {};
            const auto sym = xkb_keysym_to_lower(xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE));
            if (sym == XKB_KEY_NoSymbol)
                return std::unexpected(setting + " must be an XKB key name or empty to disable it");
            if ((sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) || sym == XKB_KEY_Mode_switch ||
                sym == XKB_KEY_Num_Lock || sym == XKB_KEY_Scroll_Lock)
                return std::unexpected(setting + " cannot be a modifier or lock key");
            if (reserved(sym, 0) || reserved(sym, HL_MODIFIER_SHIFT))
                return std::unexpected(setting + " cannot replace an overview navigation key");
            if (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req || (sym >= 0x10080000 && sym <= 0x1008FFFF))
                return std::unexpected(setting + " cannot replace a system key");
            return {};
        }
    } // namespace

    std::expected<void, std::string> validateZoomKey(const std::string& name) {
        return validateOverviewKey(name, "zoom_key");
    }

    std::expected<void, std::string> validateEmptyWorkspaceKey(const std::string& name) {
        return validateOverviewKey(name, "empty_workspace_key");
    }

    double scrollFactor() {
        return axisScale;
    }

    void syncKeyboardFocus() {
        pruneKeys();
        if (!keyboardOwned() && g_overviewSession)
            session().cancelZoom();
        if (!focusHook)
            return;
        using Fn            = void (*)(CSeatManager*, SP<CWLSurfaceResource>);
        const auto original = reinterpret_cast<Fn>(focusHook->m_original);
        if (keyboardOwned()) {
            if (windowSurface(g_pSeatManager->m_state.keyboardFocus.lock())) {
                keyboardSuspended = true;
                original(g_pSeatManager.get(), nullptr);
            }
            return;
        }
        // A foreground layer or grab keeps native ownership. Restore the
        // compositor's latest focus only after overview ownership ends.
        if (keyboardSuspended && (!launchEnabled || !launchEnabled())) {
            keyboardSuspended = false;
            if (!g_pSeatManager->m_state.keyboardFocus) {
                const auto surface = Desktop::focusState()->surface();
                const auto grab    = g_pSeatManager->m_seatGrab;
                if (surface && overlaysAllowed() && (!grab || !grab->m_keyboard || grab->accepts(surface)))
                    original(g_pSeatManager.get(), surface);
                if (auto keyboard = g_pSeatManager->m_keyboard.lock())
                    g_pInputManager->onKeyboardMod(keyboard);
            }
        }
    }

    void renderPanels(PHLMONITOR monitor) {
        for (auto level : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM})
            for (const auto& layer : monitor->m_layerSurfaceLayers[level])
                if (layer && layer->m_namespace.starts_with("waybar"))
                    (g_pHyprRenderer.get()->*member(SRenderLayer{}))(layer.lock(), monitor, Time::steadyNow(), false, false);
    }

    namespace {
        struct SScrolling {
            Layout::Tiled::CScrollingAlgorithm* algorithm = nullptr;
            SP<Layout::Tiled::SScrollingData>   data;
        };

        SScrolling scrolling(const SOverviewTarget& target) {
            const auto ws = session().workspace(target);
            if (!ws || !ws->m_space || !ws->m_space->algorithm())
                return {};
            const auto algorithm = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(ws->m_space->algorithm()->tiledAlgo().get());
            const auto column    = algorithm ? algorithm->getColumnAtViewportCenter() : nullptr;
            return {algorithm, column ? column->scrollingData.lock() : nullptr};
        }

        std::pair<double, double> scrollBounds(const SScrolling& scroll) {
            const auto   area            = scroll.algorithm->usableArea();
            const auto&  controller      = scroll.data->controller;
            const double size            = scroll.algorithm->primaryViewportSize();
            const bool   fullscreenOnOne = *CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");
            const double extent          = controller->calculateMaxExtent(area, fullscreenOnOne);
            double       first = 0, last = std::max(0.0, extent - size);
            if (*CConfigValue<Config::INTEGER>("scrolling:focus_fit_method") == 0 && !scroll.data->columns.empty()) {
                first        = std::min(first, -(size - controller->calculateStripSize(0, area, fullscreenOnOne)) / 2);
                const auto i = scroll.data->columns.size() - 1;
                last = std::max(last, controller->calculateStripStart(i, area, fullscreenOnOne) - (size - controller->calculateStripSize(i, area, fullscreenOnOne)) / 2);
            }
            return {first, last};
        }
    } // namespace

    std::optional<SScrollViewport> scrollingViewport(const SOverviewTarget& target) {
        const auto scroll = scrolling(target);
        if (!scroll.algorithm)
            return std::nullopt;
        if (!scroll.data)
            return SScrollViewport{};
        const auto&  controller      = scroll.data->controller;
        const bool   fullscreenOnOne = *CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");
        const double size            = scroll.algorithm->primaryViewportSize();
        const double extent          = controller->calculateMaxExtent(scroll.algorithm->usableArea(), fullscreenOnOne);
        const double offset          = controller->getOffset();
        const bool   before = offset > 0.5, after = extent - size - offset > 0.5;
        return SScrollViewport{controller->isPrimaryHorizontal(), controller->isReversed() ? after : before, controller->isReversed() ? before : after};
    }

    bool panWorkspace(const SOverviewTarget& target, double distance) {
        const auto scroll = scrolling(target);
        if (!scroll.algorithm || !scroll.data || session().drag.active() || !std::isfinite(distance))
            return false;
        const auto& controller = scroll.data->controller;
        if (controller->getScrollInhibitor().isInhibited)
            return false;
        const auto [first, last] = scrollBounds(scroll);
        const double before      = controller->getOffset();
        const double delta       = std::clamp(distance, -1.0, 1.0) * scroll.algorithm->primaryViewportSize() * (controller->isReversed() ? -1 : 1);
        const double after       = std::clamp(before + delta, first, last);
        // Native movement updates geometry, fullscreen visibility and damage.
        scroll.algorithm->moveTape(before - after);
        session().damage();
        return after != before;
    }

    bool stepWorkspace(const SOverviewTarget& target, int direction, bool fromSelection) {
        const auto scroll = scrolling(target);
        if (!scroll.algorithm || !scroll.data || scroll.data->columns.empty() || session().drag.active())
            return false;
        const auto& controller = scroll.data->controller;
        if (controller->getScrollInhibitor().isInhibited)
            return false;
        const int step   = (direction < 0 ? -1 : 1) * (controller->isReversed() ? -1 : 1);
        auto      column = scroll.algorithm->getColumnAtViewportCenter();
        if (fromSelection) {
            const auto w = target.window.lock();
            if (const auto data = w ? scroll.algorithm->dataFor(w->layoutTarget(), true) : nullptr)
                column = data->column.lock();
        }
        auto index = scroll.data->idx(column);
        if (index < 0)
            return false;
        if (fromSelection)
            index = std::clamp<int64_t>(index + step, 0, scroll.data->columns.size() - 1);
        else {
            const auto   area = scroll.algorithm->usableArea();
            const bool   full = *CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");
            const double size = scroll.algorithm->primaryViewportSize(), offset = controller->getOffset();
            double       nearest = std::numeric_limits<double>::max();
            for (size_t i = 0; i < scroll.data->columns.size(); ++i) {
                const double start  = controller->calculateStripStart(i, area, full) - offset;
                const double end    = start + controller->calculateStripSize(i, area, full);
                const double hidden = step > 0 ? end - size : -start;
                if (hidden > 0.5 && hidden < nearest) {
                    nearest = hidden;
                    index   = i;
                }
            }
        }
        column = scroll.data->columns[index];
        PHLWINDOW window;
        if (auto data = column->lastFocusedTarget.lock(); data && data->target)
            window = data->target->window();
        if (!window || !window->m_isMapped || window->isHidden())
            for (const auto& data : column->targetDatas)
                if (data->target && Desktop::View::validMapped(data->target->window())) {
                    window = data->target->window();
                    break;
                }
        if (!window)
            return false;
        CKeepLayerKeyboard keepLayer(!keyboardOwned());
        auto               selected = target;
        selected.window             = window;
        session().selection.keyboard(selected);
        atDesktopPoint(window->middle(), [&] {
            session().establishTarget();
            scroll.data->centerOrFitCol(column);
            scroll.data->recalculate();
        });
        session().followKeyboardFocus();
        return true;
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
        if (g_overviewSession)
            session().finishPlacement();
    }

    std::optional<SBoxF> resizeGeometry(PHLWINDOW window, const SBoxF& initial, SPoint delta, bool left, bool top) {
        const auto target = window ? window->layoutTarget() : nullptr;
        const auto ws     = target ? target->workspace() : nullptr;
        const auto mon    = ws ? ws->m_monitor.lock() : nullptr;
        if (!window || !window->m_isMapped || !mon || !mon->m_enabled)
            return std::nullopt;
        const auto work = mon->logicalBoxMinusReserved();
        SBoxF      bounds{work.x, work.y, work.w, work.h};
        if (!target->floating())
            return boundedResize(initial, delta, left, top, bounds);

        // Exclude panels, borders and reserved decorations, but not shadows.
        // Integer edges remain inside fractional logical monitor dimensions
        // after Hyprland rounds the resulting window geometry.
        const auto   extents = window->getFullWindowReservedArea();
        const auto   border  = window->getRealBorderSize();
        const double x       = std::ceil(bounds.x + std::max(extents.topLeft.x, static_cast<double>(border)));
        const double y       = std::ceil(bounds.y + std::max(extents.topLeft.y, static_cast<double>(border)));
        bounds               = {x, y, std::floor(work.x + work.w - std::max(extents.bottomRight.x, static_cast<double>(border))) - x,
                                std::floor(work.y + work.h - std::max(extents.bottomRight.y, static_cast<double>(border))) - y};
        const auto minimum   = target->minSize().value_or(Vector2D{MIN_WINDOW_SIZE, MIN_WINDOW_SIZE});
        const auto maximum   = target->maxSize().value_or(Vector2D{INFINITY, INFINITY});
        return boundedResize(initial, delta, left, top, bounds, {minimum.x, minimum.y}, {maximum.x, maximum.y},
                             window->m_ruleApplicator->keepAspectRatio().valueOrDefault());
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
            // The overview already verified real mapped motion crossed the
            // threshold. Run native pickup at its original point without a
            // fictitious movement that could resize the window itself.
            auto* controller = g_layoutManager->dragController().get();
            if (!controller->dragThresholdReached()) {
                controller->*member(SDragThreshold{}) = true;
                if (controller->updateDragWindow())
                    return;
            }
        });
        if (!g_layoutManager->dragController()->target())
            return false;
        if (resize) {
            // Native resizing coalesces motion to the output's refresh period.
            // Prime that clock without changing geometry, then flush the final
            // point after one frame. Never block the compositor with a sleep.
            atDesktopPoint(pickup, [&] { g_layoutManager->moveMouse(pickup); });
            resizeTarget          = target;
            resizePickup          = pickup;
            const auto hz         = g_pHyprRenderer->m_mostHzMonitor ? g_pHyprRenderer->m_mostHzMonitor->m_refreshRate : 60.0;
            const auto controller = g_layoutManager->dragController().get();
            const CBox initial{controller->*member(SResizePosition{}), controller->*member(SResizeSize{})};
            const auto corner = g_layoutManager->dragController().get()->*member(SResizeCorner{});
            resizeTimer       = makeShared<CEventLoopTimer>(
                std::chrono::milliseconds(static_cast<int>(1000 / std::max(1.0, static_cast<double>(hz))) + 2),
                [point = drop, pickup, initial, corner, workspace = PHLWORKSPACEREF{ws}](SP<CEventLoopTimer>, void*) {
                    const auto ws = workspace.lock();
                    if (auto target = resizeTarget.lock(); target && ws && target->workspace() == ws && g_layoutManager->dragController()->target() == target) {
                        auto        motion = point;
                        const bool  left = Layout::edgeLeft(corner), top = Layout::edgeTop(corner);
                        const SBoxF box{initial.x, initial.y, initial.w, initial.h};
                        auto        bounded =
                            target->floating() ? resizeGeometry(target->window(), box, {point.x - pickup.x, point.y - pickup.y}, left, top) : std::optional<SBoxF>{box};
                        if (bounded) {
                            if (target->floating())
                                motion = pickup + Vector2D{(bounded->w - box.w) * (left ? -1 : 1), (bounded->h - box.h) * (top ? -1 : 1)};
                            atDesktopPoint(motion, [&] {
                                // Native key-release matching resets this flag.
                                // Restore the real threshold crossing without
                                // reinitializing the original pickup geometry.
                                g_layoutManager->dragController().get()->*member(SDragThreshold{}) = true;
                                g_layoutManager->moveMouse(motion);
                                if (target->floating()) {
                                    // Resizing belongs to its source workspace even when
                                    // recovering a previously oversized floating window.
                                    if (target->space() != ws->m_space)
                                        target->assignToSpace(ws->m_space);
                                    const auto actual = target->position();
                                    if (const auto fitted = resizeGeometry(target->window(), box,
                                                                           {(actual.w - box.w) * (left ? -1 : 1), (actual.h - box.h) * (top ? -1 : 1)}, left, top)) {
                                        target->setPositionGlobal(CBox{fitted->x, fitted->y, fitted->w, fitted->h});
                                        target->warpPositionSize();
                                    }
                                }
                                g_layoutManager->endDragTarget();
                            });
                        }
                    }
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

    void reconcileDispatchers() {
        if (!keyHook)
            return;
        auto& currentDispatchers = g_pKeybindManager->m_dispatchers;
        // Lua reload replaces its dispatcher along with the interpreter state.
        // Only a matching wrapper may retain its saved native callback.
        std::erase_if(dispatchers, [&](const auto& entry) {
            const auto current = currentDispatchers.find(entry.first);
            if (current == currentDispatchers.end())
                return true;
            const auto wrapper = current->second.template target<SDispatcherWrapper>();
            return !wrapper || wrapper->registration != entry.second.id;
        });
        for (auto& [name, dispatcher] : currentDispatchers) {
            if (dispatchers.contains(name) || name.starts_with("hyprspace:") || name == "movecursor" || !compositorDispatcher(name, dispatcher))
                continue;
            const auto registration = ++nextRegistration;
            auto       callback     = [name, original = dispatcher](std::string args) {
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
            Dispatcher replacement = SDispatcherWrapper{std::move(callback), registration};
            dispatchers.emplace(name, SDispatcherRegistration{dispatcher, registration});
            dispatcher.swap(replacement);
        }
    }

    void install(std::function<bool()> owner, std::function<bool()> launching, std::function<bool(PHLMONITOR)> panels) {
        ownsKeyboard  = std::move(owner);
        launchEnabled = std::move(launching);
        promotePanels = std::move(panels);
        try {
            keyHook             = hook("onKeyEvent", "CKeybindManager::onKeyEvent(", reinterpret_cast<void*>(onKey));
            inputHook           = hook("onKeyboardKey", "CInputManager::onKeyboardKey(", reinterpret_cast<void*>(keyboardInput));
            focusHook           = hook("setKeyboardFocus", "CSeatManager::setKeyboardFocus(", reinterpret_cast<void*>(keyboardFocus));
            coordsHook          = hook("getMouseCoordsInternal", "CInputManager::getMouseCoordsInternal(", reinterpret_cast<void*>(mouseCoords));
            warpHook            = hook("warpTo", "Pointer::CPointerController::warpTo(", reinterpret_cast<void*>(warp));
            pointerHook         = hook("mouseMoveUnified", "CInputManager::mouseMoveUnified(", reinterpret_cast<void*>(pointerMove));
            axisHook            = hook("onMouseWheel", "CInputManager::onMouseWheel(", reinterpret_cast<void*>(pointerAxis));
            imeModsHook         = hook("sendMods", "CInputMethodKeyboardGrabV2::sendMods(", reinterpret_cast<void*>(imeModifiers));
            mouseBindHook       = hook("ensureMouseBindState", "CKeybindManager::ensureMouseBindState(", reinterpret_cast<void*>(ensureMouseBindState));
            layoutPointerHook   = hook("moveMouse", "Layout::CLayoutManager::moveMouse(", reinterpret_cast<void*>(layoutPointerMove));
            cursorRenderingHook = hook("ensureCursorRenderingMode", "Render::IHyprRenderer::ensureCursorRenderingMode(", reinterpret_cast<void*>(cursorRenderingMode));
            reconcileDispatchers();
        } catch (...) {
            uninstall();
            throw;
        }
    }

    void uninstall() {
        stopInspectionRepeat();
        if (inspectionRepeatTimer) {
            g_pEventLoopManager->removeTimer(inspectionRepeatTimer);
            inspectionRepeatTimer.reset();
        }
        cancelPlacement();
        ownCursor(false);
        ownsKeyboard  = {};
        launchEnabled = {};
        syncKeyboardFocus();
        for (auto& [name, registration] : dispatchers) {
            const auto current = g_pKeybindManager->m_dispatchers.find(name);
            if (current == g_pKeybindManager->m_dispatchers.end())
                continue;
            const auto wrapper = current->second.target<SDispatcherWrapper>();
            if (wrapper && wrapper->registration == registration.id)
                current->second = std::move(registration.original);
        }
        dispatchers.clear();
        for (auto handle :
             {keyHook, inputHook, focusHook, coordsHook, warpHook, pointerHook, imeModsHook, axisHook, mouseBindHook, layoutPointerHook, cursorRenderingHook})
            if (handle)
                HyprlandAPI::removeFunctionHook(PHANDLE, handle);
        keyHook = inputHook = focusHook = coordsHook = warpHook = nullptr;
        pointerHook = imeModsHook = axisHook = mouseBindHook = nullptr;
        layoutPointerHook                                    = nullptr;
        cursorRenderingHook                                  = nullptr;
        axisScale                                            = 1.0;
        keyRelease                                           = false;
        desktopPoint.reset();
        ownsKeyboard      = {};
        launchEnabled     = {};
        promotePanels     = {};
        keyboardSuspended = false;
        keys.clear();
        ignoredKeys.clear();
        layerKeyboard.reset();
    }
} // namespace hyprspace::hooks
