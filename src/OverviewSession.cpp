#include "OverviewSession.hpp"

#include "CompositorHooks.hpp"
#include "Config.hpp"
#include "EmptyWorkspace.hpp"
#include "LaunchGeometry.hpp"
#include "Overview.hpp"
#include "OverlayPolicy.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/WorkspacePlacementController.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace hyprspace {
    std::unique_ptr<COverviewSession> g_overviewSession;
    COverviewSession&                 session() {
        return *g_overviewSession;
    }

    COverviewSession::COverviewSession() = default;
    COverviewSession::~COverviewSession() {
        stopInput();
        views.clear();
        restoreVisibility();
    }

    bool COverviewSession::live() const {
        return std::ranges::any_of(views, [](const auto& view) { return !view->closing(); });
    }

    void COverviewSession::begin() {
        cancelWorkspaceSettle();
        m_emptyButtonPressed = false;
        cancelZoom();
        m_zoomTarget.reset();
        selection.clear();
        if (!config::followMouse() && covers(Desktop::focusState()->monitor()))
            followKeyboardFocus();
        m_pointer = g_pInputManager->getMouseCoordsInternal();
        pointer(m_pointer);
        if (!selection.command() && !views.empty())
            keyboard(*views.front());
        ownCursor(true);
        hooks::syncKeyboardFocus();
    }

    void COverviewSession::stopInput() {
        cancelWorkspaceSettle();
        cancelZoom();
        cancelDrag();
        ownCursor(false);
        hooks::syncKeyboardFocus();
    }

    void COverviewSession::hideWindow(PHLWINDOW w) {
        m_visibility.hide(
            PHLWINDOWREF{w}, [](const auto& window) { return window->alpha(Desktop::View::WINDOW_ALPHA_FADE)->goal(); },
            [](const auto& window) { window->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(0.F); });
    }

    void COverviewSession::restoreVisibility() {
        m_visibility.restore([](const auto& w, float alpha) {
            if (w->m_isMapped)
                w->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(alpha);
        });
        Desktop::globalWindowController()->updateSuspendedStates();
        hooks::syncKeyboardFocus();
        // Closing views retain tile identities through their final frame. A
        // committed desktop/window or captured launch owns any used workspace.
        if (views.empty())
            m_preparedWorkspaces.clear();
        if (g_overviewSession.get() == this && !live() && overlaysAllowed())
            g_pInputManager->simulateMouseMovement();
    }

    bool COverviewSession::covers(PHLMONITOR monitor) const {
        return monitor && std::ranges::any_of(views, [&](const auto& view) { return view->monitor() == monitor; });
    }

    COverview* COverviewSession::emptyWorkspaceView(PHLMONITOR monitor) const {
        if (!monitor) {
            const auto point = g_pInputManager->getMouseCoordsInternal();
            // A point on an uncovered output must not silently act elsewhere.
            for (const auto& candidate : State::monitorState()->monitors()) {
                if (candidate && candidate->m_enabled && !candidate->isMirror() && candidate->logicalBox().containsPoint(point)) {
                    monitor = candidate;
                    break;
                }
            }
            if (!monitor && selection.command())
                monitor = selection.command()->monitor.lock();
        }
        if (!monitor || !monitor->m_enabled || monitor->isMirror())
            return nullptr;
        for (const auto& view : views)
            if (!view->closing() && view->monitor() == monitor)
                return view.get();
        return nullptr;
    }

    COverview* COverviewSession::emptyWorkspaceButtonView(const Vector2D& point) const {
        for (const auto& view : views)
            if (!view->closing() && view->emptyWorkspaceButtonHit(point))
                return view.get();
        return nullptr;
    }

    bool COverviewSession::preparedWorkspace(PHLWORKSPACE workspace) const {
        return static_cast<bool>(preparedLifetime(workspace));
    }

    PHLWORKSPACE COverviewSession::preparedLifetime(PHLWORKSPACE workspace) const {
        if (!valid(workspace))
            return nullptr;
        for (const auto& prepared : m_preparedWorkspaces)
            if (prepared.workspace == workspace && prepared.monitor == workspace->m_monitor && workspace->m_monitor && workspace->m_monitor->m_enabled)
                return prepared.workspace;
        return nullptr;
    }

    void COverviewSession::prunePreparedWorkspaces() {
        std::erase_if(m_preparedWorkspaces, [&](const auto& prepared) {
            const auto& ws = prepared.workspace;
            return !valid(ws) || !ws->m_monitor || !ws->m_monitor->m_enabled || ws->m_monitor != prepared.monitor || !covers(ws->m_monitor.lock()) ||
                   State::workspaceState()->query().id(ws->m_id).run() != ws;
        });
    }

    void COverviewSession::retainPreparedWorkspace(PHLWORKSPACE workspace) {
        if (valid(workspace) && !preparedWorkspace(workspace))
            m_preparedWorkspaces.push_back({workspace, workspace->m_monitor});
    }

    std::expected<PHLWORKSPACE, std::string> COverviewSession::resolveEmptyWorkspace(PHLMONITOR monitor, bool isEmpty) {
        prunePreparedWorkspaces();
        std::vector<SEmptyWorkspaceCandidate> candidates;
        const auto                            workspaces = State::workspaceState()->workspacesCopy();
        for (const auto& ws : workspaces) {
            if (!valid(ws))
                continue;
            const auto owner   = ws->m_monitor.lock();
            bool       allowed = true;
            if (const auto rule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(ws); rule && !rule->m_monitor.empty())
                allowed = State::monitorState()->query().relativeTo(monitor).configString(rule->m_monitor).run() == monitor;
            candidates.push_back({.id       = ws->m_id,
                                  .monitor  = owner ? owner->m_id : MONITOR_INVALID,
                                  .empty    = ws->getWindowCount() == 0,
                                  .special  = ws->m_isSpecialWorkspace,
                                  .active   = ws == monitor->m_activeWorkspace,
                                  .prepared = preparedWorkspace(ws),
                                  .allowed  = allowed});
        }
        std::vector<SEmptyWorkspaceRule> rules;
        for (const auto& rule : Config::workspaceRuleMgr()->getAllWorkspaceRules()) {
            if (!rule || !rule->isEnabled())
                continue;
            rules.push_back(
                {.selector       = rule->m_workspaceString,
                 .monitorBinding = !rule->m_monitor.empty(),
                 .targetMonitor  = !rule->m_monitor.empty() && State::monitorState()->query().relativeTo(monitor).configString(rule->m_monitor).run() == monitor,
                 .defaultName    = rule->m_defaultName});
        }
        std::string error;
        const auto  choice = chooseEmptyWorkspace(candidates, monitor->m_id, rules, &error);
        if (!choice)
            return std::unexpected(error);
        const auto ws =
            choice->existing ? State::workspaceState()->query().id(choice->id).run() : State::workspaceState()->create(choice->id, monitor->m_id, "", isEmpty);
        if (!valid(ws) || ws->m_monitor != monitor)
            return std::unexpected("hyprspace: could not prepare an empty workspace on this monitor");
        return ws;
    }

    SDispatchResult COverviewSession::emptyWorkspace(PHLMONITOR monitor) {
        if (!overlaysAllowed() || !live())
            return {.success = false, .error = "hyprspace: empty workspace requires an open overview"};
        auto* view = emptyWorkspaceView(monitor);
        if (!view)
            return {.success = false, .error = "hyprspace: no open overview on the target monitor"};
        if (gestureActive() || pendingResize || m_pan)
            return {.success = false, .error = "hyprspace: finish the current gesture before preparing an empty workspace"};
        const auto workspace = resolveEmptyWorkspace(view->monitor(), true);
        if (!workspace) {
            view->setEmptyWorkspaceError(workspace.error());
            damage();
            return {.success = false, .error = workspace.error()};
        }
        const bool alreadyPrepared = preparedWorkspace(*workspace);
        retainPreparedWorkspace(*workspace);
        cancelZoom();
        view->setEmptyWorkspaceError("");
        if (!view->refreshPreparedWorkspace(*workspace)) {
            if (!alreadyPrepared)
                std::erase_if(m_preparedWorkspaces, [&](const auto& prepared) { return prepared.workspace == *workspace; });
            view->setEmptyWorkspaceError("hyprspace: not enough space to show this workspace");
            damage();
            return {.success = false, .error = view->emptyWorkspaceError()};
        }
        damage();
        return {.success = true};
    }

    void COverviewSession::reconcileVisibility() {
        prunePreparedWorkspaces();
        m_visibility.restoreIf([&](const auto& w) { return !covers(w->m_monitor.lock()); },
                               [](const auto& w, float alpha) {
                                   if (w->m_isMapped) {
                                       w->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(alpha);
                                       g_pHyprRenderer->damageWindow(w);
                                       w->setSuspended(!w->m_workspace || !w->m_workspace->isVisible());
                                   }
                               });
    }

    void COverviewSession::ownCursor(bool own) {
        own = own && overlaysAllowed();
        if (own == m_cursorOwned)
            return;
        if (!own) {
            cancelPan();
            cancelWorkspaceDrag();
        }
        cancelZoomEdge();
        m_cursorOwned = own;
        if (own)
            g_pSeatManager->setPointerFocus(nullptr, {});
        hooks::ownCursor(own);
        updateCursor();
    }

    std::optional<SOverviewTarget> COverviewSession::hit(const Vector2D& pos) const {
        if (emptyWorkspaceButtonView(pos))
            return std::nullopt;
        for (const auto& view : views)
            if (!view->closing())
                if (auto target = view->targetAt(pos))
                    return target;
        return std::nullopt;
    }

    void COverviewSession::observePointer(const Vector2D& pos) {
        if (hooks::mappingPointer() || gestureActive())
            return;
        // Foreground motion updates coordinates and observes edge exits, but
        // cannot select previews or start a dwell while another UI owns input.
        m_pointer       = pos;
        const auto view = zoomView();
        m_zoomEdgeHover.sync(view ? view->zoomEdgeAt(pos) : std::nullopt);
        m_zoomEdgeIntent.reset();
    }

    void COverviewSession::pointer(const Vector2D& pos, bool userMotion) {
        // Native commands may synthesize motion while their coordinates are
        // mapped to the desktop. Those are not overview pointer coordinates.
        if (hooks::mappingPointer())
            return;
        const auto delta     = pos - m_pointer;
        const bool userMoved = userMotion && pos != m_pointer;
        m_pointer            = pos;
        if (workspaceDrag) {
            validateWorkspaceDrag();
            if (workspaceDrag) {
                auto& gesture = *workspaceDrag;
                gesture.moved |= userMoved && (pos - gesture.pickup).size() > gesture.threshold;
                gesture.box.x         = pos.x - gesture.offset.x;
                gesture.box.y         = pos.y - gesture.offset.y;
                gesture.targetMonitor = workspaceDropMonitor(pos);
                damage();
                updateCursor();
                return;
            }
        }
        if (m_pan) {
            updatePan();
            if (m_pan) {
                if (userMoved && m_pan->started)
                    if (auto* view = zoomView())
                        view->panInspection(delta);
                updateCursor();
                return;
            }
        }
        const auto view = zoomView();
        const auto edge = view ? view->zoomEdgeAt(pos) : std::nullopt;
        if (userMoved) {
            if (m_zoomEdgeHover.motion(edge, view && zoomEdgeEnabled(*view), CZoomEdgeHover::Clock::now(), view && view->zoomNavigationReady())) {
                if (auto destination = view->zoomNeighbor(m_zoomTarget->workspace, *edge))
                    m_zoomEdgeIntent = SZoomEdgeIntent{m_zoomTarget->workspace, *destination, m_zoomTarget->monitor};
                else
                    cancelZoomEdge();
            }
            if (!m_zoomEdgeHover.pending())
                m_zoomEdgeIntent.reset();
        } else if (!userMotion) {
            m_zoomEdgeHover.sync(edge);
            m_zoomEdgeIntent.reset();
        }
        const auto target = hit(pos);
        const bool follow = config::followMouse() && !zoomHeld();
        if (userMoved)
            selection.pointer(target, follow);
        else
            selection.refresh(target, follow);
        // Real pointer motion can inspect another window in the enlarged
        // workspace while held. Once released, motion resumes pointer following
        // immediately, including while the camera is still returning.
        if (userMoved && config::followMouse() && zoomHeld() && m_zoomTarget && target && target->workspace == m_zoomTarget->workspace &&
            target->monitor == m_zoomTarget->monitor)
            selection.keyboard(*target);
        for (const auto& view : views)
            if (!view->closing())
                view->onMouseMove(pos);
        syncSelection();
        if (drag.active()) {
            const auto w = drag.window.lock();
            if (!w || !w->m_isMapped) {
                cancelDrag();
                return;
            }
            const auto delta        = pos - drag.pickup;
            auto       desktopDelta = delta / drag.scale;
            if (drag.mode == SOverviewDrag::MOVE && target)
                desktopDelta = desktopPoint(*target) - desktopPoint(drag.source);
            // Native thresholds use desktop logical pixels, and only a real
            // displacement strictly beyond the threshold starts a gesture.
            drag.moved |= desktopDelta.size() > drag.threshold;
            if (drag.mode == SOverviewDrag::MOVE) {
                drag.box.x = pos.x - drag.offset.x;
                drag.box.y = pos.y - drag.offset.y;
            } else {
                if (const auto box = hooks::resizeGeometry(w, drag.source.desktopBox, {desktopDelta.x, desktopDelta.y}, drag.resizeLeft, drag.resizeTop)) {
                    const auto point = mapPreviewPoint({box->x, box->y}, drag.source.desktopBox, drag.source.preview);
                    if (point)
                        drag.box = {point->x, point->y, box->w * drag.scale.x, box->h * drag.scale.y};
                }
            }
        }
        damage();
        updateCursor();
    }

    void COverviewSession::keyboard(COverview& view) {
        cancelWorkspaceSettle();
        cancelPan();
        cancelZoomEdge();
        retargetSelection(view);
    }

    void COverviewSession::retargetSelection(COverview& view) {
        if (gestureActive()) {
            syncSelection();
            return;
        }
        if (auto target = view.selectedTarget()) {
            selection.keyboard(*target);
            if (zoomHeld()) {
                if (m_zoomTarget && m_zoomTarget->monitor != target->monitor)
                    for (const auto& other : views)
                        if (other->monitor() == m_zoomTarget->monitor)
                            other->releaseZoom();
                m_zoomTarget = *target;
                if (!view.zoomTo(target->workspace))
                    cancelZoom();
            }
        }
        damage();
    }

    std::optional<uint64_t> COverviewSession::zoomPress() {
        cancelWorkspaceSettle();
        if (!live() || gestureActive())
            return std::nullopt;
        if (zoomHeld())
            return m_zoomHolds.press();
        auto* view = keyboardView();
        if (!view)
            return std::nullopt;
        auto target = selection.command();
        if (!target)
            target = view->selectedTarget();
        if (!target || !view->zoomTo(target->workspace))
            return std::nullopt;
        m_zoomTarget = *target;
        m_zoomEdgeHover.sync(view->zoomEdgeAt(m_pointer));
        m_zoomEdgeIntent.reset();
        selection.keyboard(*target);
        damage();
        const auto token = m_zoomHolds.press();
        m_zoomGeneration = token;
        return token;
    }

    void COverviewSession::zoomRelease(uint64_t token) {
        if (m_zoomHolds.release(token) && !zoomHeld()) {
            cancelPan();
            cancelZoomEdge();
            for (const auto& view : views)
                view->releaseZoom();
            damage();
            updateCursor();
        }
    }

    void COverviewSession::cancelZoom() {
        cancelPan();
        cancelZoomEdge();
        m_zoomHolds.cancel();
        for (const auto& view : views)
            view->releaseZoom();
        updateCursor();
    }

    bool COverviewSession::zoomScroll(const SScrollInput& event, const Vector2D& pos) {
        if (m_pan) {
            updateZoom();
            if (m_pan)
                return true;
        }
        if (!zoomHeld() || !config::overviewWheelZoom() || (!event.wheel && !event.finger) || event.wheelTilt || event.horizontal)
            return false;
        // Ownership was checked by the mouse-axis listener. A held wheel event
        // remains consumed at bounds, during a drag, or over another output.
        cancelZoomEdge();
        if (gestureActive() || hooks::mappingPointer() || !m_cursorOwned)
            return true;
        updateZoom();
        pointer(pos);
        if (auto* view = zoomView(); view && zoomHeld())
            view->inspectScroll(event, pos);
        updateCursor();
        return true;
    }

    bool COverviewSession::panAvailable() const {
        const auto* view = zoomView();
        return live() && m_cursorOwned && zoomHeld() && !gestureActive() && view && view->inspectionPanAvailable(m_pointer);
    }

    void COverviewSession::zoomStep(int direction) {
        if (!live() || !m_cursorOwned || !zoomHeld() || !config::overviewWheelZoom() || gestureActive() || m_pan || hooks::mappingPointer())
            return;
        cancelZoomEdge();
        updateZoom();
        if (auto* view = zoomView())
            view->inspectZoom(-direction, 1);
        updateCursor();
    }

    void COverviewSession::panKey(const Vector2D& delta) {
        if (!live() || !m_cursorOwned || !zoomHeld() || !config::overviewWheelZoom() || gestureActive() || m_pan || hooks::mappingPointer())
            return;
        cancelZoomEdge();
        updateZoom();
        if (auto* view = zoomView())
            view->panInspectionKey(delta);
        updateCursor();
    }

    std::optional<uint64_t> COverviewSession::panPress() {
        if (!live() || !m_cursorOwned || !zoomHeld() || !config::overviewWheelZoom() || gestureActive() || hooks::mappingPointer() || (m_pan && !m_pan->keyboard))
            return std::nullopt;
        auto* view = zoomView();
        if (!view || !m_zoomTarget || !view->inspectionPanHit(m_pointer) || (view->inspectionGoal() <= 1 && view->inspectionFactor() <= 1))
            return std::nullopt;
        if (!m_pan)
            m_pan = SInspectionPan{m_zoomTarget->workspace, m_zoomTarget->monitor, false, true};
        const auto token = m_panHolds.press();
        cancelZoomEdge();
        updatePan();
        updateCursor();
        return token;
    }

    void COverviewSession::panRelease(uint64_t token) {
        if (m_panHolds.release(token) && !m_panHolds.held() && keyboardPanHeld())
            cancelPan();
    }

    void COverviewSession::updateCursor() {
        if (!m_cursorOwned)
            return;
        const auto shape = workspaceDrag                                                 ? "grabbing"
                           : drag.active()                                               ? (drag.mode == SOverviewDrag::MOVE ? "grabbing"
                                                                                            : drag.resizeTop                 ? (drag.resizeLeft ? "nw-resize" : "ne-resize")
                                                                                                                             : (drag.resizeLeft ? "sw-resize" : "se-resize"))
                           : panning()                                                   ? "grabbing"
                           : panAvailable() && g_pInputManager->getModsFromAllKBs() == 0 ? "grab"
                                                                                         : "default";
        hooks::setCursor(shape);
    }

    void COverviewSession::cancelPan() {
        m_panHolds.cancel();
        if (!m_pan)
            return;
        m_pan.reset();
        cancelZoomEdge();
        updateCursor();
    }

    void COverviewSession::updatePan() {
        if (!m_pan)
            return;
        auto* view = zoomView();
        if (!live() || !m_cursorOwned || !zoomHeld() || gestureActive() || !config::overviewWheelZoom() || !m_zoomTarget || m_pan->workspace != m_zoomTarget->workspace ||
            m_pan->monitor != m_zoomTarget->monitor || !view) {
            cancelPan();
            return;
        }
        if (!m_pan->started && view->beginInspectionPan(m_pointer))
            m_pan->started = true;
    }

    bool COverviewSession::zoomHeld() const {
        return m_zoomHolds.held();
    }

    bool COverviewSession::zoomLocked() const {
        return m_zoomTarget.has_value();
    }

    std::optional<SOverviewTarget> COverviewSession::zoomTarget() const {
        return m_zoomTarget;
    }

    COverview* COverviewSession::zoomView() const {
        if (m_zoomTarget)
            for (const auto& view : views)
                if (!view->closing() && view->monitor() == m_zoomTarget->monitor)
                    return view.get();
        return nullptr;
    }

    bool COverviewSession::zoomEdgeEnabled(const COverview& view) const {
        return overlaysAllowed() && live() && m_cursorOwned && config::followMouse() && zoomHeld() && !gestureActive() && !m_pan && m_zoomTarget &&
               view.monitor() == m_zoomTarget->monitor && !view.closing() && !view.inspectionActive() && !view.inspectionTransitioning();
    }

    bool COverviewSession::zoomEdgeReady(const COverview& view) const {
        return zoomEdgeEnabled(view) && view.zoomNavigationReady();
    }

    std::optional<EDirection> COverviewSession::zoomEdgePending() const {
        return m_zoomEdgeHover.pending();
    }

    void COverviewSession::cancelZoomEdge() {
        m_zoomEdgeHover.cancel();
        m_zoomEdgeIntent.reset();
    }

    void COverviewSession::advanceZoomEdge(PHLMONITOR renderedMonitor) {
        if (!m_zoomEdgeIntent)
            return;
        auto* view = zoomView();
        if (!view || !zoomEdgeEnabled(*view)) {
            cancelZoomEdge();
            return;
        }
        // Other outputs may render before this view has refreshed its grid.
        if (view->monitor() != renderedMonitor)
            return;
        const auto edge        = view->zoomEdgeAt(m_pointer);
        const auto destination = edge ? view->zoomNeighbor(m_zoomTarget->workspace, *edge) : std::nullopt;
        if (m_zoomEdgeIntent->monitor != m_zoomTarget->monitor || m_zoomEdgeIntent->source != m_zoomTarget->workspace || !destination ||
            *destination != m_zoomEdgeIntent->destination) {
            cancelZoomEdge();
            return;
        }
        if (m_zoomEdgeHover.advance(edge, view->zoomNavigationReady(), CZoomEdgeHover::Clock::now())) {
            view->selectTarget({.workspace = *destination, .monitor = m_zoomTarget->monitor});
            m_zoomEdgeIntent.reset();
            retargetSelection(*view);
        }
    }

    void COverviewSession::updateZoom() {
        if (!m_zoomTarget)
            return;
        if (zoomHeld() && !gestureActive()) {
            auto view = std::ranges::find_if(views, [&](const auto& candidate) { return !candidate->closing() && candidate->monitor() == m_zoomTarget->monitor; });
            if (view == views.end() || !(*view)->zoomTo(m_zoomTarget->workspace)) {
                cancelZoom();
                if (view != views.end())
                    if (auto target = (*view)->selectedTarget())
                        selection.keyboard(*target);
            }
        }
        if (!zoomHeld() && std::ranges::none_of(views, [](const auto& view) { return !view->closing() && view->zoomTransitioning(); }))
            m_zoomTarget.reset();
    }

    void COverviewSession::syncSelection() {
        if (const auto& target = selection.command())
            for (const auto& view : views)
                if (!view->closing() && view->monitor() == target->monitor)
                    view->selectTarget(*target);
    }

    void COverviewSession::refreshPointerTarget(PHLMONITOR renderedMonitor) {
        if (workspaceDrag) {
            validateWorkspaceDrag();
            if (workspaceDrag) {
                workspaceDrag->targetMonitor = workspaceDropMonitor(m_pointer);
                updateCursor();
                return;
            }
        }
        updateZoom();
        if (!live() || !m_cursorOwned)
            return;
        updatePan();
        if (m_pan) {
            updateCursor();
            return;
        }
        selection.refresh(hit(m_pointer), config::followMouse() && !zoomHeld());
        for (const auto& view : views)
            if (!view->closing())
                view->onMouseMove(m_pointer);
        syncSelection();
        advanceZoomEdge(renderedMonitor);
        updateCursor();
    }

    COverview* COverviewSession::keyboardView() const {
        if (const auto& target = selection.command(); target)
            for (const auto& view : views)
                if (!view->closing() && view->monitor() == target->monitor) {
                    view->selectTarget(*target);
                    return view.get();
                }
        for (const auto& view : views)
            if (!view->closing())
                return view.get();
        return nullptr;
    }

    PHLWORKSPACE COverviewSession::workspace(const SOverviewTarget& target) const {
        const auto ws = State::workspaceState()->query().id(target.workspace.id).run();
        // Do not resolve a re-used named ID to a different workspace.
        if (ws)
            return ws->m_name == target.workspace.name && ws->m_monitor && ws->m_monitor->m_enabled ? ws : nullptr;
        const auto mon = target.monitor.lock();
        if (!mon || !mon->m_enabled)
            return nullptr;
        return State::workspaceState()->create(target.workspace.id, mon->m_id, target.workspace.name);
    }

    Vector2D COverviewSession::desktopPoint(const SOverviewTarget& target) const {
        const auto ws  = workspace(target);
        const auto mon = ws ? ws->m_monitor.lock() : nullptr;
        if (!mon)
            return target.desktop;
        const auto&    reserved = mon->m_reservedArea;
        const auto     pos      = mon->m_position + Vector2D{reserved.left(), reserved.top()};
        const Vector2D size{std::max(1.0, mon->m_size.x - reserved.left() - reserved.right()), std::max(1.0, mon->m_size.y - reserved.top() - reserved.bottom())};
        const auto     point = mapPreviewPoint({target.desktop.x, target.desktop.y}, target.monitorBox, {pos.x, pos.y, size.x, size.y});
        return point ? Vector2D{point->x, point->y} : target.desktop;
    }

    void COverviewSession::establishTarget() {
        if (!selection.command())
            return;
        const auto target = *selection.command();
        const auto ws     = workspace(target);
        if (!ws || !ws->m_monitor)
            return;
        const auto mon = ws->m_monitor.lock();
        if (ws->m_isSpecialWorkspace)
            mon->setSpecialWorkspace(ws);
        else {
            if (mon->m_activeSpecialWorkspace)
                mon->setSpecialWorkspace(nullptr);
            if (mon->m_activeWorkspace != ws)
                mon->changeWorkspace(ws);
        }
        Desktop::focusState()->rawMonitorFocus(mon);
        auto w = target.window.lock();
        if (!w || !w->m_isMapped || w->isHidden() || w->m_workspace != ws)
            w = ws->getLastFocusedWindow();
        if (!w || !w->m_isMapped || w->isHidden() || w->m_workspace != ws)
            w = ws->getFocusCandidate();
        if (w && (!w->m_isMapped || w->isHidden() || w->m_workspace != ws))
            w.reset();
        Desktop::focusState()->rawWindowFocus(w, Desktop::FOCUS_REASON_KEYBIND);
    }

    void COverviewSession::followKeyboardFocus() {
        const auto w   = Desktop::focusState()->window();
        const auto mon = w ? w->m_monitor.lock() : Desktop::focusState()->monitor();
        const auto ws  = w ? w->m_workspace : (mon ? (mon->m_activeSpecialWorkspace ? mon->m_activeSpecialWorkspace : mon->m_activeWorkspace) : nullptr);
        if (!ws || !mon)
            return;
        if (m_zoomTarget && (m_zoomTarget->workspace != SWorkspaceIdentity{ws->m_id, ws->m_name} || m_zoomTarget->monitor != mon))
            return;
        auto target             = selection.command().value_or(SOverviewTarget{});
        target.workspace        = {ws->m_id, ws->m_name};
        target.monitor          = mon;
        target.window           = w;
        const auto&    reserved = mon->m_reservedArea;
        const auto     origin   = mon->m_position + Vector2D{reserved.left(), reserved.top()};
        const Vector2D usable{std::max(1.0, mon->m_size.x - reserved.left() - reserved.right()), std::max(1.0, mon->m_size.y - reserved.top() - reserved.bottom())};
        target.monitorBox = {origin.x, origin.y, usable.x, usable.y};
        const auto pos    = w ? w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) : origin;
        const auto size   = w ? w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) : usable;
        target.desktopBox = {pos.x, pos.y, size.x, size.y};
        target.desktop    = pos + size / 2;
        selection.keyboard(target);
        for (const auto& view : views)
            if (view->monitor() == target.monitor)
                view->selectTarget(target);
        damage();
    }

    bool COverviewSession::workspaceDragValid() const {
        if (!workspaceDrag || !live() || !overlaysAllowed())
            return false;
        const auto& gesture = *workspaceDrag;
        const auto  source  = gesture.sourceMonitor.lock();
        return valid(gesture.workspace) && source && source->m_enabled && !source->isMirror() && covers(source) && !gesture.workspace->m_isSpecialWorkspace &&
               gesture.workspace->m_monitor == source && SWorkspaceIdentity{gesture.workspace->m_id, gesture.workspace->m_name} == gesture.identity &&
               State::workspaceState()->query().id(gesture.identity.id).run() == gesture.workspace && gesture.connectionsValid && gesture.connectionsValid() &&
               !g_layoutManager->dragController()->target();
    }

    void COverviewSession::validateWorkspaceDrag() {
        if (workspaceDrag && !workspaceDragValid())
            cancelWorkspaceDrag();
    }

    float COverviewSession::workspaceSettleProgress() const {
        if (!workspaceSettle || !m_workspaceSettleProgress)
            return 1.F;
        const float remaining = 1.F - m_workspaceSettleProgress->getPercent();
        return 1.F - remaining * remaining * remaining;
    }

    std::optional<SWorkspaceDragPreview> COverviewSession::workspaceSettlePreview() const {
        if (!workspaceSettle)
            return std::nullopt;
        const auto& settle  = *workspaceSettle;
        const auto  monitor = settle.monitor.lock();
        if (!live() || !overlaysAllowed() || !valid(settle.workspace) || !monitor || !monitor->m_enabled || monitor->isMirror() ||
            settle.workspace->m_monitor != monitor || settle.workspace->m_id != settle.identity.id || settle.workspace->m_name != settle.identity.name ||
            State::workspaceState()->query().id(settle.identity.id).run() != settle.workspace)
            return std::nullopt;
        for (const auto& view : views) {
            if (view->closing() || view->monitor() != monitor)
                continue;
            auto destination = view->workspacePreview(settle.identity);
            if (!destination)
                return std::nullopt;
            const auto            progress = workspaceSettleProgress();
            SWorkspaceDragPreview picture{.cell = interpolateBox(settle.from.cell, destination->cell, progress), .label = destination->label, .progress = progress};
            for (auto slot : destination->windows) {
                const auto old = std::ranges::find_if(settle.from.windows, [&](const auto& from) { return from.window == slot.window; });
                if (old != settle.from.windows.end()) {
                    slot.box        = interpolateBox(old->box, slot.box, progress);
                    slot.clip       = interpolateBox(old->clip, slot.clip, progress);
                    slot.visibility = std::lerp(old->visibility, slot.visibility, progress);
                    if (!slot.texture)
                        slot.texture = old->texture;
                } else {
                    // A client mapped during the drag joins the native move;
                    // ease its appearance into the same moving card.
                    slot.box  = interpolateBox(reprojectOverviewBox(slot.box, destination->cell, settle.from.cell), slot.box, progress);
                    slot.clip = interpolateBox(reprojectOverviewBox(slot.clip, destination->cell, settle.from.cell), slot.clip, progress);
                    slot.visibility *= progress;
                }
                picture.windows.push_back(std::move(slot));
            }
            return picture;
        }
        return std::nullopt;
    }

    void COverviewSession::validateWorkspaceSettle() {
        if (workspaceSettle && (!m_workspaceSettleProgress || !m_workspaceSettleProgress->isBeingAnimated() || !workspaceSettlePreview()))
            cancelWorkspaceSettle();
    }

    void COverviewSession::cancelWorkspaceSettle() {
        if (!workspaceSettle)
            return;
        workspaceSettle.reset();
        if (m_workspaceSettleProgress)
            m_workspaceSettleProgress->setValueAndWarp(1.F);
        damage();
    }

    void COverviewSession::beginWorkspaceSettle(SWorkspaceDrag gesture, PHLMONITOR destination) {
        cancelWorkspaceSettle();
        const auto configured = Config::animationTree()->getAnimationPropertyConfig("windowsMove");
        if (!*CConfigValue<Config::INTEGER>("animations:enabled") || !configured || !configured->pValues || !configured->pValues->internalEnabled)
            return;
        if (!m_workspaceSettleConfig) {
            // Keep our config alive with the session. Hyprland's native clock
            // schedules frames; a fixed 180 ms ease-out has no spring overshoot.
            m_workspaceSettleConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
            m_workspaceSettleConfig->internalEnabled = 1;
            m_workspaceSettleConfig->internalSpeed   = 1.8F;
            m_workspaceSettleConfig->internalBezier  = "default";
            m_workspaceSettleConfig->pValues         = m_workspaceSettleConfig;
            Animation::mgr()->createAnimation(1.F, m_workspaceSettleProgress, m_workspaceSettleConfig, AVARDAMAGE_NONE);
            m_workspaceSettleProgress->setUpdateCallback([this](auto) { damage(); });
        }
        const double dx = gesture.box.x - gesture.preview.cell.x, dy = gesture.box.y - gesture.preview.cell.y;
        gesture.preview.cell = gesture.box;
        for (auto& slot : gesture.preview.windows) {
            slot.box.x += dx;
            slot.box.y += dy;
            slot.clip.x += dx;
            slot.clip.y += dy;
        }
        workspaceSettle = SWorkspaceSettle{gesture.workspace, gesture.identity, destination, std::move(gesture.preview)};
        m_workspaceSettleProgress->setValueAndWarp(0.F);
        *m_workspaceSettleProgress = 1.F;
        validateWorkspaceSettle();
        damage();
    }

    PHLMONITOR COverviewSession::workspaceDropMonitor(const Vector2D& point) const {
        if (!workspaceDrag || !m_cursorOwned || hooks::foregroundPointerAt(point))
            return nullptr;
        const auto pointerMonitor = State::monitorState()->query().vec(point).run();
        for (const auto& view : views) {
            const auto monitor = view->monitor();
            const auto area    = view->workspaceDropArea();
            if (monitor && monitor == pointerMonitor && monitor != workspaceDrag->sourceMonitor && monitor->m_enabled && !monitor->isMirror() && area &&
                area->contains(point.x, point.y))
                return monitor;
        }
        return nullptr;
    }

    bool COverviewSession::beginWorkspaceDrag() {
        if (!live() || !m_cursorOwned || gestureActive() || pendingResize || m_pan || g_pInputManager->hasHeldButtons() || g_layoutManager->dragController()->target())
            return false;
        for (const auto& view : views) {
            const auto target = view->workspaceTargetAt(m_pointer);
            if (!target)
                continue;
            const auto ws      = State::workspaceState()->query().id(target->workspace.id).run();
            const auto preview = view->workspacePreview(target->workspace);
            if (!valid(ws) || ws->m_isSpecialWorkspace || ws->m_monitor != target->monitor || ws->m_name != target->workspace.name || !preview)
                return false;
            const auto command = hit(m_pointer).value_or(*target);
            workspaceDrag.emplace();
            auto& gesture            = *workspaceDrag;
            gesture.workspace        = ws;
            gesture.identity         = target->workspace;
            gesture.sourceMonitor    = target->monitor;
            gesture.focusedMonitor   = Desktop::focusState()->monitor();
            gesture.focusedWindow    = Desktop::focusState()->window();
            gesture.pickup           = m_pointer;
            gesture.preview          = *preview;
            gesture.box              = preview->cell;
            gesture.offset           = m_pointer - Vector2D{gesture.box.x, gesture.box.y};
            gesture.threshold        = std::max(0.0, static_cast<double>(*CConfigValue<Config::INTEGER>("binds:drag_threshold")));
            gesture.connectionsValid = hooks::pointerConnectionGuard();
            selection.keyboard(command);
            cancelZoom();
            for (const auto& candidate : views)
                candidate->freezeZoom();
            m_zoomTarget.reset();
            syncSelection();
            validateWorkspaceDrag();
            updateCursor();
            damage();
            return workspaceDrag.has_value();
        }
        return false;
    }

    void COverviewSession::finishWorkspaceDrag() {
        const bool ready       = workspaceDragValid();
        const auto destination = ready ? workspaceDropMonitor(m_pointer) : nullptr;
        auto       gesture     = std::move(*workspaceDrag);
        // Capture textures can be replaced on each frame. Retain the latest
        // picture until the destination has rendered its first live capture.
        for (const auto& view : views)
            if (view->monitor() == gesture.sourceMonitor)
                for (auto& slot : gesture.preview.windows)
                    if (auto window = slot.window.lock())
                        slot.texture = view->textureFor(window);
        // Native workspace changes emit callbacks and release native buttons.
        // The captured initiating release has already been consumed; remove
        // the gesture before those callbacks can examine its old ownership.
        workspaceDrag.reset();
        cancelZoom();
        if (ready && gesture.moved && destination) {
            const auto source         = gesture.sourceMonitor.lock();
            const auto focusedMonitor = gesture.focusedMonitor.lock();
            const auto focusedWindow  = gesture.focusedWindow.lock();
            bool       moved          = true;
            if (source->m_activeWorkspace == gesture.workspace) {
                PHLWORKSPACE replacement;
                for (const auto& reference : State::workspaceState()->workspaces()) {
                    const auto candidate = reference.lock();
                    if (valid(candidate) && candidate->m_monitor == source && !candidate->m_isSpecialWorkspace && candidate != gesture.workspace) {
                        replacement = candidate;
                        break;
                    }
                }
                if (!replacement) {
                    WORKSPACEID id = 1;
                    while (State::workspaceState()->query().id(id).run() || [&] {
                        const auto bound = Config::workspaceRuleMgr()->getBoundMonitorForWS(std::to_string(id));
                        return bound && bound != source;
                    }()) {
                        if (id == std::numeric_limits<WORKSPACEID>::max()) {
                            moved = false;
                            break;
                        }
                        ++id;
                    }
                    if (moved)
                        replacement = State::workspaceState()->create(id, source->m_id);
                }
                moved = valid(replacement) && replacement->m_monitor == source;
                if (moved)
                    source->changeWorkspace(replacement, false, true, true);
            }
            if (moved) {
                // The native active-workspace move repairs pinned membership
                // after switching the source. Do the same before our inactive
                // move, whose native pinned branch retains the moved ID.
                for (const auto& window : Desktop::windowState()->windows()) {
                    if (window->m_pinned && window->m_workspace == gesture.workspace && source->m_activeWorkspace) {
                        if (window->layoutTarget()->space() != source->m_activeWorkspace->m_space)
                            window->layoutTarget()->assignToSpace(source->m_activeWorkspace->m_space);
                        window->m_workspace = source->m_activeWorkspace;
                    }
                }
                State::workspacePlacementController()->moveWorkspaceToMonitor(gesture.workspace, destination, true);
                moved = gesture.workspace->m_monitor == destination;
            }
            if (moved) {
                // Empty workspaces need an owner once inactive. Update the
                // retained output before the normal pruning pass can run.
                for (auto& prepared : m_preparedWorkspaces)
                    if (prepared.workspace == gesture.workspace)
                        prepared.monitor = destination;
                retainPreparedWorkspace(gesture.workspace);
                gesture.workspace->m_space->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);
                g_layoutManager->recalculateMonitor(source);
                g_layoutManager->recalculateMonitor(destination);
                // These clients are hidden behind the overview. Finish native
                // window motion so the card alone supplies the visible glide;
                // its destination geometry must not drift underneath it.
                for (const auto& window : Desktop::windowState()->windows())
                    if (window->m_isMapped && window->m_workspace == gesture.workspace)
                        window->layoutTarget()->warpPositionSize();
                if (focusedWindow && focusedWindow->m_workspace == gesture.workspace) {
                    const auto visible   = source->m_activeSpecialWorkspace ? source->m_activeSpecialWorkspace : source->m_activeWorkspace;
                    auto       candidate = visible ? Fullscreen::controller()->getFullscreenWindow(visible) : nullptr;
                    if (!candidate && visible)
                        candidate = visible->getLastFocusedWindow();
                    if (!candidate && visible)
                        candidate = visible->getFocusCandidate();
                    if (candidate && (!candidate->m_isMapped || candidate->isHidden() || candidate->m_workspace != visible))
                        candidate.reset();
                    Desktop::focusState()->rawWindowFocus(candidate, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
                }
                if (focusedMonitor)
                    Desktop::focusState()->rawMonitorFocus(focusedMonitor);
                for (const auto& view : views)
                    view->refreshWorkspaceLayout();
                reconcileVisibility();
                selection.keyboard({.workspace = gesture.identity, .monitor = destination});
                syncSelection();
                for (const auto& view : views)
                    if (view->monitor() == destination)
                        if (auto target = view->selectedTarget(); target && target->workspace == gesture.identity)
                            selection.keyboard(*target);
                syncSelection();
                beginWorkspaceSettle(std::move(gesture), destination);
            }
        }
        updateCursor();
        damage();
    }

    void COverviewSession::cancelWorkspaceDrag() {
        if (!workspaceDrag)
            return;
        workspaceDrag.reset();
        cancelZoom();
        m_zoomTarget.reset();
        updateCursor();
        damage();
    }

    bool COverviewSession::button(uint32_t button, bool pressed, uint32_t mods) {
        if (pressed)
            cancelWorkspaceSettle();
        if (workspaceDrag) {
            if (!pressed && button == 0x110)
                finishWorkspaceDrag();
            return true;
        }
        if (!pressed && button == 0x110 && m_emptyButtonPressed) {
            m_emptyButtonPressed = false;
            return true;
        }
        if (m_pan)
            return true;
        const auto workspaceMods = config::overviewWorkspaceDragModifiers();
        if (pressed && button == 0x110 && workspaceMods != 0 && mods == workspaceMods) {
            (void)beginWorkspaceDrag();
            return true;
        }
        if (button == 0x112) {
            if (pressed && mods == 0) {
                // Mouse preparation stays on a covered output. Unlike the
                // keyboard action, a click in a monitor gap has no fallback.
                if (auto* view = emptyWorkspaceView(nullptr); view && view->monitor()->logicalBox().containsPoint(g_pInputManager->getMouseCoordsInternal()))
                    (void)emptyWorkspace(view->monitor());
            }
            return true;
        }
        if (pressed && button == 0x110 && mods == 0 && !gestureActive()) {
            if (auto* view = emptyWorkspaceButtonView(m_pointer)) {
                m_emptyButtonPressed = true;
                (void)emptyWorkspace(view->monitor());
                return true;
            }
        }
        if (pressed && button == 0x111 && mods == 0 && zoomHeld() && config::overviewWheelZoom()) {
            if (auto* view = zoomView(); m_cursorOwned && !gestureActive() && view && view->inspectionPanHit(m_pointer)) {
                m_pan = SInspectionPan{m_zoomTarget->workspace, m_zoomTarget->monitor};
                cancelZoomEdge();
                updatePan();
                updateCursor();
            }
            return true;
        }
        if (pressed && (mods & HL_MODIFIER_META) && (button == 0x110 || button == 0x111)) {
            const auto target = hit(m_pointer);
            if (!target || !target->window || gestureActive() || pendingResize)
                return true;
            cancelZoomEdge();
            drag.mode          = button == 0x110 ? SOverviewDrag::MOVE : SOverviewDrag::RESIZE;
            drag.window        = target->window;
            drag.source        = *target;
            drag.pickup        = m_pointer;
            drag.offset        = m_pointer - Vector2D{target->preview.x, target->preview.y};
            drag.desktopOffset = target->desktop - Vector2D{target->desktopBox.x, target->desktopBox.y};
            drag.box           = target->preview;
            drag.threshold     = std::max(0.0, static_cast<double>(*CConfigValue<Config::INTEGER>("binds:drag_threshold")));
            for (const auto& view : views) {
                if (view->monitor() == target->monitor)
                    drag.sourceCell = view->workspaceCell(target->workspace);
                if (zoomHeld())
                    view->freezeZoom();
            }
            drag.scale        = {target->preview.w / target->desktopBox.w, target->preview.h / target->desktopBox.h};
            drag.resizeLeft   = drag.desktopOffset.x < target->desktopBox.w / 2;
            drag.resizeTop    = drag.desktopOffset.y < target->desktopBox.h / 2;
            const auto corner = *CConfigValue<Config::INTEGER>("general:resize_corner");
            if (target->window->m_isFloating && corner >= 1 && corner <= 4) {
                drag.resizeLeft = corner == 1 || corner == 4;
                drag.resizeTop  = corner == 1 || corner == 2;
            }
            for (const auto& view : views)
                if (auto texture = view->textureFor(target->window.lock()))
                    m_dragTexture = texture;
            updateCursor();
            damage();
            return true;
        }
        if (!pressed && drag.active()) {
            if (button != (drag.mode == SOverviewDrag::MOVE ? 0x110U : 0x111U))
                return true;
            const auto window = drag.window.lock();
            if (!window || !window->m_isMapped || window->isHidden() || !valid(window->m_workspace) ||
                SWorkspaceIdentity{window->m_workspace->m_id, window->m_workspace->m_name} != drag.source.workspace || g_layoutManager->dragController()->target()) {
                cancelDrag();
                return true;
            }
            PHLWORKSPACE preparedDestination;
            PHLWINDOW    droppedWindow;
            COverview*   preparedView = nullptr;
            if (drag.moved) {
                auto destination = drag.mode == SOverviewDrag::RESIZE ? std::optional{drag.source} : selection.drop();
                if (drag.mode == SOverviewDrag::MOVE) {
                    if (auto* view = emptyWorkspaceButtonView(m_pointer)) {
                        const auto resolved = resolveEmptyWorkspace(view->monitor(), false);
                        if (resolved) {
                            preparedDestination  = *resolved;
                            preparedView         = view;
                            const auto      mon  = preparedDestination->m_monitor.lock();
                            const auto      work = mon->logicalBoxMinusReserved();
                            SOverviewTarget target{.workspace = {preparedDestination->m_id, preparedDestination->m_name}, .monitor = mon};
                            target.desktop    = work.middle();
                            target.desktopBox = target.monitorBox = {work.x, work.y, work.w, work.h};
                            if (window->m_isFloating) {
                                const auto size    = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
                                const auto extents = window->getFullWindowReservedArea();
                                const auto border  = static_cast<double>(window->getRealBorderSize());
                                const auto centered =
                                    boundedLaunchPlacement({work.middle().x - size.x / 2, work.middle().y - size.y / 2, size.x, size.y}, target.monitorBox,
                                                           {std::max(extents.topLeft.x, border), std::max(extents.topLeft.y, border)},
                                                           {std::max(extents.bottomRight.x, border), std::max(extents.bottomRight.y, border)});
                                if (centered)
                                    target.desktop = Vector2D{centered->x, centered->y} + drag.desktopOffset;
                            }
                            if (const auto box = view->emptyWorkspaceButton())
                                target.preview = target.previewClip = *box;
                            destination = target;
                        } else {
                            view->setEmptyWorkspaceError(resolved.error());
                            destination.reset();
                        }
                    }
                }
                if (destination) {
                    if (drag.mode == SOverviewDrag::RESIZE)
                        destination->desktop = drag.source.desktop + (m_pointer - drag.pickup) / drag.scale;
                    if (hooks::place(drag.window.lock(), drag.source, *destination, drag.mode == SOverviewDrag::RESIZE)) {
                        if (drag.mode == SOverviewDrag::RESIZE)
                            pendingResize = drag;
                        else if (preparedDestination) {
                            droppedWindow = drag.window.lock();
                            retainPreparedWorkspace(preparedDestination);
                            preparedView->setEmptyWorkspaceError("");
                        }
                    } else if (preparedView)
                        preparedView->setEmptyWorkspaceError("hyprspace: could not move this window to an empty workspace");
                }
            }
            drag = {};
            if (preparedView && droppedWindow) {
                cancelZoom();
                preparedView->refreshPreparedWorkspace(preparedDestination, droppedWindow);
            }
            if (!pendingResize)
                m_dragTexture.reset();
            updateCursor();
            updateZoom();
            damage();
            return true;
        }
        return false;
    }

    void COverviewSession::cancelDrag() {
        cancelWorkspaceDrag();
        drag = {};
        pendingResize.reset();
        m_dragTexture.reset();
        hooks::cancelPlacement();
        updateCursor();
        updateZoom();
        damage();
    }
    void COverviewSession::finishPlacement() {
        pendingResize.reset();
        if (!gestureActive())
            m_dragTexture.reset();
        damage();
    }
    SP<Render::ITexture> COverviewSession::dragTexture() const {
        return m_dragTexture;
    }
    void COverviewSession::monitorRemoved(PHLMONITOR mon) {
        cancelWorkspaceSettle();
        std::erase_if(m_preparedWorkspaces, [&](const auto& prepared) { return prepared.monitor == mon; });
        if (m_zoomTarget && m_zoomTarget->monitor == mon) {
            cancelZoom();
            m_zoomTarget.reset();
        }
        cancelDrag();
        if (selection.command() && selection.command()->monitor == mon)
            selection.clear();
    }
    void COverviewSession::damage() {
        for (const auto& view : views)
            view->damage();
    }
} // namespace hyprspace
