#include "Overview.hpp"

#include "Config.hpp"
#include "Focus.hpp"
#include "Texture.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <optional>
#include <ranges>


namespace hyprspace {

    static float lerpf(float a, float b, float t) {
        return a + (b - a) * t;
    }

    static SBoxF lerpBox(const SBoxF& a, const SBoxF& b, float t) {
        return SBoxF{lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.w, b.w, t), lerpf(a.h, b.h, t)};
    }

    COverview::COverview(PHLMONITOR monitor) : m_monitor(monitor) {
        m_originalFocus     = Desktop::focusState()->window();
        m_originalWorkspace = monitor->m_activeWorkspace;

        const auto& RES = monitor->m_reservedArea;
        m_usable        = SBoxF{
                   RES.left(),
                   RES.top(),
                   std::max(1.0, monitor->m_size.x - RES.left() - RES.right()),
                   std::max(1.0, monitor->m_size.y - RES.top() - RES.bottom()),
        };

        collect();
        computeLayout();
        hideRealWindows();

        // Start on the workspace the user is already looking at.
        m_selected = 0;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].isActive) {
                m_selected = static_cast<int>(i);
                break;
            }
        }

        g_pAnimationManager->createAnimation(0.F, m_progress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        m_progress->setUpdateCallback([this](auto) { damage(); });
        m_progress->setValueAndWarp(0.F);
        *m_progress = 1.F;

        damage();
    }

    COverview::~COverview() {
        restoreRealWindows();

        m_capture.clear();
        textures().clear();

        // Windows on inactive workspaces were un-suspended for the overview; let
        // the compositor work out the correct state again.
        g_pCompositor->updateSuspendedStates();
    }

    // Where a window is drawn inside its workspace.
    //
    // Normally that is simply where it is. A fullscreen window is the exception:
    // it covers the entire output, so drawing it truthfully collapses the tile
    // to one window and hides everything else on that workspace. Hyprland keeps
    // the layout's bounding box in m_position/m_size and only overrides
    // m_realPosition/m_realSize while fullscreen, so the box the window returns
    // to on un-fullscreening is still there to read — draw it there instead.
    SBoxF COverview::boxFor(const PHLWINDOW& w) const {
        const auto MONITOR = m_monitor.lock();
        if (!w || !MONITOR)
            return m_usable;

        // The explicit state, not isFullscreen(): that returned true for
        // ordinary tiled windows here, so every window was treated as
        // fullscreen and drawn over the others. This is the field hyprctl
        // reports as "fullscreen", and FSMODE_NONE means exactly that.
        if (w->m_fullscreenState.internal != FSMODE_NONE) {
            const auto POS = w->m_position - MONITOR->m_position;
            const auto SZ  = w->m_size;

            if (SZ.x >= 1.0 && SZ.y >= 1.0)
                return SBoxF{POS.x, POS.y, SZ.x, SZ.y};

            // No usable layout box — a window that came up fullscreen may never
            // have had one. Centre it rather than let it fill the tile.
            return insetBox(m_usable, 0.62);
        }

        const auto POS = w->m_realPosition->value() - MONITOR->m_position;
        const auto SZ  = w->m_realSize->value();

        if (SZ.x < 1.0 || SZ.y < 1.0)
            return m_usable;

        return SBoxF{POS.x, POS.y, SZ.x, SZ.y};
    }

    void COverview::collect() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const bool SPECIAL = config::overviewIncludeSpecial();

        // Group this monitor's mapped windows by workspace. Empty workspaces are
        // skipped entirely — there is nothing to look at on them.
        for (const auto& w : g_pCompositor->m_windows) {
            if (!w || !w->m_isMapped || w->m_fadingOut)
                continue;
            if (w->m_monitor.lock() != MONITOR)
                continue;

            const auto WS = w->m_workspace;
            if (!WS)
                continue;
            if (WS->m_isSpecialWorkspace && !SPECIAL)
                continue;

            const auto SIZE = w->m_realSize->value();
            if (SIZE.x < 1.0 || SIZE.y < 1.0)
                continue;

            auto it = std::ranges::find_if(m_entries, [&](const SEntry& e) { return e.workspaceId == WS->m_id; });
            if (it == m_entries.end()) {
                SEntry e;
                e.workspaceId = WS->m_id;
                e.name     = workspaceLabel(WS->m_id, WS->m_name);
                e.isActive = WS == MONITOR->m_activeWorkspace || WS == MONITOR->m_activeSpecialWorkspace;
                m_entries.push_back(e);
                it = std::prev(m_entries.end());
            }

            SWindowSlot slot;
            slot.window = w;

            // A fullscreen window covers the whole output, including the strip
            // the bar reserved. Tiles map the usable area, so its real geometry
            // would be drawn hanging outside the tile it belongs to. It fills
            // the screen, so let it fill the tile.
            slot.rect = boxFor(w);

            it->windows.push_back(slot);

            // Clients on hidden workspaces are suspended by the compositor and
            // would otherwise show a frozen last frame.
            w->setSuspended(false);
        }

        std::ranges::sort(m_entries, [](const SEntry& a, const SEntry& b) { return a.workspaceId < b.workspaceId; });
    }

    void COverview::computeLayout() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || m_entries.empty())
            return;

        std::vector<STileInput> input;
        input.reserve(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); ++i)
            input.push_back(STileInput{.key = i, .workspaceId = m_entries[i].workspaceId});

        SLayoutParams params;
        params.screenW = MONITOR->m_size.x;
        params.screenH = MONITOR->m_size.y;
        params.padding = config::overviewPadding();
        params.gap     = config::overviewGap();
        params.aspect  = m_usable.h > 0 ? m_usable.w / m_usable.h : 16.0 / 9.0;
        params.labelSpace = config::overviewShowLabels() ? 34.0 : 0.0;

        auto result = layout(input, params);
        m_tiles     = std::move(result.tiles);

        for (const auto& t : m_tiles)
            m_entries[t.key].target = t.box;

        int active = -1;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].isActive) {
                active = static_cast<int>(i);
                break;
            }
        }

        anchorAnimation(active);
    }

    // Pick the tile the animation grows out of and folds back into.
    //
    // That tile starts full-screen, so at progress 0 it *is* the desktop; the
    // rest start slightly shrunk in place, which reads as the grid folding
    // together. Opening anchors on the workspace already on screen. Closing
    // re-anchors on the workspace being switched to, so the zoom lands on what
    // was chosen — anchoring on the old one expands the workspace you just left
    // and then cuts to the new one, which is the glitch.
    //
    // An index of -1 anchors nothing and every tile simply collapses.
    void COverview::anchorAnimation(int entryIdx) {
        const SBoxF FULL = m_usable;

        for (size_t i = 0; i < m_entries.size(); ++i) {
            auto& e = m_entries[i];

            if (static_cast<int>(i) == entryIdx) {
                e.start = FULL;
                continue;
            }

            constexpr double SHRINK = 0.88;
            e.start                 = SBoxF{
                                e.target.x + e.target.w * (1 - SHRINK) / 2,
                                e.target.y + e.target.h * (1 - SHRINK) / 2,
                                e.target.w * SHRINK,
                                e.target.h * SHRINK,
            };
        }
    }

    // The entry the commit is about to land on, or -1 if it lands nowhere with
    // a tile of its own.
    int COverview::committedEntry() const {
        if (m_gotoWorkspace > 0) {
            for (size_t i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].workspaceId == m_gotoWorkspace)
                    return static_cast<int>(i);
            }
            return -1;
        }

        if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return -1;

        return static_cast<int>(m_tiles[m_selected].key);
    }

    // Warp every window to zero alpha so Hyprland's normal pass skips it
    // (renderWindow bails on effectiveAlpha() == 0). What remains underneath is
    // the wallpaper and the bar, which is what gets dimmed. Offscreen captures
    // are unaffected: standalone renders force alpha to 1.
    void COverview::hideRealWindows() {
        for (auto& e : m_entries) {
            for (auto& slot : e.windows) {
                const auto W = slot.window.lock();
                if (!W)
                    continue;

                auto& av         = W->alpha(Desktop::View::WINDOW_ALPHA_FADE);
                slot.savedAlpha  = av->goal();
                slot.alphaHidden = true;
                av->setValueAndWarp(0.F);
            }
        }
    }

    void COverview::restoreRealWindows() {
        for (auto& e : m_entries) {
            for (auto& slot : e.windows) {
                if (!slot.alphaHidden)
                    continue;

                if (const auto W = slot.window.lock())
                    W->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(slot.savedAlpha);

                slot.alphaHidden = false;
            }
        }
    }

    SBoxF COverview::interpolate(const SEntry& e) const {
        return lerpBox(e.start, e.target, m_progress->value());
    }

    SBoxF COverview::windowBoxInCell(const SBoxF& r, const SBoxF& cell) const {
        const double s = m_usable.w > 0 ? cell.w / m_usable.w : 0.0;
        return SBoxF{cell.x + (r.x - m_usable.x) * s, cell.y + (r.y - m_usable.y) * s, r.w * s, r.h * s};
    }

    void COverview::selectIndex(int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_tiles.size()) || idx == m_selected)
            return;

        m_selected = idx;
        damage();
    }

    void COverview::close(bool commitSelection) {
        if (m_closing)
            return;

        m_closing = true;

        if (commitSelection) {
            // Re-anchor before committing, while the selection is still intact,
            // so the closing zoom runs into the workspace being switched to.
            // Nothing to re-anchor means nothing switched, and the layout's own
            // anchor — the workspace on screen — is still the right one.
            const int  ANCHOR    = committedEntry();
            const bool SWITCHING = m_gotoWorkspace > 0;

            if (ANCHOR >= 0 || SWITCHING)
                anchorAnimation(ANCHOR);

            commit();
        }

        *m_progress = 0.F;
        damage();
    }

    // Dragging the last window off a workspace lets Hyprland reap it, so the
    // tile can outlive the workspace it stands for. Recreate it on demand rather
    // than letting the tile go dead.
    PHLWORKSPACE COverview::workspaceForEntry(const SEntry& e) const {
        if (const auto WS = g_pCompositor->getWorkspaceByID(e.workspaceId))
            return WS;

        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || e.workspaceId < 1)
            return nullptr;

        return g_pCompositor->createNewWorkspace(e.workspaceId, MONITOR->m_id);
    }

    // Hyprland animates a workspace change itself: the outgoing workspace
    // slides and fades out while the incoming one slides in. Left alone that
    // runs *underneath* the overview's zoom and is still in flight when the
    // overview hands the screen back, so the last thing you see is the new
    // workspace sliding into place after the zoom already landed on it — two
    // transitions for one action, which is the glitch.
    //
    // Warp them to their finished state instead. The zoom is the transition.
    void COverview::settleWorkspaceAnimations() const {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        for (const auto& WS : g_pCompositor->getWorkspacesCopy()) {
            if (!WS || WS->m_monitor.lock() != MONITOR)
                continue;

            if (WS->m_renderOffset)
                WS->m_renderOffset->warp();
            if (WS->m_alpha)
                WS->m_alpha->warp();
        }
    }

    void COverview::commit() {
        commitSelection();
        settleWorkspaceAnimations();
    }

    void COverview::commitSelection() {
        // A number key naming a workspace with no tile of its own still goes
        // there — it is just empty, and Hyprland creates it as needed.
        if (m_gotoWorkspace > 0) {
            (void)Config::Actions::changeWorkspace(std::to_string(m_gotoWorkspace));
            return;
        }

        if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return;

        const auto& entry = m_entries[m_tiles[m_selected].key];

        // Clicking a specific window inside a tile goes straight to it.
        if (const auto W = m_clickedWindow.lock()) {
            focusSelection(W, config::warpCursor());
            return;
        }

        const auto WS = workspaceForEntry(entry);
        if (!WS)
            return;

        if (WS->m_isSpecialWorkspace)
            (void)Config::Actions::toggleSpecial(WS);
        else if (WS != m_monitor->m_activeWorkspace)
            (void)Config::Actions::changeWorkspaceOnCurrentMonitor(WS);

        // Focus a window on the target workspace, otherwise input:follow_mouse
        // resolves the pointer's position and undoes the switch. Prefer the one
        // that already had focus.
        const auto PREV = m_originalFocus.lock();
        for (const auto& slot : entry.windows) {
            if (const auto W = slot.window.lock(); W && W == PREV) {
                focusSelection(W, config::warpCursor());
                return;
            }
        }

        if (!entry.windows.empty()) {
            if (const auto W = entry.windows.front().window.lock())
                focusSelection(W, config::warpCursor());
        }
    }

    // ---------------------------------------------------------------- input --

    bool COverview::onKey(xkb_keysym_t sym, uint32_t mods, bool pressed) {
        if (!pressed)
            return true; // swallow releases too while we hold the grab

        const bool SHIFT = mods & HL_MODIFIER_SHIFT;

        auto       move = [&](EDirection dir) {
            m_clickedWindow.reset();
            selectIndex(navigate(m_tiles, m_selected, dir));
        };

        auto cycle = [&](int delta) {
            if (m_tiles.empty())
                return;
            m_clickedWindow.reset();
            const int n = static_cast<int>(m_tiles.size());
            selectIndex(((m_selected + delta) % n + n) % n);
        };

        switch (sym) {
            case XKB_KEY_Escape: close(false); return true;

            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space: close(true); return true;

            case XKB_KEY_Tab: cycle(SHIFT ? -1 : 1); return true;
            case XKB_KEY_ISO_Left_Tab: cycle(-1); return true;

            case XKB_KEY_Left:
            case XKB_KEY_h:
            case XKB_KEY_H: move(EDirection::LEFT); return true;

            case XKB_KEY_Right:
            case XKB_KEY_l:
            case XKB_KEY_L: move(EDirection::RIGHT); return true;

            case XKB_KEY_Up:
            case XKB_KEY_k:
            case XKB_KEY_K: move(EDirection::UP); return true;

            case XKB_KEY_Down:
            case XKB_KEY_j:
            case XKB_KEY_J: move(EDirection::DOWN); return true;

            case XKB_KEY_Home: selectIndex(0); return true;
            case XKB_KEY_End: selectIndex(static_cast<int>(m_tiles.size()) - 1); return true;

            default: break;
        }

        // Number keys go straight to that workspace — the same thing Super+N
        // does on the desktop, so requiring an extra Enter would only be in the
        // way. The tile is selected first, so the closing animation still zooms
        // out of the workspace you picked.
        if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
            const long WANT = workspaceForDigit(static_cast<int>(sym - XKB_KEY_0));

            m_clickedWindow.reset();

            for (size_t i = 0; i < m_tiles.size(); ++i) {
                if (m_entries[m_tiles[i].key].workspaceId == WANT) {
                    selectIndex(static_cast<int>(i));
                    close(true);
                    return true;
                }
            }

            // No tile: the workspace is empty or does not exist yet. Go anyway.
            m_gotoWorkspace = WANT;
            close(true);
            return true;
        }

        // Everything else is swallowed: the overview owns the keyboard, and there
        // is deliberately no text entry anywhere in hyprspace.
        return true;
    }

    int COverview::tileAtLocal(const Vector2D& local) const {
        return tileAt(m_tiles, local.x, local.y);
    }

    double COverview::tileScale(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(m_tiles.size()) || m_usable.w <= 0)
            return 1.0;
        return m_tiles[idx].box.w / m_usable.w;
    }

    PHLWINDOW COverview::windowAtLocal(const Vector2D& local) const {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return nullptr;

        const int IDX = tileAtLocal(local);
        if (IDX < 0)
            return nullptr;

        const auto& entry = m_entries[m_tiles[IDX].key];
        const SBoxF cell  = m_tiles[IDX].box;

        // Topmost window wins, matching the order the tile is drawn in.
        for (const auto& slot : entry.windows | std::views::reverse) {
            if (windowBoxInCell(slot.rect, cell).contains(local.x, local.y))
                return slot.window.lock();
        }

        return nullptr;
    }

    // ------------------------------------------------------ super + drag ----

    bool COverview::beginDrag(EDrag mode, const Vector2D& local) {
        const int IDX = tileAtLocal(local);
        if (IDX < 0)
            return false;

        const auto& entry = m_entries[m_tiles[IDX].key];
        const SBoxF cell  = m_tiles[IDX].box;

        for (const auto& slot : entry.windows | std::views::reverse) {
            const SBoxF b = windowBoxInCell(slot.rect, cell);
            if (!b.contains(local.x, local.y))
                continue;

            const auto W = slot.window.lock();
            if (!W)
                return false;

            // A fullscreen window has no geometry of its own to move or resize;
            // driving the layout engine at one is asking for trouble.
            if (W->m_fullscreenState.internal != FSMODE_NONE)
                return false;

            m_drag             = {};
            m_drag.mode        = mode;
            m_drag.window      = W;
            m_drag.sourceTile  = IDX;
            m_drag.targetTile  = IDX;
            m_drag.grabOffset  = Vector2D{local.x - b.x, local.y - b.y};
            m_drag.lastPos     = local;
            m_drag.box         = b;

            selectIndex(IDX);
            damage();
            return true;
        }

        return false;
    }

    void COverview::updateDrag(const Vector2D& local) {
        const auto W = m_drag.window.lock();
        if (!W) {
            m_drag = {};
            return;
        }

        // A few pixels of slop, so Super+click on a window is not turned into a
        // one-pixel move or a jittery resize.
        constexpr double THRESHOLD = 5.0;

        const Vector2D   DELTA = local - m_drag.lastPos;
        m_drag.lastPos         = local;

        if (!m_drag.moved) {
            if (std::abs(local.x - (m_drag.box.x + m_drag.grabOffset.x)) < THRESHOLD && std::abs(local.y - (m_drag.box.y + m_drag.grabOffset.y)) < THRESHOLD)
                return;
            m_drag.moved = true;
        }

        if (m_drag.mode == EDrag::MOVE) {
            m_drag.box.x      = local.x - m_drag.grabOffset.x;
            m_drag.box.y      = local.y - m_drag.grabOffset.y;
            m_drag.targetTile = tileAtLocal(local);
        } else {
            // The tile is a scaled-down monitor, so undo that scale to get the
            // resize the pointer actually described. Hyprland's own mouse resize
            // is incremental too, which is what keeps tiled splits sane.
            const double S = tileScale(m_drag.sourceTile);
            if (S > 0.0001)
                (void)Config::Actions::resize(Vector2D{DELTA.x / S, DELTA.y / S}, true, W);
        }

        damage();
    }

    void COverview::finishDrag() {
        const auto W   = m_drag.window.lock();
        const int  SRC = m_drag.sourceTile;
        const int  DST = m_drag.targetTile;

        const bool MOVED = m_drag.moved && m_drag.mode == EDrag::MOVE;
        m_drag           = {};

        if (!W || !MOVED || SRC < 0 || DST < 0 || SRC >= static_cast<int>(m_tiles.size()) || DST >= static_cast<int>(m_tiles.size())) {
            damage();
            return;
        }

        const size_t SRC_KEY = m_tiles[SRC].key;
        const size_t DST_KEY = m_tiles[DST].key;

        if (SRC_KEY == DST_KEY) {
            damage();
            return;
        }

        const auto WS = workspaceForEntry(m_entries[DST_KEY]);
        if (!WS) {
            damage();
            return;
        }

        (void)Config::Actions::moveToWorkspace(WS, /* silent */ true, W);

        // Carry the slot across by hand instead of rebuilding the layout: the
        // tiles must not reshuffle under the pointer mid-gesture, and the slot
        // holds the alpha this window has to be restored to on close.
        auto& src = m_entries[SRC_KEY].windows;
        auto  it  = std::ranges::find_if(src, [&](const SWindowSlot& s) { return s.window.lock() == W; });

        if (it != src.end()) {
            SWindowSlot moved = *it;
            src.erase(it);
            m_entries[DST_KEY].windows.push_back(moved);
        }

        damage();
    }

    void COverview::onMouseMove(const Vector2D& globalPos) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const auto LOCAL = globalPos - MONITOR->m_position;

        if (m_drag.active()) {
            updateDrag(LOCAL);
            return;
        }

        const int IDX = tileAtLocal(LOCAL);

        if (IDX == m_hovered)
            return;

        m_hovered = IDX;

        if (IDX >= 0 && config::followMouse())
            selectIndex(IDX);

        damage();
    }

    // Scrolling steps the selection, the same order Tab walks.
    void COverview::onScroll(double delta) {
        if (m_tiles.empty() || m_closing || delta == 0.0)
            return;

        const int N = static_cast<int>(m_tiles.size());
        selectIndex(((m_selected + (delta > 0.0 ? 1 : -1)) % N + N) % N);
    }

    bool COverview::onMouseButton(uint32_t button, bool pressed, uint32_t mods) {
        constexpr uint32_t MOUSE_LEFT  = 0x110;
        constexpr uint32_t MOUSE_RIGHT = 0x111;

        const auto         MONITOR = m_monitor.lock();
        if (!MONITOR)
            return true;

        const Vector2D LOCAL = g_pInputManager->getMouseCoordsInternal() - MONITOR->m_position;

        if (!pressed) {
            // A Super+drag that never moved falls through as a plain click, so a
            // mis-grab still does the obvious thing rather than nothing.
            const bool WAS_MOVE  = m_drag.mode == EDrag::MOVE;
            const bool WAS_CLICK = m_drag.active() && !m_drag.moved;

            if (m_drag.active())
                finishDrag();

            if (WAS_CLICK && WAS_MOVE && button == MOUSE_LEFT) {
                m_clickedWindow = windowAtLocal(LOCAL);
                close(true);
            }

            return true;
        }

        // Super + left drags a window to another workspace, Super + right
        // resizes it in place — the same gestures as on the desktop, just
        // scaled into the grid.
        if (mods & HL_MODIFIER_META) {
            if (button == MOUSE_LEFT && beginDrag(EDrag::MOVE, LOCAL))
                return true;
            if (button == MOUSE_RIGHT && beginDrag(EDrag::RESIZE, LOCAL))
                return true;
            return true; // Super held: never dismiss, the user is aiming
        }

        if (button == MOUSE_LEFT) {
            if (m_hovered >= 0) {
                selectIndex(m_hovered);
                m_clickedWindow = windowAtLocal(LOCAL);
                close(true);
            } else
                close(false); // click on empty space dismisses, like GNOME
            return true;
        }

        if (button == MOUSE_RIGHT) {
            close(false);
            return true;
        }

        return true;
    }

    // --------------------------------------------------------------- render --

    void COverview::damage() {
        if (auto m = m_monitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    void COverview::prepareFrame() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        // Every frame of the close, not just the moment of the commit: Hyprland
        // may not have started its workspace slide yet when the selection is
        // committed, and a single warp then would be undone by an animation
        // that begins a frame later.
        if (m_closing)
            settleWorkspaceAnimations();

        m_capture.beginFrame();

        for (auto& e : m_entries) {
            for (auto& slot : e.windows) {
                const auto W = slot.window.lock();
                if (!W || !W->m_isMapped)
                    continue;

                // Re-assert: a workspace change elsewhere can re-suspend these.
                if (!e.isActive)
                    W->setSuspended(false);

                // Track live geometry, so a Super+right-drag resize and the
                // relayout that follows a drag between workspaces are both
                // visible in the grid as they happen.
                slot.rect = boxFor(W);

                m_capture.capture(W, MONITOR);
            }
        }

        m_capture.endFrame();

        damage();
    }

    std::vector<UP<IPassElement>> COverview::buildPass() {
        std::vector<UP<IPassElement>> out;

        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return out;

        const float  PROGRESS = std::clamp(m_progress->value(), 0.F, 1.F);
        const double SCALE    = MONITOR->m_scale;

        const int    ROUNDING = config::overviewRounding();
        const int    BORDER   = config::overviewBorderSize();
        const auto   FONT     = config::overviewFont();

        // Layout is logical; rect and texture pass elements are drawn in physical
        // pixels, so scale on the way out. (boundingBox()/opaqueRegion() stay
        // logical — the render pass scales those itself.)
        auto px   = [&](const SBoxF& b) { return CBox{b.x * SCALE, b.y * SCALE, b.w * SCALE, b.h * SCALE}; };

        auto rect = [&](const SBoxF& b, const CHyprColor& col, double round = 0) {
            out.emplace_back(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = px(b), .color = col, .round = static_cast<int>(round * SCALE)}));
        };
        auto tex = [&](SP<Render::ITexture> t, const SBoxF& b, float a, double round = 0, std::optional<SBoxF> clip = std::nullopt) {
            CTexPassElement::SRenderData d{.tex = t, .box = px(b), .a = a, .round = static_cast<int>(round * SCALE)};
            if (clip)
                d.clipBox = px(*clip);
            out.emplace_back(makeUnique<CTexPassElement>(std::move(d)));
        };
        auto logicalSize = [&](const SP<Render::ITexture>& t) { return Vector2D{t->m_size.x / SCALE, t->m_size.y / SCALE}; };

        const SBoxF MONBOX{0, 0, MONITOR->m_size.x, MONITOR->m_size.y};

        // The window being carried is drawn last, over everything, so it is not
        // clipped by the tile it is being dragged out of.
        const PHLWINDOW DRAGGED = (m_drag.mode == EDrag::MOVE && m_drag.moved) ? m_drag.window.lock() : nullptr;

        // Stroke a box. rect() fills, and the tile border trick of drawing a
        // larger rect underneath cannot work over content already drawn.
        auto outline = [&](const SBoxF& b, const CHyprColor& col, double width) {
            if (b.w < 1 || b.h < 1 || width <= 0)
                return;

            rect(SBoxF{b.x, b.y, b.w, width}, col);                       // top
            rect(SBoxF{b.x, b.y + b.h - width, b.w, width}, col);         // bottom
            rect(SBoxF{b.x, b.y + width, width, b.h - width * 2.0}, col); // left
            rect(SBoxF{b.x + b.w - width, b.y + width, width, b.h - width * 2.0}, col);
        };

        // --- backdrop -----------------------------------------------------------
        // The real windows are alpha-0 while the overview is up, so what sits
        // underneath is the wallpaper and the bar. Dimming those rather than
        // painting over them is what makes this read like the GNOME overview.
        rect(MONBOX, config::overviewBgColor().modifyA(config::overviewBgDim() * PROGRESS));

        // --- workspace tiles ----------------------------------------------------
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            const auto& entry = m_entries[m_tiles[i].key];

            const SBoxF cell = interpolate(entry);
            if (cell.w <= 1 || cell.h <= 1)
                continue;

            const bool   SELECTED = static_cast<int>(i) == m_selected;
            const bool   DROP     = DRAGGED && static_cast<int>(i) == m_drag.targetTile && m_drag.targetTile != m_drag.sourceTile;
            const bool   HOVERED  = DROP || (!DRAGGED && static_cast<int>(i) == m_hovered);

            // The active workspace starts full-screen and is already opaque; the
            // rest fade in as the grid forms. Unselected tiles then sit back a
            // little so the selection reads at a glance without hurting how much
            // of each workspace you can actually make out.
            const float  FADE  = entry.isActive ? 1.F : PROGRESS;
            const float  ALPHA = FADE * (SELECTED || PROGRESS < 0.2F ? 1.F : 0.9F);
            const double round = ROUNDING * PROGRESS;

            // A hairline around every tile so they read as distinct cards against
            // the wallpaper, with the accent border replacing it on selection.
            if (PROGRESS > 0.2F) {
                // A drop target is drawn as strongly as the selection, so it is
                // obvious where a dragged window is about to land.
                const bool   STRONG = DROP || SELECTED;
                const bool   ACCENT = STRONG || HOVERED;
                const auto   COLOUR = STRONG ? config::overviewActiveBorder() : (HOVERED ? config::overviewHoverBorder() : config::overviewTileBorderColor());
                const double W      = ACCENT ? BORDER : 1.0;

                if (W > 0)
                    rect(SBoxF{cell.x - W, cell.y - W, cell.w + W * 2.0, cell.h + W * 2.0}, COLOUR.modifyA(COLOUR.a * PROGRESS * FADE), round + W);
            }

            // A backing plate, so a workspace's empty area reads as a screen
            // rather than a hole punched in the dim.
            const auto TILEBG = config::overviewTileBgColor();
            rect(cell, TILEBG.modifyA(TILEBG.a * FADE), round);

            // A fullscreen window is drawn where it will land when it stops
            // being fullscreen, not where it actually is. Drawn literally it
            // covers the whole tile and the overview stops answering the one
            // question it exists to answer: where is everything. Its restored
            // box is still readable while it is fullscreen, so use that and
            // mark it, rather than fading it out or hiding what it covers.
            const SWindowSlot* fsSlot = nullptr;
            for (const auto& slot : entry.windows) {
                const auto W = slot.window.lock();
                if (W && W != DRAGGED && W->m_fullscreenState.internal != FSMODE_NONE)
                    fsSlot = &slot;
            }

            for (const auto& slot : entry.windows) {
                const auto W = slot.window.lock();
                if (!W || W == DRAGGED)
                    continue;

                auto t = m_capture.textureFor(W);
                if (!t)
                    continue;

                const SBoxF b = windowBoxInCell(slot.rect, cell);
                if (b.w < 1 || b.h < 1)
                    continue;

                // Clip to the cell: a window can reach past the usable area, and
                // would otherwise spill over the tile's edges.
                tex(t, b, ALPHA, round, cell);
            }

            // Say which window is the fullscreen one, since it is no longer
            // drawn fullscreen and nothing else would give it away.
            if (fsSlot) {
                const SBoxF FSBOX = windowBoxInCell(fsSlot->rect, cell);

                if (FSBOX.w >= 1 && FSBOX.h >= 1) {
                    const double MARK = std::max(2.0, static_cast<double>(config::overviewBorderSize()));
                    const auto   COL  = config::overviewFullscreenBorder();

                    outline(FSBOX, COL.modifyA(COL.a * FADE), MARK);

                    if (PROGRESS > 0.35F) {
                        const float BADGE_A = (PROGRESS - 0.35F) / 0.65F * FADE;

                        if (auto tb = textures().text("fullscreen", FONT, COL, static_cast<int>(cell.w), SCALE)) {
                            const auto       SZ    = logicalSize(tb);
                            constexpr double PAD_X = 10, PAD_Y = 4, INSET = 8;

                            const SBoxF      badge{
                                     FSBOX.x + INSET + MARK,
                                     FSBOX.y + INSET + MARK,
                                     SZ.x + PAD_X * 2,
                                     SZ.y + PAD_Y * 2,
                            };

                            const auto BGCOL = config::overviewTitleBgColor();
                            rect(badge, BGCOL.modifyA(BGCOL.a * BADGE_A), badge.h / 2.0);
                            tex(tb, SBoxF{badge.x + PAD_X, badge.y + PAD_Y, SZ.x, SZ.y}, BADGE_A);
                        }
                    }
                }
            }

            // --- workspace label ------------------------------------------------
            if (config::overviewShowLabels() && PROGRESS > 0.35F) {
                const float LABEL_A = (PROGRESS - 0.35F) / 0.65F * FADE;

                const auto  LABELCOL = SELECTED ? config::overviewActiveBorder() : config::overviewLabelColor();
                auto        t2       = textures().text(entry.name, FONT, LABELCOL, static_cast<int>(cell.w), SCALE);
                if (!t2)
                    continue;

                const auto       SZ    = logicalSize(t2);
                constexpr double PAD_X = 14, PAD_Y = 5;

                // A pill centred just under the tile, GNOME-style. Give it a
                // minimum width so single digits do not become tiny circles.
                const double     PILL_W = std::max(SZ.x + PAD_X * 2, 46.0);
                const SBoxF      bgBox{
                         cell.x + (cell.w - PILL_W) / 2.0,
                         cell.y + cell.h + 9,
                         PILL_W,
                         SZ.y + PAD_Y * 2,
                };

                const auto BGCOL = config::overviewTitleBgColor();
                rect(bgBox, BGCOL.modifyA(BGCOL.a * LABEL_A), bgBox.h / 2.0);
                tex(t2, SBoxF{bgBox.x + (bgBox.w - SZ.x) / 2.0, bgBox.y + PAD_Y, SZ.x, SZ.y}, LABEL_A);
            }
        }

        // --- the window being carried -------------------------------------------
        if (DRAGGED) {
            if (auto t = m_capture.textureFor(DRAGGED); t && m_drag.box.w >= 1 && m_drag.box.h >= 1) {
                // Lifted slightly and outlined, so it reads as picked up rather
                // than as part of whichever tile it happens to be over.
                constexpr double LIFT = 1.04;

                const SBoxF      box{
                         m_drag.box.x - m_drag.box.w * (LIFT - 1) / 2,
                         m_drag.box.y - m_drag.box.h * (LIFT - 1) / 2,
                         m_drag.box.w * LIFT,
                         m_drag.box.h * LIFT,
                };

                const auto OUTLINE = config::overviewActiveBorder();
                rect(SBoxF{box.x - BORDER, box.y - BORDER, box.w + BORDER * 2.0, box.h + BORDER * 2.0}, OUTLINE, ROUNDING + BORDER);
                tex(t, box, 0.92F, ROUNDING);
            }
        }

        return out;
    }

} // namespace hyprspace
