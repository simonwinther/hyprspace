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
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <xkbcommon/xkbcommon.h>

#include <memory>
#include <unordered_set>

using namespace hyprspace;

namespace {

    std::unique_ptr<COverview> g_overview;
    std::unique_ptr<CSwitcher> g_switcher;

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
        CHyprSignalListener renderPre;
        CHyprSignalListener renderStage;
        CHyprSignalListener monitorRemoved;
        CHyprSignalListener configReloaded;
    };

    SListeners g_listeners;

    // Something is on screen and has to be drawn.
    bool       active() {
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
        const auto KEEB = g_pSeatManager->m_keyboard;
        return KEEB ? KEEB->getModifiers() : 0;
    }

    // The modifier a key is itself, or 0 for an ordinary key.
    uint32_t modifierBitFor(xkb_keysym_t sym) {
        switch (sym) {
            case XKB_KEY_Super_L:
            case XKB_KEY_Super_R:
            case XKB_KEY_Meta_L:
            case XKB_KEY_Meta_R: return HL_MODIFIER_META;

            case XKB_KEY_Alt_L:
            case XKB_KEY_Alt_R: return HL_MODIFIER_ALT;

            case XKB_KEY_Control_L:
            case XKB_KEY_Control_R: return HL_MODIFIER_CTRL;

            case XKB_KEY_Shift_L:
            case XKB_KEY_Shift_R: return HL_MODIFIER_SHIFT;

            default: return 0;
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

            if (switcherLive()) {
                g_switcher->onKey(SYM, modsWith(SYM, false), false);

                // Alt released -> commit, exactly like GNOME.
                if (SYM == XKB_KEY_Alt_L || SYM == XKB_KEY_Alt_R || SYM == XKB_KEY_Meta_L || SYM == XKB_KEY_Meta_R)
                    g_switcher->close(true);
            } else if (overviewLive() && MINE)
                g_overview->onKey(SYM, modsWith(SYM, false), false);

            return;
        }

        if (!ownsInput())
            return;

        const xkb_keysym_t SYM  = keysymFor(event.keycode);
        const uint32_t     MODS = modsWith(SYM, PRESSED);

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
        if (!ownsInput())
            return;

        info.cancelled = true;

        if (switcherLive())
            g_switcher->onMouseMove(pos);
        else if (overviewLive())
            g_overview->onMouseMove(pos);
    }

    void onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (!ownsInput())
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
        // LAST_MOMENT is after the top/overlay layer surfaces, so the overlays
        // sit above bars and notifications the way a full-screen overview should.
        if (stage != RENDER_LAST_MOMENT || !active())
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
    g_listeners.renderPre      = bus.render.pre.listen(onRenderPre);
    g_listeners.renderStage    = bus.render.stage.listen(onRenderStage);
    g_listeners.monitorRemoved = bus.monitor.removed.listen(onMonitorRemoved);
    g_listeners.configReloaded = bus.config.reloaded.listen([] { textures().clear(); });

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

    textures().clear();
}
