#include "OverviewSession.hpp"

#include "CompositorHooks.hpp"
#include "Overview.hpp"

#include <hyprland/src/config/ConfigValue.hpp>

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

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
        selection.clear();
        m_pointer = g_pInputManager->getMouseCoordsInternal();
        pointer(m_pointer);
        if (!selection.command() && !views.empty())
            keyboard(*views.front());
        ownCursor(true);
        hooks::syncKeyboardFocus();
    }

    void COverviewSession::stopInput() {
        cancelDrag();
        hooks::cancelPlacement();
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
        if (g_overviewSession.get() == this && !live() && !g_pSessionLockManager->isSessionLocked())
            g_pInputManager->simulateMouseMovement();
    }

    bool COverviewSession::covers(PHLMONITOR monitor) const {
        return monitor && std::ranges::any_of(views, [&](const auto& view) { return view->monitor() == monitor; });
    }

    void COverviewSession::reconcileVisibility() {
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
        if (own == m_cursorOwned)
            return;
        m_cursorOwned = own;
        if (own)
            g_pSeatManager->setPointerFocus(nullptr, {});
        hooks::ownCursor(own);
    }

    std::optional<SOverviewTarget> COverviewSession::hit(const Vector2D& pos) const {
        for (const auto& view : views)
            if (!view->closing())
                if (auto target = view->targetAt(pos))
                    return target;
        return std::nullopt;
    }

    void COverviewSession::pointer(const Vector2D& pos) {
        m_pointer = pos;
        selection.pointer(hit(pos));
        for (const auto& view : views)
            if (!view->closing())
                view->onMouseMove(pos);
        if (drag.active()) {
            const auto w = drag.window.lock();
            if (!w || !w->m_isMapped) {
                cancelDrag();
                return;
            }
            const auto delta = pos - drag.pickup;
            drag.moved |= delta.size() >= 5;
            if (drag.mode == SOverviewDrag::MOVE) {
                drag.box.x = pos.x - drag.offset.x;
                drag.box.y = pos.y - drag.offset.y;
            } else {
                const auto desktopDelta = delta / drag.scale;
                if (const auto box = hooks::resizeGeometry(w, drag.source.desktopBox, {desktopDelta.x, desktopDelta.y}, drag.resizeLeft, drag.resizeTop)) {
                    const auto point = mapPreviewPoint({box->x, box->y}, drag.source.desktopBox, drag.source.preview);
                    if (point)
                        drag.box = {point->x, point->y, box->w * drag.scale.x, box->h * drag.scale.y};
                }
            }
        }
        damage();
    }

    void COverviewSession::keyboard(COverview& view) {
        if (auto target = view.selectedTarget())
            selection.keyboard(*target);
        damage();
    }

    void COverviewSession::refreshPointerTarget() {
        if (!live() || !m_cursorOwned)
            return;
        selection.refresh(hit(m_pointer));
        if (selection.followsPointer())
            for (const auto& view : views)
                if (!view->closing())
                    view->onMouseMove(m_pointer);
    }

    COverview* COverviewSession::keyboardView() const {
        if (const auto& target = selection.command(); target)
            for (const auto& view : views)
                if (!view->closing() && view->monitor() == target->monitor)
                    return view.get();
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

    bool COverviewSession::button(uint32_t button, bool pressed, uint32_t mods) {
        if (pressed && (mods & HL_MODIFIER_META) && (button == 0x110 || button == 0x111)) {
            const auto target = hit(m_pointer);
            if (!target || !target->window || drag.active())
                return true;
            drag.mode          = button == 0x110 ? SOverviewDrag::MOVE : SOverviewDrag::RESIZE;
            drag.window        = target->window;
            drag.source        = *target;
            drag.pickup        = m_pointer;
            drag.offset        = m_pointer - Vector2D{target->preview.x, target->preview.y};
            drag.desktopOffset = target->desktop - Vector2D{target->desktopBox.x, target->desktopBox.y};
            drag.box           = target->preview;
            drag.scale         = {target->preview.w / target->desktopBox.w, target->preview.h / target->desktopBox.h};
            drag.resizeLeft    = drag.desktopOffset.x < target->desktopBox.w / 2;
            drag.resizeTop     = drag.desktopOffset.y < target->desktopBox.h / 2;
            const auto corner  = *CConfigValue<Config::INTEGER>("general:resize_corner");
            if (target->window->m_isFloating && corner >= 1 && corner <= 4) {
                drag.resizeLeft = corner == 1 || corner == 4;
                drag.resizeTop  = corner == 1 || corner == 2;
            }
            for (const auto& view : views)
                if (auto texture = view->textureFor(target->window.lock()))
                    m_dragTexture = texture;
            damage();
            return true;
        }
        if (!pressed && drag.active()) {
            if (button != (drag.mode == SOverviewDrag::MOVE ? 0x110U : 0x111U))
                return true;
            if (drag.moved) {
                auto destination = drag.mode == SOverviewDrag::RESIZE ? std::optional{drag.source} : selection.drop();
                if (destination) {
                    if (drag.mode == SOverviewDrag::RESIZE)
                        destination->desktop = drag.source.desktop + (m_pointer - drag.pickup) / drag.scale;
                    hooks::place(drag.window.lock(), drag.source, *destination, drag.mode == SOverviewDrag::RESIZE);
                }
            }
            cancelDrag();
            return true;
        }
        return false;
    }

    void COverviewSession::cancelDrag() {
        drag = {};
        m_dragTexture.reset();
        damage();
    }
    SP<Render::ITexture> COverviewSession::dragTexture() const {
        return m_dragTexture;
    }
    void COverviewSession::monitorRemoved(PHLMONITOR mon) {
        cancelDrag();
        hooks::cancelPlacement();
        if (selection.command() && selection.command()->monitor == mon)
            selection.clear();
    }
    void COverviewSession::damage() {
        for (const auto& view : views)
            view->damage();
    }
} // namespace hyprspace
