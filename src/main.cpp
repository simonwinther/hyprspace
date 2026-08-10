// hyprspace - a Hyprland window overview and Alt+Tab switcher.
//
// Two overlays, one plugin:
//   * hyprspace:overview  - full-screen live overview of all windows/workspaces
//   * hyprspace:switch    - GNOME-style Alt+Tab switcher
//
// There is deliberately no launcher, no search field and no text input anywhere.

#include "globals.hpp"

#include "Config.hpp"
#include "Overview.hpp"
#include "PassElements.hpp"
#include "Switcher.hpp"
#include "Texture.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <unordered_set>

using namespace hyprspace;

namespace {

    std::unique_ptr<COverview> g_overview;
    std::unique_ptr<CSwitcher> g_switcher;

    // Print Screen starts an external layer-shell UI (Omarchy uses a frozen
    // hyprpicker surface plus slurp). While that UI exists, it must sit above
    // hyprspace and receive the input that hyprspace normally owns.
    struct SExternalUiState {
        bool                      pending                = false;
        bool                      active                 = false;
        bool                      deferredSwitcherCommit = false;
        SP<CEventLoopTimer>       timeout;
        UP<SEventLoopDoLaterLock> layerCloseCheck;
    };

    SExternalUiState g_externalUi;

    // Keycodes whose press the overlay swallowed.
    //
    // A key's release must always travel the path its press took, and the
    // decision cannot be re-derived per event: the modifiers, and whether an
    // overlay is still up at all, both change between the two halves of one
    // keystroke. Getting it wrong in either direction breaks something.
    //
    // Pass the press, swallow the release, and the client keeps repeating a key
    // it never saw released. Swallow the press, pass the release — which is what
    // happens to the digit that dismisses the overview, since the overlay has
    // stopped owning input by the time the finger comes up — and Hyprland gets a
    // release for a key it never saw pressed. That desyncs its pressed-key
    // bookkeeping and the *next* keybind silently does not fire, which is why
    // Alt+Tab alternated between working and dead after every overview use.
    std::unordered_set<uint32_t> g_swallowedPresses;

    struct SListeners {
        CHyprSignalListener key;
        CHyprSignalListener mouseMove;
        CHyprSignalListener mouseButton;
        CHyprSignalListener mouseAxis;
        CHyprSignalListener renderPre;
        CHyprSignalListener renderStage;
        CHyprSignalListener monitorRemoved;
        CHyprSignalListener configReloaded;
        CHyprSignalListener layerOpened;
        CHyprSignalListener layerClosed;
    };

    SListeners g_listeners;

    // Something is on screen and has to be drawn.
    bool active() {
        return g_overview || g_switcher;
    }

    // Something on screen still wants the keyboard and pointer.
    //
    // Deliberately narrower than active(). An overlay that has been closed is
    // only an animation playing itself out — it has already committed, and it
    // must not keep swallowing input, or every keystroke in the couple of
    // hundred milliseconds after a close is lost: keybinds included, so the
    // next Alt+Tab or Super+A silently does nothing, and text typed in that
    // window never reaches the app.
    bool overviewLive() {
        return g_overview && !g_overview->closing();
    }

    bool switcherLive() {
        return g_switcher && !g_switcher->closing();
    }

    bool ownsInput() {
        return overviewLive() || switcherLive();
    }

    PHLMONITOR targetMonitor() {
        if (auto m = g_pCompositor->getMonitorFromCursor())
            return m;

        if (auto w = Desktop::focusState()->window(); w && w->m_monitor)
            return w->m_monitor.lock();

        return g_pCompositor->m_monitors.empty() ? nullptr : g_pCompositor->m_monitors.front();
    }

    // Translate a raw evdev keycode into a layout-independent keysym. The
    // "static" xkb state deliberately ignores active modifiers, so Alt+Shift+Tab
    // still reports Tab and navigation keys behave the same on every layout.
    xkb_keysym_t keysymFor(uint32_t keycode) {
        const auto KEEB = g_pSeatManager->m_keyboard;
        if (!KEEB)
            return XKB_KEY_NoSymbol;

        // m_xkbSymState tracks the layout group but not modifiers, so Alt+Shift+Tab
        // still resolves to Tab and hjkl stay hjkl on every layout.
        if (!KEEB->m_xkbSymState)
            return XKB_KEY_NoSymbol;

        return xkb_state_key_get_one_sym(KEEB->m_xkbSymState, keycode + 8);
    }

    uint32_t currentMods() {
        // Hyprland resolves binds against the union of every keyboard. Reading
        // only the seat's currently preferred keyboard disagrees as soon as a
        // USB or virtual keyboard holds the modifier and another device sends
        // the key, which can make us miss Alt while Hyprland still sees it.
        return g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
    }

    // Keys that belong to the system, not to whatever is on screen.
    //
    // Screenshot, volume, brightness, keyboard backlight, media transport and
    // the rest of the XF86 vendor block are global by nature: they mean the
    // same thing no matter what has focus, and an overlay swallowing them just
    // makes the machine feel broken while it is up. Letting Print through also
    // means the overview and the switcher can be screenshotted at all, which
    // is the only way to show anyone what they look like.
    bool isSystemKey(xkb_keysym_t sym) {
        if (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req)
            return true;

        // XFree86 vendor keysyms: every media and hardware key lives here.
        return sym >= 0x10080000 && sym <= 0x1008FFFF;
    }

    bool isScreenshotKey(xkb_keysym_t sym) {
        return sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req || sym == XKB_KEY_XF86SelectiveScreenshot;
    }

    // Alt has to stay held while the switcher is open, but that changes which
    // Hyprland binding Print resolves to (Omarchy uses Alt+Print for recording).
    // Invoke the user's ordinary, unmodified screenshot binding directly so
    // the switcher can be captured without hard-coding a distro command here.
    bool dispatchPlainScreenshotBinding() {
        if (!g_pKeybindManager)
            return false;

        const auto SUBMAP = g_pKeybindManager->getCurrentSubmap();

        for (const auto& binding : g_pKeybindManager->m_keybinds) {
            if (!binding || !binding->enabled || binding->modmask != 0 || binding->release || binding->longPress || binding->multiKey)
                continue;
            if (binding->submap != SUBMAP && !binding->submapUniversal)
                continue;

            const auto SYM = xkb_keysym_from_name(binding->key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
            if (!isScreenshotKey(SYM))
                continue;

            const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find(binding->handler);
            if (DISPATCHER == g_pKeybindManager->m_dispatchers.end())
                continue;

            return DISPATCHER->second(binding->arg).success;
        }

        return false;
    }

    // The modifier a key is itself, or 0 for an ordinary key.
    uint32_t modifierBitFor(xkb_keysym_t sym) {
        switch (sym) {
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R:
            return HL_MODIFIER_META;

        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
            return HL_MODIFIER_ALT;

        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
            return HL_MODIFIER_CTRL;

        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:
            return HL_MODIFIER_SHIFT;

        default:
            return 0;
        }
    }

    // Modifiers as they stand *including* the event being delivered.
    //
    // getModifiers() lags by exactly one event — this callback runs before the
    // device layer folds the key into the xkb state, so a Super press still
    // reads as "no Super" and a Super release still reads as "Super held".
    // Taking it at face value makes the two halves of a keystroke disagree:
    // the Super press gets swallowed while the Super release is passed
    // through, which is the same press/release asymmetry that strands keys.
    uint32_t modsWith(xkb_keysym_t sym, bool pressed) {
        const uint32_t MODS = currentMods();
        const uint32_t BIT  = modifierBitFor(sym);

        if (!BIT)
            return MODS;

        return pressed ? (MODS | BIT) : (MODS & ~BIT);
    }

    PHLMONITOR overlayMonitor() {
        if (switcherLive())
            return g_switcher->monitor();
        if (overviewLive())
            return g_overview->monitor();
        return nullptr;
    }

    bool isExternalUiLayer(const PHLLS& layer) {
        const auto MONITOR = overlayMonitor();
        if (!layer || !MONITOR || !layer->m_mapped || layer->m_layer != ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || layer->m_monitor.lock() != MONITOR)
            return false;

        // These are the namespaces used by Omarchy's freeze + selection pair.
        // The geometry fallback keeps the hand-off useful for other screenshot
        // tools without yielding to ordinary notifications.
        if (layer->m_namespace == "hyprpicker" || layer->m_namespace == "selection" || layer->m_namespace == "slurp")
            return true;

        return layer->m_geometry.w >= MONITOR->m_size.x * 0.8 && layer->m_geometry.h >= MONITOR->m_size.y * 0.8;
    }

    bool hasExternalUiLayer() {
        return std::ranges::any_of(g_pCompositor->m_layers, isExternalUiLayer);
    }

    void damageOverlayMonitor() {
        if (const auto MONITOR = overlayMonitor())
            g_pHyprRenderer->damageMonitor(MONITOR);
    }

    void finishExternalUi() {
        if (!g_externalUi.pending && !g_externalUi.active)
            return;

        const bool COMMIT_SWITCHER = g_externalUi.deferredSwitcherCommit && switcherLive() && !(currentMods() & HL_MODIFIER_ALT);

        g_externalUi.pending                = false;
        g_externalUi.active                 = false;
        g_externalUi.deferredSwitcherCommit = false;
        if (g_externalUi.timeout)
            g_externalUi.timeout->updateTimeout(std::nullopt);

        if (COMMIT_SWITCHER)
            g_switcher->close(true);

        damageOverlayMonitor();
    }

    void armExternalUi() {
        g_externalUi.pending = true;

        if (!g_externalUi.timeout) {
            g_externalUi.timeout = makeShared<CEventLoopTimer>(
                std::chrono::seconds(2),
                [](SP<CEventLoopTimer> self, void*) {
                    // A direct/fullscreen capture may not create a selector layer at
                    // all. Do not leave the overlay's input suspended in that case.
                    if (hasExternalUiLayer()) {
                        g_externalUi.pending = false;
                        g_externalUi.active  = true;
                        damageOverlayMonitor();
                    } else
                        finishExternalUi();

                    self->updateTimeout(std::nullopt);
                },
                nullptr);
            g_pEventLoopManager->addTimer(g_externalUi.timeout);
        } else
            g_externalUi.timeout->updateTimeout(std::chrono::seconds(2));
    }

    bool yieldingInput() {
        return g_externalUi.pending || g_externalUi.active;
    }

    void onLayerOpened(PHLLS layer) {
        if (!g_externalUi.pending && !g_externalUi.active)
            return;
        if (!isExternalUiLayer(layer))
            return;

        g_externalUi.pending = false;
        g_externalUi.active  = true;
        if (g_externalUi.timeout)
            g_externalUi.timeout->updateTimeout(std::nullopt);
        damageOverlayMonitor();
    }

    void onLayerClosed(PHLLS) {
        if (!g_externalUi.active)
            return;

        // Hyprland emits layer.closed from CLayerSurface::onUnmap immediately
        // before setting m_mapped=false. Checking synchronously still sees the
        // closing selector and can strand the input hand-off indefinitely. The
        // idle turn runs after onUnmap has completed.
        g_externalUi.layerCloseCheck = g_pEventLoopManager->doLaterLock([] {
            if (g_externalUi.active && !hasExternalUiLayer())
                finishExternalUi();
        });
    }

    void destroyOverview() {
        if (!g_overview)
            return;

        const auto MON = g_overview->monitor();
        g_overview.reset();

        if (MON)
            g_pHyprRenderer->damageMonitor(MON);
    }

    void destroySwitcher() {
        if (!g_switcher)
            return;

        const auto MON = g_switcher->monitor();
        g_switcher.reset();

        if (MON)
            g_pHyprRenderer->damageMonitor(MON);
    }

    // ------------------------------------------------------------- input ----

    void onKey(IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        const bool PRESSED = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;

        // A release always mirrors its own press for *cancellation*, whatever
        // the modifiers or the overlay say now. Whether the overlay is told
        // about it is a separate question: the switcher commits on the Alt
        // release, and that Alt press was passed through, so the two decisions
        // cannot share an answer.
        if (!PRESSED) {
            const xkb_keysym_t SYM  = keysymFor(event.keycode);
            const bool         MINE = g_swallowedPresses.erase(event.keycode) > 0;

            if (MINE)
                info.cancelled = true;

            if (yieldingInput()) {
                if (switcherLive() && (SYM == XKB_KEY_Alt_L || SYM == XKB_KEY_Alt_R || SYM == XKB_KEY_Meta_L || SYM == XKB_KEY_Meta_R))
                    g_externalUi.deferredSwitcherCommit = true;
                return;
            }

            if (switcherLive()) {
                g_switcher->onKey(SYM, modsWith(SYM, false), false);

                // Alt released -> commit, exactly like GNOME.
                if (SYM == XKB_KEY_Alt_L || SYM == XKB_KEY_Alt_R || SYM == XKB_KEY_Meta_L || SYM == XKB_KEY_Meta_R)
                    g_switcher->close(true);
            } else if (overviewLive() && MINE)
                g_overview->onKey(SYM, modsWith(SYM, false), false);

            return;
        }

        if (yieldingInput())
            return;

        if (!ownsInput())
            return;

        const xkb_keysym_t SYM  = keysymFor(event.keycode);
        const uint32_t     MODS = modsWith(SYM, PRESSED);

        // System keys are never normally the overlay's to eat. Print additionally
        // arms a short hand-off window for the selector layer its binding opens.
        if (isSystemKey(SYM)) {
            if (isScreenshotKey(SYM)) {
                armExternalUi();

                // The switcher necessarily has Alt held, which would make
                // Hyprland run Alt+Print (screen recording on Omarchy). Consume
                // this event and run the configured plain Print binding instead.
                if (switcherLive() && (MODS & HL_MODIFIER_ALT)) {
                    info.cancelled = true;
                    g_swallowedPresses.insert(event.keycode);

                    const bool DISPATCHED = dispatchPlainScreenshotBinding();
                    if (!DISPATCHED) {
                        finishExternalUi();
                        HyprlandAPI::addNotification(PHANDLE, HS_LOG_PREFIX "no usable unmodified Print binding", CHyprColor{1.0, 0.35, 0.2, 1.0}, 4000);
                    }
                }
            }
            return;
        }

        // Super-modified keys keep reaching Hyprland's keybinds. That is what
        // makes the overview's own binding a toggle — the second Super+A has to
        // get through to the dispatcher to close it — and it leaves the rest of
        // your Super shortcuts working while the overview is up. The switcher is
        // exempt: it is driven by Alt and lives for a fraction of a second.
        if (overviewLive() && !switcherLive() && (MODS & HL_MODIFIER_META))
            return;

        // Otherwise the overlay owns the keyboard entirely: nothing reaches
        // keybinds or clients. This event fires before both, and the device
        // layer folds the key into the xkb state either way, so cancelling here
        // cannot leave the compositor's own modifier tracking out of step.
        info.cancelled = true;
        g_swallowedPresses.insert(event.keycode);

        if (switcherLive()) {
            g_switcher->onKey(SYM, MODS, true);
            return;
        }

        if (overviewLive())
            g_overview->onKey(SYM, MODS, true);
    }

    void onMouseMove(Vector2D pos, Event::SCallbackInfo& info) {
        if (yieldingInput() || !ownsInput())
            return;

        info.cancelled = true;

        if (switcherLive())
            g_switcher->onMouseMove(pos);
        else if (overviewLive())
            g_overview->onMouseMove(pos);
    }

    // Scroll belongs to the overlay while one is up. Without this the wheel
    // falls through to whatever sits underneath, so scrolling over the switcher
    // silently scrolls the page behind it.
    void onMouseAxis(IPointer::SAxisEvent event, Event::SCallbackInfo& info) {
        if (yieldingInput() || !ownsInput())
            return;

        info.cancelled = true;

        if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;

        // One notch per event, not a pixel delta: a high-resolution wheel
        // reports many small deltas and would race through the list.
        const double DELTA = event.deltaDiscrete != 0 ? static_cast<double>(event.deltaDiscrete) : event.delta;

        if (switcherLive())
            g_switcher->onScroll(DELTA);
        else if (overviewLive())
            g_overview->onScroll(DELTA);
    }

    void onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (yieldingInput() || !ownsInput())
            return;

        info.cancelled     = true;
        const bool PRESSED = event.state == WL_POINTER_BUTTON_STATE_PRESSED;

        if (switcherLive())
            g_switcher->onMouseButton(event.button, PRESSED);
        else if (overviewLive())
            g_overview->onMouseButton(event.button, PRESSED, currentMods());
    }

    // ------------------------------------------------------------ render ----

    void onRenderPre(PHLMONITOR monitor) {
        // Reap finished overlays before anything else touches them.
        if (g_overview && g_overview->finished())
            destroyOverview();
        if (g_switcher && g_switcher->finished())
            destroySwitcher();

        if (!monitor)
            return;

        const bool OWNS_MONITOR = (g_overview && g_overview->monitor() == monitor) || (g_switcher && g_switcher->monitor() == monitor);

        // The overlays repaint the whole output every frame and (for the
        // overview) occlude everything under them. Damage tracking alone leaves
        // stale content from older buffers in the swapchain, so ask for complete
        // frames while either overlay is up.
        if (OWNS_MONITOR)
            monitor->m_forceFullFrames = 2;

        if (g_overview && g_overview->monitor() == monitor)
            g_overview->prepareFrame();
    }

    void onRenderStage(eRenderStage stage) {
        if (!active())
            return;

        // Normally hyprspace is deliberately last, above bars and notifications.
        // A screenshot selector is the one exception: draw before top/overlay
        // layers so its frozen preview and selection UI remain visible above us.
        const auto WANTED_STAGE = g_externalUi.active ? RENDER_POST_WINDOWS : RENDER_LAST_MOMENT;
        if (stage != WANTED_STAGE)
            return;

        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MONITOR)
            return;

        // Add to the pass being built rather than calling draw() directly:
        // draw() renders immediately with whatever damage it is handed, and the
        // pass is what computes the real per-element damage and occlusion.
        if (g_overview && g_overview->monitor() == MONITOR)
            g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>(g_overview.get()));

        if (g_switcher && g_switcher->monitor() == MONITOR)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CSwitcherPassElement>(g_switcher.get()));
    }

    void onMonitorRemoved(PHLMONITOR monitor) {
        if (g_overview && g_overview->monitor() == monitor)
            destroyOverview();
        if (g_switcher && g_switcher->monitor() == monitor)
            destroySwitcher();
    }

    // -------------------------------------------------------- dispatchers ----

    SDispatchResult dispatchOverview(std::string args) {
        // An already-open overview toggles closed, which makes a single Super
        // binding behave the way people expect. An overview that is mid-close
        // does not count as open, otherwise a quick second press would be
        // swallowed instead of reopening.

        if (g_overview && !g_overview->closing()) {
            if (args != "on")
                g_overview->close(false);
            return {.success = true};
        }

        if (args == "off") {
            if (g_overview)
                g_overview->close(false);
            return {.success = true};
        }

        if (g_switcher)
            destroySwitcher();

        const auto MONITOR = targetMonitor();
        if (!MONITOR)
            return {.success = false, .error = "hyprspace: no monitor"};

        // Tear the closing instance down *before* building the new one: the
        // overview parks the real windows at zero alpha and restores them in its
        // destructor, so overlapping lifetimes would let the new instance record
        // the hidden value as the one to restore.
        destroyOverview();

        g_overview = std::make_unique<COverview>(MONITOR);
        return {.success = true};
    }

    SDispatchResult dispatchSwitch(std::string args) {
        const bool FORWARD = args != "prev" && args != "backward";

        if (g_overview)
            g_overview->close(false);

        if (g_switcher && !g_switcher->closing()) {
            g_switcher->advance(FORWARD);
            return {.success = true};
        }

        const auto MONITOR = targetMonitor();
        if (!MONITOR)
            return {.success = false, .error = "hyprspace: no monitor"};

        destroySwitcher();

        auto sw = std::make_unique<CSwitcher>(MONITOR, FORWARD);
        if (sw->empty())
            return {.success = true}; // nothing to switch between

        g_switcher = std::move(sw);

        // If the dispatcher was reached without Alt held (e.g. bound to a plain
        // key), there will never be an Alt release to commit on. Fall back to
        // committing on Enter/click, which onKey already handles.
        return {.success = true};
    }

    SDispatchResult dispatchClose(std::string) {
        if (g_overview)
            g_overview->close(false);
        if (g_switcher)
            g_switcher->close(false);
        return {.success = true};
    }

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // __hyprland_api_get_hash() resolves to the running compositor's symbol;
    // __hyprland_api_get_client_hash() is inline in the headers and so carries
    // the ABI string this plugin was compiled against. Both cover the commit
    // hash *and* the hypr* library versions. A mismatch would crash the session
    // later, so refuse to load now.
    const std::string RUNNING = __hyprland_api_get_hash();
    const std::string BUILT   = __hyprland_api_get_client_hash();

    if (RUNNING != BUILT) {
        HyprlandAPI::addNotification(PHANDLE, HS_LOG_PREFIX "built for " + BUILT + " but Hyprland is " + RUNNING + " — rebuild with `make && make install`",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
        throw std::runtime_error("[hyprspace] Hyprland ABI mismatch, rebuild the plugin");
    }

    config::registerAll();

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprspace:overview", dispatchOverview);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprspace:switch", dispatchSwitch);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprspace:close", dispatchClose);

    auto& bus = Event::bus()->m_events;

    g_listeners.key            = bus.input.keyboard.key.listen(onKey);
    g_listeners.mouseMove      = bus.input.mouse.move.listen(onMouseMove);
    g_listeners.mouseButton    = bus.input.mouse.button.listen(onMouseButton);
    g_listeners.mouseAxis      = bus.input.mouse.axis.listen(onMouseAxis);
    g_listeners.renderPre      = bus.render.pre.listen(onRenderPre);
    g_listeners.renderStage    = bus.render.stage.listen(onRenderStage);
    g_listeners.monitorRemoved = bus.monitor.removed.listen(onMonitorRemoved);
    g_listeners.configReloaded = bus.config.reloaded.listen([] { textures().clear(); });
    g_listeners.layerOpened    = bus.layer.opened.listen(onLayerOpened);
    g_listeners.layerClosed    = bus.layer.closed.listen(onLayerClosed);

    // Filesystem discovery starts early but never runs on the compositor thread.
    // In the unlikely event Alt+Tab wins the race, it gets instant placeholders
    // which are replaced as soon as the index is ready.
    startIconDiscovery();

    // Hyprland parses the config before it finishes loading plugins, so any
    // `bind = ..., hyprspace:overview` in that same config is rejected with
    // "invalid dispatcher". Queue a reload now that the dispatchers exist; the
    // second pass registers them. This cannot loop, because a plugin is never
    // initialised twice.
    HyprlandAPI::reloadConfig();

    return {"hyprspace", "Live workspace overview and GNOME-style Alt+Tab switcher", "hyprspace", "1.0.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    destroyOverview();
    destroySwitcher();

    g_listeners = {};
    g_swallowedPresses.clear();
    if (g_externalUi.timeout)
        g_pEventLoopManager->removeTimer(g_externalUi.timeout);
    g_externalUi.layerCloseCheck.reset();
    g_externalUi.timeout.reset();
    g_externalUi = {};

    textures().clear();
    finishIconDiscovery();
}
