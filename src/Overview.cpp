#include "Overview.hpp"

#include "Access.hpp"
#include "Config.hpp"
#include "Focus.hpp"
#include "CompositorHooks.hpp"
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
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
#include <cctype>
#include <cmath>
#include <optional>
#include <ranges>

namespace hyprspace {

    namespace {
        constexpr double EMPTY_WORKSPACE_TEXT_SCALE = 0.9;
        constexpr double EMPTY_WORKSPACE_KEY_SCALE  = 0.75;
    } // namespace

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
        for (auto& entry : m_entries)
            updateWindowLayout(entry);
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
        Animation::mgr()->createAnimation(1.F, m_zoomScale, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        Animation::mgr()->createAnimation(Vector2D{}, m_zoomOffset, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        Animation::mgr()->createAnimation(1.F, m_inspectionProgress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        m_progress->setUpdateCallback([this](auto) { damage(); });
        m_zoomScale->setUpdateCallback([this](auto) { damage(); });
        m_zoomOffset->setUpdateCallback([this](auto) { damage(); });
        m_inspectionProgress->setUpdateCallback([this](auto) { damage(); });
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
                !(rule && rule->m_isPersistent.value_or(false)) && !session().preparedWorkspace(ws))
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
    }

    void COverview::refreshWindows() {
        if (!session().gestureActive()) {
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
        if (session().gestureActive()) {
            // Retain tile positions and identities until release, including a
            // now-empty source. New tiles join the layout after the drag.
            for (auto& entry : previous) {
                auto fresh = std::ranges::find_if(m_entries, [&](const auto& e) { return e.workspaceId == entry.workspaceId && e.workspaceName == entry.workspaceName; });
                entry.windows = fresh == m_entries.end() ? std::vector<SWindowSlot>{} : std::move(fresh->windows);
                updateWindowLayout(entry);
            }
            m_entries = std::move(previous);
        } else {
            for (auto& entry : m_entries) {
                const auto old =
                    std::ranges::find_if(previous, [&](const auto& e) { return e.workspaceId == entry.workspaceId && e.workspaceName == entry.workspaceName; });
                if (old != previous.end()) {
                    entry.windowLayout = std::move(old->windowLayout);
                    entry.labelSize    = old->labelSize;
                }
                updateWindowLayout(entry);
            }
            computeLayout();
            if (selected)
                selectTarget(*selected);
        }
    }

    void COverview::updateWindowLayout(SEntry& entry) {
        ++m_layoutCounters.windowUpdates;
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
            const size_t columns = session().gestureActive() ? entry.previewColumns : 0;
            // Cache only geometry. Window identities, desktop bounds and draw
            // order are refreshed above, including during closing animations.
            if (!entry.windowLayout || entry.windowLayout->input != input || entry.windowLayout->usable != m_usable || entry.windowLayout->fixedColumns != columns) {
                ++m_layoutCounters.windowLayouts;
                if (input.size() > 1 && std::ranges::any_of(input, [](const auto& window) { return window.fullscreen; }))
                    ++m_layoutCounters.spreadLayouts;
                auto result        = layoutOverviewWindows(input, m_usable, columns);
                entry.windowLayout = SWindowLayoutState{std::move(input), m_usable, columns, std::move(result)};
            }
            const auto& LAYOUT   = entry.windowLayout->result;
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
        // Tile keys are indices into the ordered entries. Empty membership must
        // invalidate both the keys and the cached geometry.
        m_animationAnchor  = -1;
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || m_entries.empty()) {
            m_tiles.clear();
            m_tileLayoutKey.reset();
            m_selected = m_hovered = -1;
            m_clickedWindow.reset();
            m_hoveredWindow.reset();
            return;
        }

        const STileLayoutKey key{m_usable, m_entries.size(), config::overviewPadding(), config::overviewGap(), config::overviewShowLabels()};
        if (!m_tileLayoutKey || *m_tileLayoutKey != key) {
            std::vector<STileInput> input;
            input.reserve(m_entries.size());
            for (size_t i = 0; i < m_entries.size(); ++i)
                input.push_back(STileInput{.key = i, .workspaceId = m_entries[i].workspaceId});

            SLayoutParams params;
            params.screenW = m_usable.w;
            // The toolbar fits inside the usual outer padding. Reserve only
            // the missing space for tighter grids, retaining their aspect and
            // the established single-workspace zoom fit at default settings.
            const double toolbar = std::max(0.0, workspaceToolbarHeight() - static_cast<double>(key.padding));
            params.screenH       = m_usable.h - toolbar;
            params.padding       = key.padding;
            params.gap           = key.gap;
            params.aspect        = m_usable.h > 0 ? m_usable.w / m_usable.h : 16.0 / 9.0;
            params.labelSpace    = key.labels ? 34.0 : 0.0;

            ++m_layoutCounters.gridLayouts;
            auto result = layout(input, params);
            m_tiles     = std::move(result.tiles);
            for (auto& tile : m_tiles) {
                tile.box.x += m_usable.x;
                tile.box.y += m_usable.y + toolbar;
            }
            m_tileLayoutKey = key;
        }
        m_selected = m_tiles.empty() ? -1 : std::clamp(m_selected, 0, static_cast<int>(m_tiles.size()) - 1);

        // Workspace identities and the animation anchor can change even when
        // the ordered grid's count and dimensions remain the same.
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
        return interpolateBox(e.start, e.target, m_progress->value());
    }

    float COverview::overviewProgress() const {
        const float remaining = previewUnit(m_progress->value());
        return m_closing ? m_closeStartProgress * remaining : remaining;
    }

    SOverviewCamera COverview::rawZoomCamera() const {
        const auto& offset = m_zoomOffset->value();
        return composeOverviewCamera({m_zoomScale->value(), offset.x, offset.y}, m_inspection.current(m_inspectionProgress->value()));
    }

    SOverviewCamera COverview::camera() const {
        return overviewCameraAt(rawZoomCamera(), overviewProgress());
    }

    SBoxF COverview::displayedCell(const SEntry& e) const {
        if (m_closing && e.closeCell)
            return interpolateBox(e.start, *e.closeCell, m_progress->value());
        return camera().apply(interpolate(e));
    }

    SBoxF COverview::workspaceCell(const SWorkspaceIdentity& workspace) const {
        const auto entry = std::ranges::find_if(m_entries, [&](const auto& e) { return workspace == SWorkspaceIdentity{e.workspaceId, e.workspaceName}; });
        return entry == m_entries.end() ? SBoxF{} : displayedCell(*entry);
    }

    bool COverview::zoomTo(const SWorkspaceIdentity& workspace) {
        if (m_closing)
            return false;
        const auto entry = std::ranges::find_if(m_entries, [&](const auto& e) { return workspace == SWorkspaceIdentity{e.workspaceId, e.workspaceName}; });
        if (entry == m_entries.end() || entry->target.w <= 0 || entry->target.h <= 0)
            return false;
        auto fitted = overviewZoomCamera(entry->target, m_usable, config::overviewPadding(), config::overviewShowLabels() ? 34.0 : 0.0);
        // The baseline's scalar animation stores a float; use its exact goal
        // for inspection bounds and identity restoration as well.
        fitted.scale = static_cast<float>(fitted.scale);
        const bool changed =
            !m_zoomFit || m_zoomFit->workspace != workspace || m_zoomFit->camera != fitted || m_zoomFit->cell != entry->target || m_zoomFit->usable != m_usable;
        if (changed || !config::overviewWheelZoom() || !m_inspection.goalFits(fitted.apply(entry->target))) {
            session().cancelPan();
            foldInspection();
        }
        if (changed)
            m_zoomFit = SZoomFit{workspace, fitted, entry->target, m_usable};
        const auto scale  = static_cast<float>(fitted.scale);
        const auto offset = Vector2D{fitted.x, fitted.y};
        // Called during live geometry refresh as well as keyboard navigation.
        // Restarting an unchanged goal here would keep the camera in motion.
        if (m_zoomScale->goal() != scale)
            *m_zoomScale = scale;
        if (m_zoomOffset->goal() != offset)
            *m_zoomOffset = offset;
        return true;
    }

    void COverview::releaseZoom() {
        if (m_closing)
            return;
        foldInspection();
        m_zoomFit.reset();
        if (m_zoomScale->goal() != 1.F)
            *m_zoomScale = 1.F;
        if (m_zoomOffset->goal() != Vector2D{})
            *m_zoomOffset = Vector2D{};
    }

    bool COverview::zoomTransitioning() const {
        return !m_closing && (m_zoomScale->isBeingAnimated() || m_zoomOffset->isBeingAnimated() || inspectionTransitioning());
    }

    bool COverview::zoomNavigationReady() const {
        return !m_closing && !m_progress->isBeingAnimated() && m_progress->value() >= 0.99F && !zoomTransitioning();
    }

    bool COverview::inspectionReady() const {
        return !m_closing && m_zoomFit && config::overviewWheelZoom() && !session().gestureActive();
    }

    bool COverview::inspectionActive() const {
        return m_inspection.goal() != SOverviewCamera{} || m_inspection.current(m_inspectionProgress->value()) != SOverviewCamera{};
    }

    bool COverview::inspectionTransitioning() const {
        return !m_closing && m_inspectionProgress->isBeingAnimated();
    }

    double COverview::inspectionFactor() const {
        return std::clamp(m_inspection.current(m_inspectionProgress->value()).scale, 1.0, 4.0);
    }

    double COverview::inspectionGoal() const {
        return std::clamp(m_inspection.goal().scale, 1.0, 4.0);
    }

    bool COverview::inspectionPanHit(const Vector2D& globalPos) const {
        if (!inspectionReady())
            return false;
        const auto mon = monitor();
        if (!mon || !SBoxF{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y}.contains(globalPos.x, globalPos.y))
            return false;
        const auto local = globalPos - mon->m_position;
        return m_usable.contains(local.x, local.y) && workspaceCell(m_zoomFit->workspace).contains(local.x, local.y);
    }

    bool COverview::inspectionPanAvailable(const Vector2D& globalPos) const {
        // Opening morphs and unfinished base fits still own their camera.
        // A wheel lens can be grabbed before its animation has settled.
        return inspectionPanHit(globalPos) && !m_progress->isBeingAnimated() && !m_zoomScale->isBeingAnimated() && !m_zoomOffset->isBeingAnimated() &&
               m_inspection.canPan(m_zoomFit->camera.apply(m_zoomFit->cell), m_inspectionProgress->value());
    }

    bool COverview::beginInspectionPan(const Vector2D& globalPos) {
        if (!inspectionPanAvailable(globalPos) || !m_inspection.beginPan(m_zoomFit->camera.apply(m_zoomFit->cell), m_inspectionProgress->value()))
            return false;
        m_inspectionProgress->setValueAndWarp(1.F);
        damage();
        return true;
    }

    void COverview::panInspection(const Vector2D& delta) {
        if (inspectionReady() && m_inspection.pan(m_zoomFit->camera.apply(m_zoomFit->cell), delta.x, delta.y))
            damage();
    }

    bool COverview::inspectScroll(const SScrollInput& event, const Vector2D& globalPos) {
        if ((!event.wheel && !event.finger) || event.horizontal || event.wheelTilt || !std::isfinite(event.delta))
            return false;
        const double detents = event.wheel && event.value120 != 0 ? event.value120 / 120.0 : event.delta / 15.0;
        return inspectZoom(detents, hooks::scrollFactor(), globalPos);
    }

    void COverview::panInspectionKey(const Vector2D& delta) {
        const auto mon = monitor();
        if (mon && beginInspectionPan(mon->m_position + Vector2D{m_usable.cx(), m_usable.cy()}))
            panInspection(delta);
    }

    bool COverview::inspectZoom(double detents, double factor, std::optional<Vector2D> globalPos) {
        if (!inspectionReady())
            return false;
        const auto mon = monitor();
        if (!mon || (globalPos && !SBoxF{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y}.contains(globalPos->x, globalPos->y)))
            return false;
        const auto local     = globalPos ? *globalPos - mon->m_position : Vector2D{m_usable.cx(), m_usable.cy()};
        const auto footprint = m_zoomFit->camera.apply(m_zoomFit->cell);
        const auto displayed = workspaceCell(m_zoomFit->workspace);
        if (!m_usable.contains(local.x, local.y) || (globalPos && !displayed.contains(local.x, local.y)))
            return false;
        m_scroll.reset();
        auto       prospective = m_inspection;
        double     progress    = m_inspectionProgress->value();
        const bool fitting     = m_zoomScale->isBeingAnimated() || m_zoomOffset->isBeingAnimated();
        if (fitting) {
            if (!prospective.beginFitTransition(rawZoomCamera(), m_zoomFit->camera))
                return false;
            progress = 0;
        }
        if (!prospective.zoom(detents, factor, footprint, local.x, local.y, progress, displayed))
            return false;
        if (fitting) {
            // Rebase before overview progress, preserving every displayed cell
            // at takeover. The lens now owns the fit and extra zoom together.
            m_zoomScale->setValueAndWarp(static_cast<float>(m_zoomFit->camera.scale));
            m_zoomOffset->setValueAndWarp({m_zoomFit->camera.x, m_zoomFit->camera.y});
        }
        m_inspection = prospective;
        m_inspectionProgress->setValueAndWarp(0.F);
        *m_inspectionProgress = 1.F;
        damage();
        return true;
    }

    void COverview::foldInspection() {
        if (inspectionActive()) {
            // Snapshot both layers before clearing either one. The composition
            // is folded before overview progress to preserve continuity.
            const auto camera = rawZoomCamera();
            m_zoomScale->setValueAndWarp(static_cast<float>(camera.scale));
            m_zoomOffset->setValueAndWarp({camera.x, camera.y});
        }
        // A curve can pass above 1 and then return below it. Even when the
        // clamped lens currently looks like identity, discard its old start.
        m_inspection.reset();
        if (m_inspectionProgress->isBeingAnimated() || m_inspectionProgress->value() != 1.F || m_inspectionProgress->goal() != 1.F)
            m_inspectionProgress->setValueAndWarp(1.F);
    }

    std::optional<EDirection> COverview::zoomEdgeAt(const Vector2D& globalPos) const {
        const auto mon = monitor();
        if (!mon)
            return std::nullopt;
        const auto local = globalPos - mon->m_position;
        if (scrollControlAt(local))
            return std::nullopt;
        return overviewZoomEdgeAt(m_usable, local.x, local.y);
    }

    std::optional<SWorkspaceIdentity> COverview::zoomNeighbor(const SWorkspaceIdentity& workspace, EDirection direction) const {
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            const auto& source = m_entries[m_tiles[i].key];
            if (workspace != SWorkspaceIdentity{source.workspaceId, source.workspaceName})
                continue;
            const int next = navigate(m_tiles, static_cast<int>(i), direction);
            if (next < 0 || next == static_cast<int>(i))
                return std::nullopt;
            const auto& destination = m_entries[m_tiles[next].key];
            return SWorkspaceIdentity{destination.workspaceId, destination.workspaceName};
        }
        return std::nullopt;
    }

    std::vector<COverview::SZoomEdgeHint> COverview::zoomEdgeHints() const {
        std::vector<SZoomEdgeHint> result;
        if (!session().zoomEdgeReady(*this))
            return result;
        std::vector<SBoxF> obstacles;
        for (const auto& entry : m_entries) {
            const auto cell = displayedCell(entry);
            if (auto state = scrollingFor(entry)) {
                const auto controls = scrollControls(cell, state->horizontal);
                if (state->previous)
                    obstacles.push_back(controls.previous);
                if (state->next)
                    obstacles.push_back(controls.next);
            }
            if (config::overviewShowLabels() && entry.labelSize)
                obstacles.push_back(overviewWorkspaceLabelBox(cell, entry.labelSize->x, entry.labelSize->y));
        }
        const auto source = session().zoomTarget();
        for (auto direction : {EDirection::LEFT, EDirection::RIGHT, EDirection::UP, EDirection::DOWN}) {
            const auto neighbor = zoomNeighbor(source->workspace, direction);
            const auto box      = overviewZoomHintBox(m_usable, direction, obstacles);
            if (neighbor && box)
                result.push_back({direction, *neighbor, *box, session().zoomEdgePending() == direction});
        }
        return result;
    }

    void COverview::freezeZoom() {
        if (m_closing)
            return;
        m_zoomScale->setValueAndWarp(m_zoomScale->value());
        m_zoomOffset->setValueAndWarp(m_zoomOffset->value());
        m_inspection.freeze(m_inspectionProgress->value());
        m_inspectionProgress->setValueAndWarp(1.F);
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
        const SBoxF CLIP   = slot.fullscreen != Fullscreen::FSMODE_NONE ? SBoxF{0, 0, MONITOR->m_size.x, MONITOR->m_size.y} : m_usable;
        if (m_closing && slot.closeGeometry) {
            const SWindowPreviewGeometry endpoint =
                ANCHOR ? SWindowPreviewGeometry{slot.desktopRect, CLIP} : SWindowPreviewGeometry{windowBoxInCell(slot.previewRect, entry.start), entry.start};
            return closingWindowPreviewGeometry(*slot.closeGeometry, endpoint, m_progress->value());
        }
        const auto CELL = ANCHOR ? entry.target : interpolate(entry);
        const auto raw  = overviewWindowGeometry({windowBoxInCell(slot.previewRect, CELL), CELL}, {slot.desktopRect, CLIP}, ANCHOR, m_progress->value());
        const auto view = camera();
        return {view.apply(raw.box), view.apply(raw.clip)};
    }

    SWorkspacePreviewStyle COverview::styleFor(const SEntry& entry, bool selected) const {
        const bool anchor = m_animationAnchor >= 0 && &entry == &m_entries[m_animationAnchor];
        if (m_closing)
            return closingWorkspacePreviewStyle(entry.closeStyle, anchor, m_progress->value());
        return workspacePreviewStyle(anchor, selected, overviewProgress());
    }

    float COverview::visibilityFor(const SEntry& entry, const SWindowSlot& slot) const {
        const auto window = slot.window.lock();
        if (!window)
            return 0.F;
        const bool anchor  = m_animationAnchor >= 0 && &entry == &m_entries[m_animationAnchor];
        const auto desktop = window->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
        if (m_closing)
            return closingWindowPreviewVisibility(slot.closeVisibility, desktop, anchor, m_progress->value());
        const bool selected = m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size()) && &entry == &m_entries[m_tiles[m_selected].key];
        return styleFor(entry, selected).windowVisibility * overviewWindowVisibility(desktop, anchor, overviewProgress());
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

        if (commitSelection)
            session().keyboard(*this);

        // Re-anchoring while an opening or inspection zoom is in flight must
        // start from exactly the displayed frame, including old anchor opacity.
        m_closeStartProgress = overviewProgress();
        for (auto& entry : m_entries) {
            const bool selected = m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size()) && &entry == &m_entries[m_tiles[m_selected].key];
            entry.closeCell     = displayedCell(entry);
            entry.closeStyle    = styleFor(entry, selected);
            for (auto& slot : entry.windows) {
                slot.closeGeometry   = geometryFor(entry, slot);
                slot.closeVisibility = visibilityFor(entry, slot);
            }
        }
        freezeZoom();
        m_closing = true;

        if (commitSelection) {
            // Re-anchor before committing, while the selection is still intact,
            // so the closing zoom runs into the workspace being switched to.
            // A missing local tile fades into an empty destination. A request
            // on an uncovered output leaves this output's original anchor intact.
            const int  ANCHOR            = committedEntry();
            const auto REQUESTED         = m_gotoWorkspace > 0 ? State::workspaceState()->query().id(m_gotoWorkspace).run() : nullptr;
            const auto REQUESTED_MONITOR = REQUESTED
                                               ? REQUESTED->m_monitor.lock()
                                               : (m_gotoWorkspace > 0 ? Config::workspaceRuleMgr()->getBoundMonitorForWS(std::to_string(m_gotoWorkspace)) : nullptr);
            const bool SWITCHING         = m_gotoWorkspace > 0 && (!REQUESTED_MONITOR || REQUESTED_MONITOR == m_monitor);

            if (ANCHOR >= 0 || SWITCHING)
                anchorAnimation(ANCHOR);

            commit();
        }

        // Closing geometry uses snapshots and its own normalized remaining
        // fraction; visual styling retains the original overview progress.
        m_progress->setValueAndWarp(1.F);
        *m_progress = 0.F;
        damage();
    }

    // Dragging the last window off a workspace lets Hyprland reap it, so the
    // tile can outlive the workspace it stands for. Recreate it on demand rather
    // than letting the tile go dead.
    PHLWORKSPACE COverview::workspaceForEntry(const SEntry& e) const {
        return session().workspace({.workspace = {e.workspaceId, e.workspaceName}, .monitor = m_monitor});
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
            // Resolve the workspace itself so native focus cannot reinterpret
            // this exact choice through workspace_back_and_forth. Existing
            // workspaces keep their owning output; a fresh empty workspace
            // belongs to the output whose overview asked for it, respecting
            // native workspace monitor bindings.
            const auto WS = State::workspaceState()->query().id(m_gotoWorkspace).run();

            if (WS)
                (void)Config::Actions::changeWorkspace(WS);
            else if (const auto FRESH = State::workspaceState()->create(m_gotoWorkspace, MONITOR->m_id))
                (void)Config::Actions::changeWorkspace(FRESH);

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
            if (const auto W = entry.windows.front().window.lock()) {
                focusSelection(W, config::warpCursor());
                return;
            }
        }

        // An empty workspace has no window whose focus establishes its output.
        // Keep native monitor and null-window focus for cross-output selection,
        // including an empty workspace already active on that output.
        if (WS->m_isSpecialWorkspace) {
            // The native special-workspace action uses current focus rather
            // than the workspace owner and would move it back to the source.
            Desktop::focusState()->rawMonitorFocus(MONITOR);
            Desktop::focusState()->rawWindowFocus(nullptr, Desktop::FOCUS_REASON_KEYBIND);
        } else
            (void)Config::Actions::changeWorkspace(WS);
    }

    // ---------------------------------------------------------------- input --

    bool COverview::onKey(xkb_keysym_t sym, uint32_t mods, bool pressed) {
        if (!pressed)
            return true; // swallow releases too while we hold the grab

        // The carried workspace remains the command target until release.
        // Native shortcuts are routed before this overview key handler.
        if (session().workspaceDrag && sym != XKB_KEY_Escape)
            return true;

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
            if (session().gestureActive())
                session().cancelDrag();
            else
                close(false);
            return true;

        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_s:
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

        case XKB_KEY_Page_Up:
        case XKB_KEY_Page_Down:
            if (m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size()) && scrollingFor(m_entries[m_tiles[m_selected].key]))
                if (auto target = selectedTarget())
                    hooks::stepWorkspace(*target, sym == XKB_KEY_Page_Up ? -1 : 1, true);
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

            COverview* destination = this;
            bool       foundTile   = false;
            for (const auto& view : session().views) {
                if (view->closing())
                    continue;
                for (size_t i = 0; i < view->m_tiles.size(); ++i) {
                    if (view->m_entries[view->m_tiles[i].key].workspaceId != WANT)
                        continue;
                    destination = view.get();
                    destination->selectIndex(static_cast<int>(i));
                    foundTile = true;
                    break;
                }
                if (foundTile)
                    break;
            }

            // An inactive empty workspace has no tile, but its covered output
            // still owns the transition. Native monitor rules can also bind a
            // workspace before it exists. Fade that grid into its empty desktop.
            if (!foundTile) {
                const auto ws  = State::workspaceState()->query().id(WANT).run();
                const auto mon = ws ? ws->m_monitor.lock() : Config::workspaceRuleMgr()->getBoundMonitorForWS(std::to_string(WANT));
                if (mon)
                    for (const auto& view : session().views)
                        if (!view->closing() && view->m_monitor == mon) {
                            destination = view.get();
                            break;
                        }
            }

            destination->m_clickedWindow.reset();
            destination->m_gotoWorkspace = foundTile ? 0 : WANT;
            destination->close(true);
            // The key matcher observes the view where this key arrived. Close
            // that source too so it releases the grab and dismisses other views.
            if (destination != this)
                close(false);
            return true;
        }

        // Everything else is swallowed: the overview owns the keyboard, and there
        // is deliberately no text entry anywhere in hyprspace.
        return true;
    }

    int COverview::tileAtLocal(const Vector2D& local) const {
        for (int i = static_cast<int>(m_tiles.size()) - 1; i >= 0; --i) {
            const auto& entry = m_entries[m_tiles[i].key];
            if (displayedCell(entry).contains(local.x, local.y))
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

        if (scrollControlAt(local))
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
        if (emptyWorkspaceButtonHit(globalPos))
            return std::nullopt;
        const auto local = globalPos - mon->m_position;
        const int  index = tileAtLocal(local);
        if (index < 0)
            return std::nullopt;
        const auto&     entry = m_entries[m_tiles[index].key];
        SOverviewTarget target;
        target.workspace   = {entry.workspaceId, entry.workspaceName};
        target.monitor     = mon;
        target.preview     = displayedCell(entry);
        target.previewClip = target.preview;
        target.desktopBox  = m_usable;
        target.window      = windowAtLocal(local);
        if (auto w = target.window.lock()) {
            for (const auto& slot : entry.windows) {
                if (slot.window != w)
                    continue;
                const auto geometry = geometryFor(entry, slot);
                target.preview      = geometry.box;
                target.previewClip  = geometry.clip;
                target.desktopBox   = slot.desktopRect;
                break;
            }
        }
        const auto point = mapPreviewPoint({local.x, local.y}, target.preview, target.desktopBox);
        if (!point)
            return std::nullopt;
        target.desktop = mon->m_position + Vector2D{point->x, point->y};
        target.preview.x += mon->m_position.x;
        target.preview.y += mon->m_position.y;
        target.previewClip.x += mon->m_position.x;
        target.previewClip.y += mon->m_position.y;
        target.desktopBox.x += mon->m_position.x;
        target.desktopBox.y += mon->m_position.y;
        target.monitorBox = m_usable;
        target.monitorBox.x += mon->m_position.x;
        target.monitorBox.y += mon->m_position.y;
        return target;
    }

    std::optional<SOverviewTarget> COverview::workspaceTargetAt(const Vector2D& globalPos) const {
        const auto mon = monitor();
        if (!mon || m_closing || !SBoxF{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y}.contains(globalPos.x, globalPos.y))
            return std::nullopt;
        const auto local = globalPos - mon->m_position;
        for (const auto& tile : m_tiles | std::views::reverse) {
            const auto& entry = m_entries[tile.key];
            const auto  cell  = displayedCell(entry);
            if (!cell.contains(local.x, local.y))
                continue;
            const auto point = mapPreviewPoint({local.x, local.y}, cell, m_usable);
            if (!point)
                return std::nullopt;
            SOverviewTarget target{.workspace   = {entry.workspaceId, entry.workspaceName},
                                   .monitor     = mon,
                                   .desktop     = mon->m_position + Vector2D{point->x, point->y},
                                   .preview     = cell,
                                   .desktopBox  = m_usable,
                                   .monitorBox  = m_usable,
                                   .previewClip = cell};
            for (auto* box : {&target.preview, &target.desktopBox, &target.monitorBox, &target.previewClip}) {
                box->x += mon->m_position.x;
                box->y += mon->m_position.y;
            }
            return target;
        }
        return std::nullopt;
    }

    std::optional<SWorkspaceDragPreview> COverview::workspacePreview(const SWorkspaceIdentity& workspace) const {
        const auto mon = monitor();
        if (!mon || m_closing)
            return std::nullopt;
        const auto entry = std::ranges::find_if(m_entries, [&](const auto& e) { return workspace == SWorkspaceIdentity{e.workspaceId, e.workspaceName}; });
        if (entry == m_entries.end())
            return std::nullopt;
        SWorkspaceDragPreview preview{.cell = displayedCell(*entry), .label = entry->name};
        preview.cell.x += mon->m_position.x;
        preview.cell.y += mon->m_position.y;
        for (const auto index : entry->drawOrder) {
            const auto& slot   = entry->windows[index];
            const auto  window = slot.window.lock();
            if (!window || !window->m_isMapped || window->isHidden())
                continue;
            auto geometry = geometryFor(*entry, slot);
            for (auto* box : {&geometry.box, &geometry.clip}) {
                box->x += mon->m_position.x;
                box->y += mon->m_position.y;
            }
            preview.windows.push_back({.window     = window,
                                       .box        = geometry.box,
                                       .clip       = geometry.clip,
                                       .visibility = visibilityFor(*entry, slot),
                                       .fullscreen = slot.fullscreen != Fullscreen::FSMODE_NONE,
                                       .blur       = slot.blur,
                                       .texture    = m_capture.textureFor(window)});
        }
        return preview;
    }

    std::optional<SBoxF> COverview::workspaceDropArea() const {
        const auto mon = monitor();
        if (!mon || m_closing)
            return std::nullopt;
        return SBoxF{mon->m_position.x + m_usable.x, mon->m_position.y + m_usable.y, m_usable.w, m_usable.h};
    }

    void COverview::refreshWorkspaceLayout() {
        refreshWindows();
        damage();
    }

    std::optional<SOverviewTarget> COverview::selectedTarget() const {
        const auto mon = monitor();
        if (!mon || m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return std::nullopt;
        const auto&     entry = m_entries[m_tiles[m_selected].key];
        SOverviewTarget target{.workspace = {entry.workspaceId, entry.workspaceName}, .monitor = mon};
        target.preview     = displayedCell(entry);
        target.previewClip = target.preview;
        target.desktopBox  = m_usable;
        target.monitorBox  = m_usable;
        if (auto w = m_clickedWindow.lock(); w && w->m_isMapped && !w->isHidden() && w->m_workspace && w->m_workspace->m_id == entry.workspaceId) {
            for (const auto& slot : entry.windows)
                if (slot.window == w) {
                    target.window       = w;
                    const auto geometry = geometryFor(entry, slot);
                    target.preview      = geometry.box;
                    target.previewClip  = geometry.clip;
                    target.desktopBox   = slot.desktopRect;
                    break;
                }
        }
        target.desktop = mon->m_position + Vector2D{target.desktopBox.cx(), target.desktopBox.cy()};
        for (auto* box : {&target.preview, &target.desktopBox, &target.monitorBox, &target.previewClip}) {
            box->x += mon->m_position.x;
            box->y += mon->m_position.y;
        }
        return target;
    }

    std::optional<SScrollViewport> COverview::scrollingFor(const SEntry& entry) const {
        if (entry.spread || m_closing || session().gestureActive())
            return std::nullopt;
        return hooks::scrollingViewport({.workspace = {entry.workspaceId, entry.workspaceName}, .monitor = m_monitor});
    }

    std::optional<std::pair<int, int>> COverview::scrollControlAt(const Vector2D& local) const {
        if (m_closing || session().gestureActive() || m_progress->value() < 0.95F)
            return std::nullopt;
        for (int i = static_cast<int>(m_tiles.size()) - 1; i >= 0; --i) {
            const auto& entry = m_entries[m_tiles[i].key];
            const auto  state = scrollingFor(entry);
            if (!state)
                continue;
            const auto controls = scrollControls(displayedCell(entry), state->horizontal);
            if (state->previous && controls.previous.contains(local.x, local.y))
                return std::pair{i, -1};
            if (state->next && controls.next.contains(local.x, local.y))
                return std::pair{i, 1};
        }
        return std::nullopt;
    }

    std::vector<SOverviewTarget> COverview::inspectTargets() const {
        std::vector<SOverviewTarget> result;
        const auto                   mon = monitor();
        if (!mon)
            return result;
        for (const auto& entry : m_entries) {
            auto cell = displayedCell(entry);
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

    bool COverview::refreshPreparedWorkspace(PHLWORKSPACE workspace, PHLWINDOW window) {
        if (!valid(workspace) || workspace->m_monitor != m_monitor || m_closing)
            return false;
        refreshWindows();
        selectTarget({.workspace = {workspace->m_id, workspace->m_name}, .monitor = m_monitor, .window = window});
        const auto target = selectedTarget();
        if (!target || target->workspace != SWorkspaceIdentity{workspace->m_id, workspace->m_name})
            return false;
        session().selection.keyboard(*target);
        damage();
        return true;
    }

    double COverview::workspaceToolbarHeight() const {
        return std::min(52.0, m_usable.h / 4.0);
    }

    const COverview::SEmptyWorkspaceButtonText& COverview::emptyWorkspaceButtonText(double scale) const {
        const auto  font  = config::overviewFont();
        const auto  label = m_emptyWorkspaceError.empty() ? "Empty workspace" : "Workspace unavailable";
        std::string keyLabel;
        const auto  key = config::overviewEmptyWorkspaceKey();
        if (key != XKB_KEY_NoSymbol && m_emptyWorkspaceError.empty()) {
            char name[128]{};
            if (xkb_keysym_get_name(key, name, sizeof(name)) > 0) {
                keyLabel = name;
                if (keyLabel.size() == 1)
                    keyLabel[0] = std::toupper(static_cast<unsigned char>(keyLabel[0]));
            }
        }
        if (!m_emptyWorkspaceButtonText || m_emptyWorkspaceButtonText->font != font || m_emptyWorkspaceButtonText->label != label ||
            m_emptyWorkspaceButtonText->key != keyLabel || m_emptyWorkspaceButtonText->scale != scale) {
            // Input and rendering share measured bounds; only remeasure when
            // the font, shortcut, error state or output scale changes.
            int width = 0, height = 0;
            measureText(label, font, width, height, scale * EMPTY_WORKSPACE_TEXT_SCALE);
            const double titleWidth = std::ceil(width / scale);
            double       keyWidth   = 0;
            if (!keyLabel.empty()) {
                measureText(keyLabel, font, width, height, scale * EMPTY_WORKSPACE_KEY_SCALE);
                keyWidth = std::max(20.0, std::min(std::ceil(width / scale), 64.0 * EMPTY_WORKSPACE_KEY_SCALE) + 10);
            }
            m_emptyWorkspaceButtonText = SEmptyWorkspaceButtonText{font, label, std::move(keyLabel), scale, titleWidth, keyWidth};
        }
        return *m_emptyWorkspaceButtonText;
    }

    std::optional<SBoxF> COverview::emptyWorkspaceButton() const {
        const auto  mon  = monitor();
        const auto& drag = session().drag;
        if (!mon || m_closing || session().workspaceDrag || session().pendingResize || drag.mode == SOverviewDrag::RESIZE ||
            (session().zoomLocked() && drag.mode != SOverviewDrag::MOVE))
            return std::nullopt;
        const double height       = workspaceToolbarHeight();
        const double inset        = std::min(8.0, height / 4.0);
        const double margin       = std::min(16.0, m_usable.w / 8.0);
        const auto&  text         = emptyWorkspaceButtonText(mon->m_scale);
        const double titleInset   = m_emptyWorkspaceError.empty() ? 30 : 12;
        const double width        = std::min(titleInset + text.titleWidth + (text.keyWidth > 0 ? text.keyWidth + 12 : 0) + 12, m_usable.w - margin * 2.0);
        const double buttonHeight = std::min(32.0, height - inset * 2.0);
        if (width < 24 || buttonHeight < 16)
            return std::nullopt;
        return SBoxF{mon->m_position.x + m_usable.x + m_usable.w - margin - width, mon->m_position.y + m_usable.y + inset, width, buttonHeight};
    }

    bool COverview::emptyWorkspaceButtonHit(const Vector2D& globalPos) const {
        const auto box = emptyWorkspaceButton();
        return box && box->contains(globalPos.x, globalPos.y);
    }

    void COverview::setEmptyWorkspaceError(std::string error) {
        m_emptyWorkspaceError = std::move(error);
        damage();
    }

    void COverview::onMouseMove(const Vector2D& globalPos) {
        if (session().workspaceDrag)
            return;
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const auto LOCAL = globalPos - MONITOR->m_position;

        const bool EMPTY_BUTTON = emptyWorkspaceButtonHit(globalPos);
        const int  IDX          = EMPTY_BUTTON ? -1 : tileAtLocal(LOCAL);
        const auto WINDOW       = EMPTY_BUTTON ? nullptr : windowAtLocal(LOCAL);

        if (IDX == m_hovered && WINDOW == m_hoveredWindow.lock())
            return;

        m_hovered       = IDX;
        m_hoveredWindow = WINDOW;

        if (IDX >= 0 && session().selection.followsPointer())
            m_scroll.reset();

        damage();
    }

    // Pan scrolling workspaces in place; other layouts keep tile navigation.
    void COverview::onScroll(const SScrollInput& event) {
        if (m_tiles.empty() || m_closing || session().gestureActive())
            return;

        const auto mon = monitor();
        if (!mon)
            return;
        const auto pos = g_pInputManager->getMouseCoordsInternal();
        if (emptyWorkspaceButtonHit(pos))
            return;
        const auto local = pos - mon->m_position;
        const int  index = tileAtLocal(local);
        if (index >= 0) {
            const auto& entry = m_entries[m_tiles[index].key];
            if (auto state = scrollingFor(entry)) {
                session().pointer(pos);
                const auto target = targetAt(pos);
                const auto cell   = displayedCell(entry);
                if (target)
                    hooks::panWorkspace(*target, scrollDistance(event, state->horizontal ? cell.w : cell.h) * hooks::scrollFactor());
                return;
            }
        }
        if (session().zoomLocked())
            return;
        if (event.horizontal)
            return;
        const int STEPS = m_scroll.steps(event);
        if (STEPS == 0)
            return;

        m_clickedWindow.reset();
        const int N = static_cast<int>(m_tiles.size());
        selectIndex(static_cast<int>(((static_cast<int64_t>(m_selected) + STEPS) % N + N) % N));
        session().keyboard(*this);
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
            if (const auto control = scrollControlAt(LOCAL)) {
                selectIndex(control->first);
                if (auto target = selectedTarget())
                    hooks::stepWorkspace(*target, control->second, false);
                return true;
            }
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
        ++m_layoutCounters.frames;
        session().validateWorkspaceDrag();
        session().validateWorkspaceSettle();

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
            if (m_closing)
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

        // Carried workspace windows reuse their source captures on every
        // covered output. Their blur still needs that output's backdrop.
        if (const auto& drag = session().workspaceDrag; drag && drag->moved) {
            const double dx = drag->box.x - drag->preview.cell.x, dy = drag->box.y - drag->preview.cell.y;
            for (const auto& slot : drag->preview.windows) {
                const auto window = slot.window.lock();
                if (!window || !window->m_isMapped || window->isHidden() || window->m_workspace != drag->workspace)
                    continue;
                const double left = std::max(slot.box.x, slot.clip.x) + dx, top = std::max(slot.box.y, slot.clip.y) + dy;
                const double right  = std::min(slot.box.x + slot.box.w, slot.clip.x + slot.clip.w) + dx;
                const double bottom = std::min(slot.box.y + slot.box.h, slot.clip.y + slot.clip.h) + dy;
                if (right > left && bottom > top && right > MONITOR->m_position.x && bottom > MONITOR->m_position.y && left < MONITOR->m_position.x + MONITOR->m_size.x &&
                    top < MONITOR->m_position.y + MONITOR->m_size.y)
                    m_needsBlur |= hidden::shouldBlurWindow(window);
            }
        }
        if (const auto preview = session().workspaceSettlePreview()) {
            for (const auto& slot : preview->windows) {
                const auto window = slot.window.lock();
                if (!window || !window->m_isMapped || window->isHidden())
                    continue;
                const double left = std::max(slot.box.x, slot.clip.x), top = std::max(slot.box.y, slot.clip.y);
                const double right  = std::min(slot.box.x + slot.box.w, slot.clip.x + slot.clip.w);
                const double bottom = std::min(slot.box.y + slot.box.h, slot.clip.y + slot.clip.h);
                if (right > left && bottom > top && right > MONITOR->m_position.x && bottom > MONITOR->m_position.y && left < MONITOR->m_position.x + MONITOR->m_size.x &&
                    top < MONITOR->m_position.y + MONITOR->m_size.y)
                    m_needsBlur |= hidden::shouldBlurWindow(window);
            }
        }

        damage();
    }

    std::vector<UP<IPassElement>> COverview::buildPass() {
        std::vector<UP<IPassElement>> out;

        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return out;

        const float  PROGRESS = overviewProgress();
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
        const auto&     drag              = session().pendingResize ? *session().pendingResize : session().drag;
        const PHLWINDOW DRAGGED           = (drag.active() && drag.moved) ? drag.window.lock() : nullptr;
        const auto&     workspaceDrag     = session().workspaceDrag;
        const bool      WORKSPACE_CARRIED = workspaceDrag && workspaceDrag->moved;
        const auto      workspaceSettle   = session().workspaceSettlePreview();

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
        // The growing destination covers other previews throughout the close,
        // independent of its numeric position in the grid's draw order.
        const auto   anchor     = std::ranges::find_if(m_tiles, [&](const auto& tile) { return static_cast<int>(tile.key) == m_animationAnchor; });
        const size_t anchorTile = m_closing && anchor != m_tiles.end() ? static_cast<size_t>(anchor - m_tiles.begin()) : m_tiles.size();
        for (size_t order = 0; order < m_tiles.size(); ++order) {
            size_t i = order;
            if (anchorTile < m_tiles.size()) {
                if (order == m_tiles.size() - 1)
                    i = anchorTile;
                else if (order >= anchorTile)
                    ++i;
            }
            auto& entry = m_entries[m_tiles[i].key];

            // The native move is already committed. Only its picture remains
            // lifted until it reaches this tile; input keeps the real layout.
            if (workspaceSettle && session().workspaceSettle && session().workspaceSettle->monitor == MONITOR &&
                session().workspaceSettle->identity == SWorkspaceIdentity{entry.workspaceId, entry.workspaceName})
                continue;

            const SBoxF cell = displayedCell(entry);
            if (cell.w <= 1 || cell.h <= 1)
                continue;

            const bool  SELECTED    = static_cast<int>(i) == m_selected;
            const auto& drop        = session().selection.drop();
            const auto  destination = drag.mode == SOverviewDrag::RESIZE ? drag.source.workspace : (drop ? drop->workspace : SWorkspaceIdentity{});
            const bool  DROP        = DRAGGED && destination == SWorkspaceIdentity{entry.workspaceId, entry.workspaceName};
            const bool  HOVERED     = DROP || (!DRAGGED && !workspaceDrag && static_cast<int>(i) == m_hovered);

            // The zoom's anchor stays visible all the way to the desktop, also
            // when closing into a different workspace from the original one.
            const auto STYLE = styleFor(entry, SELECTED);
            const bool SOURCE =
                WORKSPACE_CARRIED && workspaceDrag->sourceMonitor == MONITOR && workspaceDrag->identity == SWorkspaceIdentity{entry.workspaceId, entry.workspaceName};
            const float  SOURCE_ALPHA = SOURCE ? 0.28F : 1.F;
            const float  FADE         = STYLE.visibility * SOURCE_ALPHA;
            const double round        = ROUNDING * PROGRESS;

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
            rect(cell, TILEBG.modifyA(TILEBG.a * STYLE.plateVisibility * SOURCE_ALPHA), round);

            if (SELECTED && entry.windows.empty() && PROGRESS > 0.35F && session().preparedWorkspace(State::workspaceState()->query().id(entry.workspaceId).run()) &&
                cell.w > 80 && cell.h > 60) {
                if (auto hint = textures().text("Launch an app or drop a window", FONT, config::overviewLabelColor(), static_cast<int>(cell.w - 32), SCALE)) {
                    const auto size = logicalSize(hint);
                    const auto box  = fitBox({cell.cx() - size.x / 2, cell.y + 16, size.x, cell.h - 32}, size.x / size.y);
                    tex(hint, box, FADE * PROGRESS * 0.7F);
                }
            }

            for (const auto index : entry.drawOrder) {
                const auto& slot = entry.windows[index];
                const auto  W    = slot.window.lock();
                if (!W || !W->m_isMapped || W->isHidden() || W == DRAGGED)
                    continue;

                auto t = m_capture.textureFor(W);

                const auto  GEOMETRY = geometryFor(entry, slot);
                const auto& b        = GEOMETRY.box;
                if (b.w < 1 || b.h < 1)
                    continue;

                const float VISIBILITY = visibilityFor(entry, slot) * SOURCE_ALPHA;
                if (t)
                    windowTexture(W, t, b, VISIBILITY, round, slot.blur, GEOMETRY.clip);
                else {
                    const auto&  clip = GEOMETRY.clip;
                    const double left = std::max(b.x, clip.x), top = std::max(b.y, clip.y);
                    const SBoxF  fallback{left, top, std::min(b.x + b.w, clip.x + clip.w) - left, std::min(b.y + b.h, clip.y + clip.h) - top};
                    if (fallback.w > 0 && fallback.h > 0)
                        rect(fallback, config::overviewTitleBgColor().modifyA(VISIBILITY), round);
                }

                const bool   FULLSCREEN = slot.fullscreen != Fullscreen::FSMODE_NONE;
                const auto&  target     = session().selection.command();
                const bool   HOVER      = !session().gestureActive() && target && target->window == W;
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

            if (const auto state = scrollingFor(entry); state && PROGRESS >= 0.95F) {
                const auto controls = scrollControls(cell, state->horizontal);
                const auto pointer  = g_pInputManager->getMouseCoordsInternal() - MONITOR->m_position;
                const auto ink      = config::overviewActiveBorder();
                auto       arrow    = [&](const SBoxF& box, const char* label) {
                    const bool hover = box.contains(pointer.x, pointer.y);
                    rect(box, config::overviewTitleBgColor().modifyA(FADE), box.w / 2);
                    border(box, (hover ? ink : config::overviewTileBorderColor()).modifyA(FADE), hover ? 2 : 1, box.w / 2);
                    if (auto text = textures().text(label, FONT, ink, static_cast<int>(box.w), SCALE)) {
                        const auto size = logicalSize(text);
                        tex(text, {box.cx() - size.x / 2, box.cy() - size.y / 2, size.x, size.y}, FADE);
                    }
                };
                if (state->previous)
                    arrow(controls.previous, state->horizontal ? "←" : "↑");
                if (state->next)
                    arrow(controls.next, state->horizontal ? "→" : "↓");
                if (SELECTED && (state->previous || state->next) && cell.w >= 200 && cell.h >= 130) {
                    if (auto hint = textures().text("PgUp / PgDn · Enter", FONT, config::overviewLabelColor(), static_cast<int>(cell.w - 80), SCALE)) {
                        const auto   size = logicalSize(hint);
                        const double top  = cell.y + (state->horizontal ? 8 : controls.previous.h + 20);
                        const SBoxF  box{cell.cx() - size.x / 2 - 8, top, size.x + 16, size.y + 8};
                        rect(box, config::overviewTitleBgColor().modifyA(FADE), 8);
                        tex(hint, {box.x + 8, box.y + 4, size.x, size.y}, FADE);
                    }
                }
            }

            // --- workspace label ------------------------------------------------
            entry.labelSize.reset();
            if (config::overviewShowLabels() && PROGRESS > 0.35F) {
                const float LABEL_A = (PROGRESS - 0.35F) / 0.65F * FADE;

                const auto LABELCOL = SELECTED ? config::overviewActiveBorder() : config::overviewLabelColor();
                auto       t2       = textures().text(entry.name, FONT, LABELCOL, static_cast<int>(cell.w), SCALE);
                if (!t2)
                    continue;

                const auto SZ    = logicalSize(t2);
                entry.labelSize  = SZ;
                const auto bgBox = overviewWorkspaceLabelBox(cell, SZ.x, SZ.y);

                const auto BGCOL = config::overviewTitleBgColor();
                rect(bgBox, BGCOL.modifyA(BGCOL.a * LABEL_A), bgBox.h / 2.0);
                tex(t2, SBoxF{bgBox.x + (bgBox.w - SZ.x) / 2.0, bgBox.y + 5, SZ.x, SZ.y}, LABEL_A);
            }
        }

        for (const auto& hint : zoomEdgeHints()) {
            const char* arrow = "";
            switch (hint.direction) {
            case EDirection::LEFT:
                arrow = "←";
                break;
            case EDirection::RIGHT:
                arrow = "→";
                break;
            case EDirection::UP:
                arrow = "↑";
                break;
            case EDirection::DOWN:
                arrow = "↓";
                break;
            }
            const auto  ink   = config::overviewActiveBorder();
            const float alpha = hint.pending ? 1.F : 0.65F;
            rect(hint.box, config::overviewTitleBgColor().modifyA(alpha), 8);
            constexpr double GLYPH_SCALE = 1.5;
            if (auto text = textures().text(arrow, FONT, ink, static_cast<int>(hint.box.w / GLYPH_SCALE), SCALE * GLYPH_SCALE)) {
                const auto size = logicalSize(text);
                const auto box  = fitBox({hint.box.x + 4, hint.box.y + 4, hint.box.w - 8, hint.box.h - 8}, size.x / size.y);
                tex(text, box, alpha);
            }
        }

        if (drag.mode == SOverviewDrag::MOVE && drag.moved) {
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
                const double LIFT = drag.mode == SOverviewDrag::RESIZE ? 1.0 : 1.04;

                SBoxF box{
                    drag.box.x - MONITOR->m_position.x - drag.box.w * (LIFT - 1) / 2,
                    drag.box.y - MONITOR->m_position.y - drag.box.h * (LIFT - 1) / 2,
                    drag.box.w * LIFT,
                    drag.box.h * LIFT,
                };

                const auto OUTLINE = config::overviewActiveBorder();
                if (drag.mode == SOverviewDrag::RESIZE) {
                    if (drag.source.monitor != MONITOR)
                        return out;
                    auto clip = drag.source.previewClip;
                    clip.x -= MONITOR->m_position.x;
                    clip.y -= MONITOR->m_position.y;
                    // Input retains the pickup preview and scale. Reproject
                    // only this picture when inspection zoom returns to the
                    // grid, including with a stationary resize pointer.
                    const auto currentCell = workspaceCell(drag.source.workspace);
                    if (currentCell.w > 0 && currentCell.h > 0) {
                        box  = reprojectOverviewBox(box, drag.sourceCell, currentCell);
                        clip = currentCell;
                    }
                    const double x = std::max(box.x, clip.x), y = std::max(box.y, clip.y);
                    const double w = std::min(box.x + box.w, clip.x + clip.w) - x, h = std::min(box.y + box.h, clip.y + clip.h) - y;
                    windowTexture(DRAGGED, t, box, 0.92F, ROUNDING, hidden::shouldBlurWindow(DRAGGED), clip);
                    // Keep the rounded stroke inside the clipped workspace too.
                    const double stroke = std::min(static_cast<double>(BORDER), std::min(w, h) / 2);
                    if (stroke > 0 && w > 2 * stroke && h > 2 * stroke)
                        border({x + stroke, y + stroke, w - 2 * stroke, h - 2 * stroke}, OUTLINE, stroke, std::max(0.0, ROUNDING - stroke));
                } else {
                    border(box, OUTLINE, BORDER, ROUNDING);
                    windowTexture(DRAGGED, t, box, 0.92F, ROUNDING, hidden::shouldBlurWindow(DRAGGED));
                }
            }
        }

        // Keep the destination control visible above a carried preview, even
        // when a drag starts from a magnified workspace.
        if (auto button = emptyWorkspaceButton()) {
            button->x -= MONITOR->m_position.x;
            button->y -= MONITOR->m_position.y;
            const bool   hover    = emptyWorkspaceButtonHit(g_pInputManager->getMouseCoordsInternal());
            const auto   ink      = hover ? config::overviewActiveBorder() : config::overviewLabelColor();
            const auto   bg       = config::overviewTitleBgColor();
            const double rounding = std::min(10.0, button->h / 2.0);
            rect(*button, bg.modifyA(bg.a * PROGRESS * (hover ? 1.F : 0.65F)), rounding);

            const auto& text       = emptyWorkspaceButtonText(SCALE);
            double      keyWidth   = 0;
            auto        keyTexture = text.key.empty() ? nullptr : textures().text(text.key, FONT, ink, 64, SCALE * EMPTY_WORKSPACE_KEY_SCALE);
            if (keyTexture && button->w >= 144) {
                const auto size     = logicalSize(keyTexture);
                keyWidth            = text.keyWidth;
                const double height = std::min(20.0, button->h - 12);
                const SBoxF  badge{button->x + button->w - keyWidth - 12, button->cy() - height / 2, keyWidth, height};
                const auto   textBox = fitBox({badge.x + 4, badge.y + 2, badge.w - 8, badge.h - 4}, size.x / size.y);
                tex(keyTexture, textBox, PROGRESS * 0.65F);
                keyWidth += 12;
            }
            const bool   showPlus   = m_emptyWorkspaceError.empty() && button->w >= 80;
            const double titleInset = showPlus ? 30 : 12;
            if (showPlus) {
                const auto color = ink.modifyA(ink.a * PROGRESS * (hover ? 1.F : 0.7F));
                rect({button->x + 12, button->cy() - 0.75, 10, 1.5}, color, 0.75);
                rect({button->x + 16.25, button->cy() - 5, 1.5, 10}, color, 0.75);
            }
            const int titleWidth = static_cast<int>(button->w - keyWidth - titleInset - 12);
            if (titleWidth > 0) {
                if (auto title = textures().text(text.label, FONT, ink, static_cast<int>(std::ceil(titleWidth / EMPTY_WORKSPACE_TEXT_SCALE)),
                                                 SCALE * EMPTY_WORKSPACE_TEXT_SCALE)) {
                    const auto size = logicalSize(title);
                    const auto box  = fitBox({button->x + titleInset, button->y + 4, std::min(size.x, static_cast<double>(titleWidth)), button->h - 8}, size.x / size.y);
                    tex(title, box, PROGRESS * (hover ? 1.F : 0.9F));
                }
            }
            const int errorWidth = static_cast<int>(button->x - m_usable.x - 24);
            if (!m_emptyWorkspaceError.empty() && errorWidth > 40) {
                if (auto text = textures().text(m_emptyWorkspaceError, FONT, config::overviewLabelColor(), errorWidth, SCALE)) {
                    const auto size = logicalSize(text);
                    const auto box  = fitBox({m_usable.x + 12, button->y + 4, size.x, button->h - 8}, size.x / size.y);
                    tex(text, box, PROGRESS);
                }
            }
        }

        // The output is the destination, rather than one of its existing tiles.
        if (WORKSPACE_CARRIED && workspaceDrag->targetMonitor == MONITOR) {
            const auto ink = config::overviewActiveBorder();
            rect(m_usable, ink.modifyA(0.055F));
            const double inset = std::max(2, BORDER);
            border({m_usable.x + inset, m_usable.y + inset, m_usable.w - inset * 2, m_usable.h - inset * 2}, ink, BORDER, ROUNDING);
        }

        // Dragging and settling share one live card. Geometry stays in global
        // logical coordinates until each output applies its own scale.
        auto workspaceCard = [&](const SWorkspaceDragPreview& preview, PHLWORKSPACE workspace, float opacity, const CHyprColor& ink, const CHyprColor& labelInk,
                                 double borderWidth, COverview* captureView = nullptr) {
            auto local = [&](SBoxF box) {
                box.x -= MONITOR->m_position.x;
                box.y -= MONITOR->m_position.y;
                return box;
            };
            const auto cell = local(preview.cell);
            const auto bg   = config::overviewTileBgColor();
            border(cell, ink, borderWidth, ROUNDING);
            rect(cell, bg.modifyA(bg.a * opacity), ROUNDING);
            for (const auto& slot : preview.windows) {
                const auto window = slot.window.lock();
                if (!window || !window->m_isMapped || window->isHidden() || window->m_workspace != workspace)
                    continue;
                const auto   box = local(slot.box), clip = local(slot.clip);
                const float  alpha = slot.visibility * opacity;
                const double left = std::max(box.x, clip.x), top = std::max(box.y, clip.y);
                const SBoxF  visible{left, top, std::min(box.x + box.w, clip.x + clip.w) - left, std::min(box.y + box.h, clip.y + clip.h) - top};
                if (visible.w <= 0 || visible.h <= 0)
                    continue;
                const auto texture = captureView ? captureView->textureFor(window) : slot.texture;
                if (texture)
                    windowTexture(window, texture, box, alpha, ROUNDING, hidden::shouldBlurWindow(window), clip);
                else
                    rect(visible, config::overviewTitleBgColor().modifyA(alpha), ROUNDING);
                if (slot.fullscreen) {
                    const auto color = config::overviewFullscreenBorder();
                    outline(visible, color.modifyA(color.a * alpha), std::max(2, BORDER));
                }
            }
            if (config::overviewShowLabels()) {
                if (auto text = textures().text(preview.label, FONT, labelInk, static_cast<int>(std::max(1.0, cell.w)), SCALE)) {
                    const auto size  = logicalSize(text);
                    const auto badge = overviewWorkspaceLabelBox(cell, size.x, size.y);
                    const auto color = config::overviewTitleBgColor();
                    rect(badge, color.modifyA(color.a * opacity), badge.h / 2);
                    tex(text, {badge.x + (badge.w - size.x) / 2, badge.y + 5, size.x, size.y}, opacity);
                }
            }
        };
        if (WORKSPACE_CARRIED) {
            const auto& carried = *workspaceDrag;
            const auto  source  = std::ranges::find_if(session().views, [&](const auto& view) { return view->monitor() == carried.sourceMonitor; });
            if (source != session().views.end()) {
                auto         preview = carried.preview;
                const double dx = carried.box.x - preview.cell.x, dy = carried.box.y - preview.cell.y;
                preview.cell = carried.box;
                for (auto& slot : preview.windows) {
                    for (auto* box : {&slot.box, &slot.clip}) {
                        box->x += dx;
                        box->y += dy;
                    }
                }
                workspaceCard(preview, carried.workspace, 0.94F, config::overviewActiveBorder(), config::overviewActiveBorder(), BORDER, source->get());
            }
        }
        if (workspaceSettle && session().workspaceSettle) {
            const auto& settle   = *session().workspaceSettle;
            const float progress = workspaceSettle->progress;
            auto        ink      = config::overviewActiveBorder();
            auto        labelInk = ink;
            double      width    = BORDER;
            // Selection remains usable during the glide. Blend into the tile's
            // current border rather than flashing back when the lift ends.
            if (const auto view = std::ranges::find_if(session().views, [&](const auto& view) { return view->monitor() == settle.monitor; });
                view != session().views.end()) {
                const auto& destination = **view;
                const auto  tile        = std::ranges::find_if(destination.m_tiles, [&](const auto& tile) {
                    const auto& entry = destination.m_entries[tile.key];
                    return settle.identity == SWorkspaceIdentity{entry.workspaceId, entry.workspaceName};
                });
                if (tile != destination.m_tiles.end()) {
                    const int  index    = static_cast<int>(tile - destination.m_tiles.begin());
                    const bool selected = index == destination.m_selected, hovered = index == destination.m_hovered;
                    const auto target      = selected ? config::overviewActiveBorder() : (hovered ? config::overviewHoverBorder() : config::overviewTileBorderColor());
                    ink                    = CHyprColor(std::lerp(ink.r, target.r, progress), std::lerp(ink.g, target.g, progress), std::lerp(ink.b, target.b, progress),
                                                        std::lerp(ink.a, target.a, progress));
                    const auto labelTarget = selected ? config::overviewActiveBorder() : config::overviewLabelColor();
                    labelInk               = CHyprColor(std::lerp(labelInk.r, labelTarget.r, progress), std::lerp(labelInk.g, labelTarget.g, progress),
                                                        std::lerp(labelInk.b, labelTarget.b, progress), std::lerp(labelInk.a, labelTarget.a, progress));
                    width                  = std::lerp(static_cast<double>(BORDER), selected || hovered ? static_cast<double>(BORDER) : 1.0, progress);
                }
            }
            workspaceCard(*workspaceSettle, settle.workspace, std::lerp(0.94F, 1.F, progress), ink, labelInk, width);
        }

        // Name the destination above the carried card, including when its
        // pickup size covers the top of a smaller output.
        if (WORKSPACE_CARRIED && workspaceDrag->targetMonitor == MONITOR) {
            const auto ink   = config::overviewActiveBorder();
            const auto label = "Move workspace " + workspaceDrag->preview.label + " to " + MONITOR->m_name;
            if (auto text = textures().text(label, FONT, ink, static_cast<int>(std::max(1.0, m_usable.w - 48)), SCALE)) {
                const auto  size = logicalSize(text);
                const SBoxF badge{m_usable.x + 16, m_usable.y + 12, size.x + 24, size.y + 12};
                rect(badge, config::overviewTitleBgColor(), 8);
                tex(text, {badge.x + 12, badge.y + 6, size.x, size.y}, 1.F);
            }
        }

        return out;
    }

} // namespace hyprspace
