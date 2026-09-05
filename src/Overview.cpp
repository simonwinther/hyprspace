#include "Overview.hpp"

#include "Access.hpp"
#include "Config.hpp"
#include "Focus.hpp"
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include "OverviewLayout.hpp"
#include "PassElements.hpp"
#include "PreviewStyle.hpp"
#include "Texture.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

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

        // Start on the workspace the user is already looking at.
        m_selected = 0;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].isActive) {
                m_selected = static_cast<int>(i);
                break;
            }
        }

        Animation::mgr()->createAnimation(0.F, m_progress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        m_progress->setUpdateCallback([this](auto) { damage(); });
        m_progress->setValueAndWarp(0.F);
        *m_progress = 1.F;

        damage();
    }

    COverview::~COverview() {

        m_capture.clear();

        // Windows on inactive workspaces were un-suspended for the overview; let
        // the compositor work out the correct state again.
        Desktop::globalWindowController()->updateSuspendedStates();
    }

    SBoxF COverview::boxFor(const PHLWINDOW& w) const {
        const auto MONITOR = m_monitor.lock();
        if (!w || !MONITOR)
            return m_usable;

        const auto POS = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
        const auto SZ  = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

        if (SZ.x < 1.0 || SZ.y < 1.0)
            return m_usable;

        return SBoxF{POS.x, POS.y, SZ.x, SZ.y};
    }

    void COverview::collect() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const bool SPECIAL = config::overviewIncludeSpecial();

        // Empty active and persistent workspaces are valid destinations too.
        for (const auto& ws : State::workspaceState()->workspacesCopy()) {
            if (!ws || ws->m_monitor != MONITOR || (ws->m_isSpecialWorkspace && !SPECIAL))
                continue;
            const auto rule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(ws);
            if (ws != MONITOR->m_activeWorkspace && ws != MONITOR->m_activeSpecialWorkspace && ws->getWindowCount() == 0 &&
                !(rule && rule->m_isPersistent.value_or(false)))
                continue;
            SEntry entry;
            entry.workspaceId   = ws->m_id;
            entry.workspaceName = ws->m_name;
            entry.name          = workspaceLabel(ws->m_id, ws->m_name);
            entry.isActive      = ws == MONITOR->m_activeWorkspace || ws == MONITOR->m_activeSpecialWorkspace;
            m_entries.push_back(std::move(entry));
        }

        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || !w->m_isMapped || w->isHidden())
                continue;
            if (w->m_monitor.lock() != MONITOR)
                continue;

            const auto WS = w->m_workspace;
            if (!WS)
                continue;
            if (WS->m_isSpecialWorkspace && !SPECIAL)
                continue;

            const auto SIZE = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            if (SIZE.x < 1.0 || SIZE.y < 1.0)
                continue;

            auto it = std::ranges::find_if(m_entries, [&](const SEntry& e) { return e.workspaceId == WS->m_id; });
            if (it == m_entries.end()) {
                SEntry e;
                e.workspaceId   = WS->m_id;
                e.workspaceName = WS->m_name;
                e.name          = workspaceLabel(WS->m_id, WS->m_name);
                e.isActive      = WS == MONITOR->m_activeWorkspace || WS == MONITOR->m_activeSpecialWorkspace;
                m_entries.push_back(e);
                it = std::prev(m_entries.end());
            }

            SWindowSlot slot;
            slot.window = w;

            it->windows.push_back(slot);

            // Clients on hidden workspaces are suspended by the compositor and
            // would otherwise show a frozen last frame.
            session().hideWindow(w);
            w->setSuspended(false);
        }

        std::ranges::sort(m_entries, [](const SEntry& a, const SEntry& b) { return a.workspaceId < b.workspaceId; });
        for (auto& entry : m_entries)
            updateWindowLayout(entry);
    }

    void COverview::refreshWindows() {
        if (!session().drag.active()) {
            if (const auto mon = monitor()) {
                const auto& reserved = mon->m_reservedArea;
                m_usable             = {reserved.left(), reserved.top(), std::max(1.0, mon->m_size.x - reserved.left() - reserved.right()),
                                        std::max(1.0, mon->m_size.y - reserved.top() - reserved.bottom())};
            }
        }
        const auto selected = selectedTarget();
        auto       previous = std::move(m_entries);
        m_entries.clear();
        collect();
        if (session().drag.active()) {
            // Retain tile positions and identities until release, including a
            // now-empty source. New tiles join the layout after the drag.
            for (auto& entry : previous) {
                auto fresh = std::ranges::find_if(m_entries, [&](const auto& e) { return e.workspaceId == entry.workspaceId && e.workspaceName == entry.workspaceName; });
                entry.windows = fresh == m_entries.end() ? std::vector<SWindowSlot>{} : std::move(fresh->windows);
                updateWindowLayout(entry);
            }
            m_entries = std::move(previous);
        } else {
            computeLayout();
            if (selected)
                selectTarget(*selected);
        }
    }

    void COverview::updateWindowLayout(SEntry& entry) {
        std::vector<SOverviewWindowInput> input;
        entry.drawOrder.clear();
        for (size_t i = 0; i < entry.windows.size(); ++i) {
            auto&      slot = entry.windows[i];
            const auto W    = slot.window.lock();
            if (!W || !W->m_isMapped || W->isHidden() || !W->m_workspace)
                continue;

            slot.desktopRect = boxFor(W);
            slot.fullscreen  = Fullscreen::controller()->getFullscreenModes(W).internal;
            slot.renderLayer = slot.fullscreen != Fullscreen::FSMODE_NONE ? 1 : (W->m_isFloating && W->shouldRenderOverFullscreen() ? 2 : 0);
            input.push_back({slot.desktopRect, slot.fullscreen != Fullscreen::FSMODE_NONE});
            entry.drawOrder.push_back(i);
        }

        // A focus commit can change fullscreen and desktop geometry. Keep the
        // overview endpoint fixed throughout the closing animation.
        if (!m_closing) {
            const auto LAYOUT    = layoutOverviewWindows(input, m_usable, session().drag.active() ? entry.previewColumns : 0);
            entry.spread         = LAYOUT.spread;
            entry.previewColumns = LAYOUT.columns;
            for (size_t i = 0; i < entry.drawOrder.size(); ++i)
                entry.windows[entry.drawOrder[i]].previewRect = LAYOUT.boxes[i];
        }

        // Match the compositor at the desktop endpoint without changing the
        // preview order: fullscreen covers tiled windows, with permitted floats above.
        std::stable_sort(entry.drawOrder.begin(), entry.drawOrder.end(), [&](size_t a, size_t b) { return entry.windows[a].renderLayer < entry.windows[b].renderLayer; });
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
        params.screenW    = m_usable.w;
        params.screenH    = m_usable.h;
        params.padding    = config::overviewPadding();
        params.gap        = config::overviewGap();
        params.aspect     = m_usable.h > 0 ? m_usable.w / m_usable.h : 16.0 / 9.0;
        params.labelSpace = config::overviewShowLabels() ? 34.0 : 0.0;

        auto result = layout(input, params);
        m_tiles     = std::move(result.tiles);

        for (auto& t : m_tiles) {
            t.box.x += m_usable.x;
            t.box.y += m_usable.y;
            m_entries[t.key].target = t.box;
        }

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
        m_animationAnchor = entryIdx;
        const SBoxF FULL  = m_usable;

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

    SBoxF COverview::interpolate(const SEntry& e) const {
        return lerpBox(e.start, e.target, m_progress->value());
    }

    SBoxF COverview::windowBoxInCell(const SBoxF& r, const SBoxF& cell) const {
        const double s = m_usable.w > 0 ? cell.w / m_usable.w : 0.0;
        return SBoxF{cell.x + (r.x - m_usable.x) * s, cell.y + (r.y - m_usable.y) * s, r.w * s, r.h * s};
    }

    SWindowPreviewGeometry COverview::geometryFor(const SEntry& entry, const SWindowSlot& slot) const {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return {};

        const bool  ANCHOR = m_animationAnchor >= 0 && &entry == &m_entries[m_animationAnchor];
        const auto  CELL   = ANCHOR ? entry.target : interpolate(entry);
        const SBoxF CLIP   = slot.fullscreen != Fullscreen::FSMODE_NONE ? SBoxF{0, 0, MONITOR->m_size.x, MONITOR->m_size.y} : m_usable;
        return overviewWindowGeometry({windowBoxInCell(slot.previewRect, CELL), CELL}, {slot.desktopRect, CLIP}, ANCHOR, m_progress->value());
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
        if (const auto WS = State::workspaceState()->query().id(e.workspaceId).run())
            return WS;

        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || e.workspaceId < 1)
            return nullptr;

        return State::workspaceState()->create(e.workspaceId, MONITOR->m_id);
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

        for (const auto& WS : State::workspaceState()->workspacesCopy()) {
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
        // Everything here acts on this overview's own monitor. Several
        // overviews can be up at once, and the one being picked from is not
        // necessarily the monitor Hyprland considers current — the pointer
        // moved there, but the input grab stopped focus following it — so the
        // current-monitor actions would rearrange the screen you are not
        // looking at.
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        // A number key naming a workspace with no tile of its own still goes
        // there — it is just empty, and Hyprland creates it as needed.
        if (m_gotoWorkspace > 0) {
            // One that already exists may live on another output, and moving it
            // is Hyprland's call to make, exactly as Super+N would. One that
            // does not exist yet belongs to the monitor whose overview asked
            // for it rather than to whichever one holds focus.
            const auto WS = State::workspaceState()->query().id(m_gotoWorkspace).run();

            if (WS)
                (void)Config::Actions::changeWorkspace(std::to_string(m_gotoWorkspace));
            else if (const auto FRESH = State::workspaceState()->create(m_gotoWorkspace, MONITOR->m_id))
                MONITOR->changeWorkspace(FRESH);

            return;
        }

        if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return;

        const auto& entry = m_entries[m_tiles[m_selected].key];

        // Clicking a specific window inside a tile goes straight to it.
        if (const auto W = m_clickedWindow.lock()) {
            focusSelection(W, config::warpCursor(), true);
            return;
        }

        const auto WS = workspaceForEntry(entry);
        if (!WS)
            return;

        if (WS->m_isSpecialWorkspace)
            MONITOR->setSpecialWorkspace(WS);
        else if (WS != MONITOR->m_activeWorkspace)
            MONITOR->changeWorkspace(WS);

        if (const auto W = WS->getFocusCandidate(); W && W->m_isMapped && !W->isHidden()) {
            focusSelection(W, config::warpCursor());
            return;
        }

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

        m_scroll.reset();
        const bool SHIFT = mods & HL_MODIFIER_SHIFT;

        auto move = [&](EDirection dir) {
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
        case XKB_KEY_Escape:
            if (session().drag.active())
                session().cancelDrag();
            else
                close(false);
            return true;

        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_space:
            close(true);
            return true;

        case XKB_KEY_Tab:
            cycle(SHIFT ? -1 : 1);
            return true;
        case XKB_KEY_ISO_Left_Tab:
            cycle(-1);
            return true;

        case XKB_KEY_Left:
        case XKB_KEY_h:
        case XKB_KEY_H:
            move(EDirection::LEFT);
            return true;

        case XKB_KEY_Right:
        case XKB_KEY_l:
        case XKB_KEY_L:
            move(EDirection::RIGHT);
            return true;

        case XKB_KEY_Up:
        case XKB_KEY_k:
        case XKB_KEY_K:
            move(EDirection::UP);
            return true;

        case XKB_KEY_Down:
        case XKB_KEY_j:
        case XKB_KEY_J:
            move(EDirection::DOWN);
            return true;

        case XKB_KEY_Home:
            selectIndex(0);
            return true;
        case XKB_KEY_End:
            selectIndex(static_cast<int>(m_tiles.size()) - 1);
            return true;

        default:
            break;
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
        for (int i = static_cast<int>(m_tiles.size()) - 1; i >= 0; --i) {
            const auto& entry = m_entries[m_tiles[i].key];
            if (interpolate(entry).contains(local.x, local.y))
                return i;
            for (const auto index : entry.drawOrder) {
                if (previewContains(geometryFor(entry, entry.windows[index]), local.x, local.y))
                    return i;
            }
        }
        return -1;
    }

    PHLWINDOW COverview::windowAtLocal(const Vector2D& local) const {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return nullptr;

        const int IDX = tileAtLocal(local);
        if (IDX < 0)
            return nullptr;

        const auto& entry = m_entries[m_tiles[IDX].key];
        // Topmost window wins, matching the order the tile is drawn in.
        for (const auto index : entry.drawOrder | std::views::reverse) {
            const auto& slot = entry.windows[index];
            if (const auto W = slot.window.lock(); W && W->m_isMapped && !W->isHidden() && previewContains(geometryFor(entry, slot), local.x, local.y))
                return W;
        }

        return nullptr;
    }

    std::optional<SOverviewTarget> COverview::targetAt(const Vector2D& globalPos) const {
        const auto mon = monitor();
        if (!mon || !SBoxF{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y}.contains(globalPos.x, globalPos.y))
            return std::nullopt;
        const auto local = globalPos - mon->m_position;
        const int  index = tileAtLocal(local);
        if (index < 0)
            return std::nullopt;
        const auto&     entry = m_entries[m_tiles[index].key];
        SOverviewTarget target;
        target.workspace  = {entry.workspaceId, entry.workspaceName};
        target.monitor    = mon;
        target.preview    = interpolate(entry);
        target.desktopBox = m_usable;
        target.window     = windowAtLocal(local);
        if (auto w = target.window.lock()) {
            for (const auto& slot : entry.windows) {
                if (slot.window != w)
                    continue;
                target.preview    = geometryFor(entry, slot).box;
                target.desktopBox = slot.desktopRect;
                break;
            }
        }
        const auto point = mapPreviewPoint({local.x, local.y}, target.preview, target.desktopBox);
        if (!point)
            return std::nullopt;
        target.desktop = mon->m_position + Vector2D{point->x, point->y};
        target.preview.x += mon->m_position.x;
        target.preview.y += mon->m_position.y;
        target.desktopBox.x += mon->m_position.x;
        target.desktopBox.y += mon->m_position.y;
        target.monitorBox = m_usable;
        target.monitorBox.x += mon->m_position.x;
        target.monitorBox.y += mon->m_position.y;
        return target;
    }

    std::optional<SOverviewTarget> COverview::selectedTarget() const {
        const auto mon = monitor();
        if (!mon || m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return std::nullopt;
        const auto& entry  = m_entries[m_tiles[m_selected].key];
        auto        target = targetAt(mon->m_position + Vector2D{interpolate(entry).cx(), interpolate(entry).cy()});
        if (target) {
            target->window = m_clickedWindow;
            if (auto w = target->window.lock(); w && (!w->m_workspace || w->m_workspace->m_id != entry.workspaceId))
                target->window.reset();
        }
        return target;
    }

    std::vector<SOverviewTarget> COverview::inspectTargets() const {
        std::vector<SOverviewTarget> result;
        const auto                   mon = monitor();
        if (!mon)
            return result;
        for (const auto& entry : m_entries) {
            auto cell = interpolate(entry);
            cell.x += mon->m_position.x;
            cell.y += mon->m_position.y;
            result.push_back({.workspace = {entry.workspaceId, entry.workspaceName}, .monitor = mon, .preview = cell});
            for (const auto& slot : entry.windows) {
                auto box = geometryFor(entry, slot).box;
                box.x += mon->m_position.x;
                box.y += mon->m_position.y;
                result.push_back({.workspace = {entry.workspaceId, entry.workspaceName}, .monitor = mon, .window = slot.window, .preview = box});
            }
        }
        return result;
    }

    void COverview::selectTarget(const SOverviewTarget& target) {
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            const auto& entry = m_entries[m_tiles[i].key];
            if (entry.workspaceId == target.workspace.id && entry.workspaceName == target.workspace.name) {
                m_selected      = static_cast<int>(i);
                m_clickedWindow = target.window;
                damage();
                return;
            }
        }
    }

    void COverview::onMouseMove(const Vector2D& globalPos) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const auto LOCAL = globalPos - MONITOR->m_position;

        const int  IDX    = tileAtLocal(LOCAL);
        const auto WINDOW = windowAtLocal(LOCAL);

        if (IDX == m_hovered && WINDOW == m_hoveredWindow.lock())
            return;

        m_hovered       = IDX;
        m_hoveredWindow = WINDOW;

        if (IDX >= 0 && config::followMouse()) {
            m_scroll.reset();
            selectIndex(IDX);
        }

        damage();
    }

    // Scrolling steps the selection, the same order Tab walks.
    void COverview::onScroll(const SScrollInput& event) {
        if (m_tiles.empty() || m_closing || session().drag.active())
            return;

        const int STEPS = m_scroll.steps(event);
        if (STEPS == 0)
            return;

        m_clickedWindow.reset();
        const int N = static_cast<int>(m_tiles.size());
        selectIndex(static_cast<int>(((static_cast<int64_t>(m_selected) + STEPS) % N + N) % N));
    }

    bool COverview::onMouseButton(uint32_t button, bool pressed, uint32_t mods) {
        constexpr uint32_t MOUSE_LEFT  = 0x110;
        constexpr uint32_t MOUSE_RIGHT = 0x111;

        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return true;

        const Vector2D LOCAL = g_pInputManager->getMouseCoordsInternal() - MONITOR->m_position;

        if (pressed)
            m_scroll.reset();

        if (!pressed || (mods & HL_MODIFIER_META))
            return true;

        if (button == MOUSE_LEFT) {
            const int HIT = tileAtLocal(LOCAL);
            if (HIT >= 0) {
                selectIndex(HIT);
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
        else
            refreshWindows();

        m_capture.beginFrame();
        m_needsBlur = false;

        for (auto& e : m_entries) {
            updateWindowLayout(e);
            for (auto& slot : e.windows) {
                const auto W = slot.window.lock();
                if (!W || !W->m_isMapped || W->isHidden() || !W->m_workspace)
                    continue;

                // Fullscreen can suspend other clients even on the active workspace.
                W->setSuspended(false);

                slot.blur = hidden::shouldBlurWindow(W);
                m_needsBlur |= slot.blur;

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

        const int  ROUNDING = config::overviewRounding();
        const int  BORDER   = config::overviewBorderSize();
        const auto FONT     = config::overviewFont();

        // Layout is logical; rect and texture pass elements are drawn in physical
        // pixels, so scale on the way out. (boundingBox()/opaqueRegion() stay
        // logical — the render pass scales those itself.)
        auto px = [&](const SBoxF& b) { return CBox{b.x * SCALE, b.y * SCALE, b.w * SCALE, b.h * SCALE}; };

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

        auto border = [&](const SBoxF& b, const CHyprColor& col, double width, double round) {
            if (width <= 0)
                return;
            out.emplace_back(makeUnique<CBorderPassElement>(CBorderPassElement::SBorderData{
                .box        = px(b),
                .grad1      = Config::CGradientValueData(col),
                .round      = static_cast<int>(round * SCALE),
                .borderSize = std::max(1, static_cast<int>(width * SCALE)),
                .outerRound = static_cast<int>((round + width) * SCALE),
            }));
        };
        auto windowTexture = [&](const PHLWINDOW& w, SP<Render::ITexture> t, const SBoxF& b, float visibility, double round, bool blur,
                                 std::optional<SBoxF> clip = std::nullopt) {
            const float OPACITY = windowPreviewOpacity(w->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE), w->m_ruleApplicator->opaque().valueOrDefault(), visibility);
            if (OPACITY <= 0)
                return;
            CTexPassElement::SRenderData data{
                .tex = t, .box = px(b), .a = OPACITY, .blurA = previewUnit(visibility), .round = static_cast<int>(round * SCALE), .blur = blur};
            if (clip)
                data.clipBox = px(*clip);
            out.emplace_back(makeUnique<CWindowPreviewPassElement>(std::move(data), w));
        };

        const SBoxF MONBOX{0, 0, MONITOR->m_size.x, MONITOR->m_size.y};

        // The window being carried is drawn last, over everything, so it is not
        // clipped by the tile it is being dragged out of.
        const auto&     drag    = session().drag;
        const PHLWINDOW DRAGGED = (drag.active() && drag.moved) ? drag.window.lock() : nullptr;

        // Stroke a box. rect() fills, and the tile border trick of drawing a
        // larger rect underneath cannot work over content already drawn.
        auto outline = [&](const SBoxF& b, const CHyprColor& col, double width) {
            if (b.w < 1 || b.h < 1 || width <= 0)
                return;

            width = std::min(width, std::min(b.w, b.h) / 2.0);

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

            const bool  SELECTED = static_cast<int>(i) == m_selected;
            const auto& drop     = session().selection.drop();
            const bool  DROP     = DRAGGED && drop && drop->workspace == SWorkspaceIdentity{entry.workspaceId, entry.workspaceName};
            const bool  HOVERED  = DROP || (!DRAGGED && static_cast<int>(i) == m_hovered);

            // The zoom's anchor stays visible all the way to the desktop, also
            // when closing into a different workspace from the original one.
            const bool   ANCHOR = static_cast<int>(m_tiles[i].key) == m_animationAnchor;
            const auto   STYLE  = workspacePreviewStyle(ANCHOR, SELECTED, PROGRESS);
            const float  FADE   = STYLE.visibility;
            const float  ALPHA  = STYLE.windowVisibility;
            const double round  = ROUNDING * PROGRESS;

            // A hairline around every tile so they read as distinct cards against
            // the wallpaper, with the accent border replacing it on selection.
            if (PROGRESS > 0.2F) {
                // A drop target is drawn as strongly as the selection, so it is
                // obvious where a dragged window is about to land.
                const bool   STRONG = DROP || SELECTED;
                const bool   ACCENT = STRONG || HOVERED;
                const auto   COLOUR = STRONG ? config::overviewActiveBorder() : (HOVERED ? config::overviewHoverBorder() : config::overviewTileBorderColor());
                const double W      = ACCENT ? BORDER : 1.0;

                border(cell, COLOUR.modifyA(COLOUR.a * PROGRESS * FADE), W, round);
            }

            // Fade the backing away with the zoom. Leaving it behind a full-size
            // transparent window would change the background at the hand-off.
            const auto TILEBG = config::overviewTileBgColor();
            rect(cell, TILEBG.modifyA(TILEBG.a * STYLE.plateVisibility), round);

            for (const auto index : entry.drawOrder) {
                const auto& slot = entry.windows[index];
                const auto  W    = slot.window.lock();
                if (!W || !W->m_isMapped || W->isHidden() || W == DRAGGED)
                    continue;

                auto t = m_capture.textureFor(W);
                if (!t)
                    continue;

                const auto  GEOMETRY = geometryFor(entry, slot);
                const auto& b        = GEOMETRY.box;
                if (b.w < 1 || b.h < 1)
                    continue;

                const float VISIBILITY = overviewWindowVisibility(W->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN), ANCHOR, PROGRESS);
                windowTexture(W, t, b, ALPHA * VISIBILITY, round, slot.blur, GEOMETRY.clip);

                const bool   FULLSCREEN = slot.fullscreen != Fullscreen::FSMODE_NONE;
                const auto&  target     = session().selection.command();
                const bool   HOVER      = !drag.active() && target && target->window == W;
                const double MARK       = FULLSCREEN || HOVER ? std::max(2, BORDER) : 1;
                const auto   COL        = HOVER ? config::overviewActiveBorder() : (FULLSCREEN ? config::overviewFullscreenBorder() : config::overviewTileBorderColor());
                if (FULLSCREEN || HOVER || entry.spread) {
                    const auto&  clip = GEOMETRY.clip;
                    const double left = std::max(b.x, clip.x), top = std::max(b.y, clip.y);
                    outline({left, top, std::min(b.x + b.w, clip.x + clip.w) - left, std::min(b.y + b.h, clip.y + clip.h) - top}, COL.modifyA(COL.a * FADE * PROGRESS),
                            MARK);
                }

                if (!FULLSCREEN || PROGRESS <= 0.35F)
                    continue;

                constexpr double PAD_X = 10, PAD_Y = 4, INSET = 8;
                const int        TEXT_WIDTH = static_cast<int>(b.w - 2 * (INSET + MARK + PAD_X));
                if (TEXT_WIDTH <= 0)
                    continue;

                const auto LABEL = slot.fullscreen == Fullscreen::FSMODE_FULLSCREEN ? "fullscreen" : "maximized";
                const auto tb    = textures().text(LABEL, FONT, COL, TEXT_WIDTH, SCALE);
                if (!tb)
                    continue;

                const auto SZ = logicalSize(tb);
                if (SZ.y + 2 * (INSET + MARK + PAD_Y) > b.h)
                    continue;

                const float BADGE_A = (PROGRESS - 0.35F) / 0.65F * FADE;
                const SBoxF badge{b.x + INSET + MARK, b.y + INSET + MARK, SZ.x + PAD_X * 2, SZ.y + PAD_Y * 2};
                const auto  BGCOL = config::overviewTitleBgColor();
                rect(badge, BGCOL.modifyA(BGCOL.a * BADGE_A), badge.h / 2.0);
                tex(tb, {badge.x + PAD_X, badge.y + PAD_Y, SZ.x, SZ.y}, BADGE_A);
            }

            // --- workspace label ------------------------------------------------
            if (config::overviewShowLabels() && PROGRESS > 0.35F) {
                const float LABEL_A = (PROGRESS - 0.35F) / 0.65F * FADE;

                const auto LABELCOL = SELECTED ? config::overviewActiveBorder() : config::overviewLabelColor();
                auto       t2       = textures().text(entry.name, FONT, LABELCOL, static_cast<int>(cell.w), SCALE);
                if (!t2)
                    continue;

                const auto       SZ    = logicalSize(t2);
                constexpr double PAD_X = 14, PAD_Y = 5;

                // A pill centred just under the tile, GNOME-style. Give it a
                // minimum width so single digits do not become tiny circles.
                const double PILL_W = std::max(SZ.x + PAD_X * 2, 46.0);
                const SBoxF  bgBox{
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

        if (drag.active() && drag.moved) {
            if (const auto& drop = session().selection.drop(); drop && drop->monitor == MONITOR) {
                const auto point = mapPreviewPoint({drop->desktop.x, drop->desktop.y}, drop->desktopBox, drop->preview);
                if (point) {
                    const double x = point->x - MONITOR->m_position.x, y = point->y - MONITOR->m_position.y;
                    rect({x - 12, y - 2, 24, 4}, config::overviewActiveBorder());
                    rect({x - 2, y - 12, 4, 24}, config::overviewActiveBorder());
                }
            }
        }

        // --- the window being carried -------------------------------------------
        if (DRAGGED) {
            if (auto t = session().dragTexture(); t && drag.box.w >= 1 && drag.box.h >= 1) {
                // Lifted slightly and outlined, so it reads as picked up rather
                // than as part of whichever tile it happens to be over.
                constexpr double LIFT = 1.04;

                const SBoxF box{
                    drag.box.x - MONITOR->m_position.x - drag.box.w * (LIFT - 1) / 2,
                    drag.box.y - MONITOR->m_position.y - drag.box.h * (LIFT - 1) / 2,
                    drag.box.w * LIFT,
                    drag.box.h * LIFT,
                };

                const auto OUTLINE = config::overviewActiveBorder();
                border(box, OUTLINE, BORDER, ROUNDING);
                windowTexture(DRAGGED, t, box, 0.92F, ROUNDING, hidden::shouldBlurWindow(DRAGGED));
            }
        }

        return out;
    }

} // namespace hyprspace
