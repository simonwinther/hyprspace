#include "WindowBoard.hpp"

#include "Access.hpp"
#include "Config.hpp"
#include "CompositorHooks.hpp"
#include "DesktopDb.hpp"
#include "Overview.hpp"
#include "PassElements.hpp"
#include "PreviewStyle.hpp"
#include "Texture.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <format>

namespace hyprspace {
    namespace {
        uint64_t nextBoardGeneration = 0;

        SBoxF intersection(const SBoxF& a, const SBoxF& b) {
            const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
            return {x, y, std::max(0.0, std::min(a.x + a.w, b.x + b.w) - x), std::max(0.0, std::min(a.y + a.h, b.y + b.h) - y)};
        }

        std::string keyLabel(xkb_keysym_t key) {
            char name[64]{};
            if (key == XKB_KEY_NoSymbol || xkb_keysym_get_name(key, name, sizeof(name)) <= 0)
                return {};
            auto        upper = g_utf8_strup(name, -1);
            std::string label(upper);
            g_free(upper);
            return label;
        }

        CHyprColor fade(const CHyprColor& color, float amount) {
            return color.modifyA(color.a * std::clamp(amount, 0.F, 1.F));
        }

        bool drawableSize(const Vector2D& size) {
            return std::isfinite(size.x) && std::isfinite(size.y) && size.x >= 1 && size.y >= 1;
        }

        Vector2D previewSize(PHLWINDOW window) {
            const auto size = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            return drawableSize(size) ? size : Vector2D{640, 400};
        }
    } // namespace

    CWindowBoard::CWindowBoard(COverviewSession& owner) : m_session(owner) {
        const auto animation = Config::animationTree()->getAnimationPropertyConfig("windowsMove");
        for (auto* value : {&m_openness, &m_reflow, &m_inspectionMix, &m_lensProgress}) {
            Animation::mgr()->createAnimation(0.F, *value, animation, AVARDAMAGE_NONE);
            (*value)->setUpdateCallback([this](auto) { m_session.damage(); });
        }
        m_reflow->setValueAndWarp(1.F);
        m_lensProgress->setValueAndWarp(1.F);
    }

    const CWindowBoard::SItem* CWindowBoard::item(uint64_t key) const {
        const auto found = std::ranges::find_if(m_items, [&](const auto& candidate) { return candidate.input.key == key; });
        return found == m_items.end() ? nullptr : &*found;
    }

    void CWindowBoard::enter(PHLMONITOR monitor, std::optional<EWindowGrouping> grouping) {
        if (!monitor)
            return;
        m_monitor    = monitor;
        m_active     = true;
        m_generation = ++nextBoardGeneration;
        m_query.clear();
        m_searchFocused = false;
        m_scroll        = 0;
        m_recency.clear();
        if (grouping)
            m_grouping = *grouping;
        const auto chosen = m_session.selection.command();
        m_dirty           = true;
        refresh();
        const auto& history = Desktop::History::windowTracker()->fullHistory();
        size_t      rank    = 0;
        for (const auto& ref : history | std::views::reverse)
            for (const auto& candidate : m_items)
                if (candidate.window == ref && !m_recency.contains(candidate.input.key))
                    m_recency[candidate.input.key] = rank++;
        if (chosen) {
            const auto found = std::ranges::find_if(m_items, [&](const auto& candidate) {
                return chosen->window ? candidate.window == chosen->window
                                      : candidate.input.workspace == chosen->workspace.id && candidate.window->m_monitor == chosen->monitor;
            });
            if (found != m_items.end())
                m_selected = found->input.key;
        }
        m_dirty = true;
        refresh();
        select(m_selected);
        *m_openness = 1.F;
        m_session.damage();
    }

    void CWindowBoard::leave() {
        if (!m_active)
            return;
        m_active        = false;
        m_searchFocused = false;
        m_query.clear();
        endPan();
        endInspection();
        *m_openness = 0.F;
        m_session.damage();
    }

    void CWindowBoard::refresh() {
        if (!m_active)
            return;
        if (!monitor() || !m_session.covers(monitor())) {
            m_monitor.reset();
            for (const auto& view : m_session.views)
                if (!view->closing() && view->monitor() && view->monitor()->m_enabled) {
                    m_monitor = view->monitor();
                    break;
                }
            if (!monitor()) {
                leave();
                return;
            }
            m_dirty = true;
        }
        const auto   mon      = monitor();
        const auto&  reserved = mon->m_reservedArea;
        const SBoxF  usable{reserved.left(), reserved.top(), std::max(1.0, mon->m_size.x - reserved.left() - reserved.right()),
                            std::max(1.0, mon->m_size.y - reserved.top() - reserved.bottom())};
        const double padding = std::min<double>(config::overviewPadding(), std::min(usable.w, usable.h) * 0.08);
        const SBoxF  viewport{usable.x + padding, usable.y + padding + 132, std::max(1.0, usable.w - padding * 2), std::max(1.0, usable.h - padding * 2 - 166)};
        const double gap = std::clamp<double>(config::overviewGap(), 8, 32);
        if (m_usable != usable || m_viewport != viewport || m_gap != gap)
            m_dirty = true;
        m_usable   = usable;
        m_viewport = viewport;
        m_gap      = gap;

        std::vector<SItem> fresh;
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!window || !window->m_isMapped || window->isHidden() || !window->m_workspace || !m_session.covers(window->m_monitor.lock()) ||
                (window->m_workspace->m_isSpecialWorkspace && !config::overviewIncludeSpecial()))
                continue;
            const auto     old    = std::ranges::find_if(m_items, [&](const auto& candidate) { return candidate.window == window; });
            const uint64_t key    = old == m_items.end() ? m_nextKey++ : old->input.key;
            const auto     cls    = window->m_class.empty() ? window->m_initialClass : window->m_class;
            const auto     app    = appIdentity(cls);
            const auto     source = window->m_monitor.lock();
            const auto     ws     = window->m_workspace;
            fresh.push_back({{.key           = key,
                              .monitor       = source->m_id,
                              .workspace     = ws->m_id,
                              .monitorName   = source->m_name,
                              .workspaceName = ws->m_name,
                              .appId         = app.id,
                              .appName       = app.name,
                              .appClass      = cls,
                              .title         = window->m_title.empty() ? app.name : window->m_title,
                              .recent        = m_recency.contains(key) ? m_recency.at(key) : std::numeric_limits<size_t>::max()},
                             window});
        }
        std::vector<SWindowViewInput> input;
        for (const auto& candidate : fresh)
            input.push_back(candidate.input);
        m_items = std::move(fresh);
        if (m_input != input) {
            m_input = std::move(input);
            m_dirty = true;
        }
        if (m_dirty)
            rebuild();
        if (const auto chosen = selectedTarget()) {
            const auto previous = m_session.selection.command();
            if (!previous || previous->window != chosen->window || previous->workspace != chosen->workspace || previous->monitor != chosen->monitor)
                m_session.selectBoardTarget(chosen);
        } else {
            m_session.selection.clear();
            if (m_session.zoomHeld())
                m_session.cancelZoom();
        }
    }

    void CWindowBoard::rebuild() {
        const auto                          oldSelected = std::ranges::find(m_layout.cards, m_selected, &SWindowViewCard::key);
        const size_t                        ordinal     = oldSelected == m_layout.cards.end() ? 0 : oldSelected - m_layout.cards.begin();
        std::unordered_map<uint64_t, SBoxF> from;
        for (const auto& card : m_layout.cards)
            from[card.key] = gridBox(card);
        m_from   = std::move(from);
        m_layout = layoutWindowViews(m_input, groupWindowViews(m_input, m_grouping, m_recent, m_query), m_viewport, m_gap);
        if (std::ranges::find(m_layout.cards, m_selected, &SWindowViewCard::key) == m_layout.cards.end())
            m_selected = m_layout.cards.empty() ? 0 : m_layout.cards[std::min(ordinal, m_layout.cards.size() - 1)].key;
        m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, m_layout.height - m_viewport.h));
        ensureVisible();
        m_reflow->setValueAndWarp(0.F);
        *m_reflow = 1.F;
        m_dirty   = false;
        m_session.selectBoardTarget(selectedTarget());
        m_session.damage();
    }

    SBoxF CWindowBoard::gridBox(const SWindowViewCard& card) const {
        auto box = card.box;
        box.y -= m_scroll;
        if (const auto found = m_from.find(card.key); found != m_from.end())
            box = interpolateBox(found->second, box, std::clamp(m_reflow->value(), 0.F, 1.F));
        return box;
    }

    SBoxF CWindowBoard::cardBox(const SWindowViewCard& card) const {
        auto box = gridBox(card);
        if (card.key == m_selected && m_inspectionMix->value() > 0) {
            auto fitted = inspectionFit();
            fitted.h += 52;
            box = interpolateBox(box, fitted, std::clamp(m_inspectionMix->value(), 0.F, 1.F));
        }
        return box;
    }

    SBoxF CWindowBoard::inspectionFit() const {
        const auto chosen = item(m_selected);
        const auto window = chosen ? chosen->window.lock() : nullptr;
        if (!window)
            return m_viewport;
        const auto size = previewSize(window);
        return fitBox({m_viewport.x + 8, m_viewport.y + 8, std::max(1.0, m_viewport.w - 16), std::max(1.0, m_viewport.h - 68)}, size.x / size.y);
    }

    SBoxF CWindowBoard::previewBox(const SItem& candidate, const SBoxF& card) const {
        const auto window = candidate.window.lock();
        if (!window)
            return {};
        const auto size = previewSize(window);
        auto       box  = fitBox({card.x + 4, card.y + 4, std::max(1.0, card.w - 8), std::max(1.0, card.h - 60)}, size.x / size.y);
        if (candidate.input.key == m_selected && m_inspectionMix->value() > 0)
            box = interpolateBox(box, m_lens.current(m_lensProgress->value()).apply(inspectionFit()), std::clamp(m_inspectionMix->value(), 0.F, 1.F));
        return box;
    }

    std::optional<SOverviewTarget> CWindowBoard::target(const SItem& candidate, const SBoxF& preview, std::optional<Vector2D> point) const {
        const auto window = candidate.window.lock();
        const auto host   = monitor();
        if (!host || !window || !window->m_isMapped || window->isHidden() || !window->m_workspace)
            return std::nullopt;
        const auto source = window->m_monitor.lock();
        if (!source || !m_session.covers(source))
            return std::nullopt;
        const auto pos  = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        auto       size = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        // A crowded native layout can collapse a mapped window to zero or
        // negative size. Keep its identity selectable, with a finite command
        // point and an icon placeholder instead of discarding the window.
        size.x                   = std::isfinite(size.x) ? std::max(1.0, size.x) : 1;
        size.y                   = std::isfinite(size.y) ? std::max(1.0, size.y) : 1;
        const auto&     reserved = source->m_reservedArea;
        SOverviewTarget result{.workspace   = {window->m_workspace->m_id, window->m_workspace->m_name},
                               .monitor     = source,
                               .window      = window,
                               .desktop     = pos + size / 2,
                               .preview     = preview,
                               .desktopBox  = {pos.x, pos.y, size.x, size.y},
                               .monitorBox  = {source->m_position.x + reserved.left(), source->m_position.y + reserved.top(),
                                               source->m_size.x - reserved.left() - reserved.right(), source->m_size.y - reserved.top() - reserved.bottom()},
                               .previewClip = m_viewport};
        result.preview.x += host->m_position.x;
        result.preview.y += host->m_position.y;
        result.previewClip.x += host->m_position.x;
        result.previewClip.y += host->m_position.y;
        if (point && result.preview.contains(point->x, point->y))
            if (const auto mapped = mapPreviewPoint({point->x, point->y}, result.preview, result.desktopBox))
                result.desktop = {mapped->x, mapped->y};
        return result;
    }

    std::optional<SOverviewTarget> CWindowBoard::selectedTarget() const {
        const auto card   = std::ranges::find(m_layout.cards, m_selected, &SWindowViewCard::key);
        const auto chosen = item(m_selected);
        if (card == m_layout.cards.end() || !chosen)
            return std::nullopt;
        return target(*chosen, previewBox(*chosen, cardBox(*card)));
    }

    std::optional<SOverviewTarget> CWindowBoard::targetAt(const Vector2D& pos) const {
        const auto mon = monitor();
        if (!m_active || !mon)
            return std::nullopt;
        const auto local = pos - mon->m_position;
        if (!m_viewport.contains(local.x, local.y))
            return std::nullopt;
        for (const auto& card : m_layout.cards) {
            if (m_inspecting && card.key != m_selected)
                continue;
            const auto chosen = item(card.key);
            if (chosen && cardBox(card).contains(local.x, local.y))
                return target(*chosen, previewBox(*chosen, cardBox(card)), pos);
        }
        return std::nullopt;
    }

    std::vector<SOverviewTarget> CWindowBoard::inspectTargets() const {
        std::vector<SOverviewTarget> result;
        for (const auto& card : m_layout.cards)
            if (const auto chosen = item(card.key))
                if (const auto value = target(*chosen, previewBox(*chosen, cardBox(card))))
                    result.push_back(*value);
        return result;
    }

    void CWindowBoard::ensureVisible() {
        const auto card = std::ranges::find(m_layout.cards, m_selected, &SWindowViewCard::key);
        if (card == m_layout.cards.end())
            return;
        if (card->box.y - m_scroll < m_viewport.y)
            m_scroll = card->box.y - m_viewport.y;
        if (card->box.y + card->box.h - m_scroll > m_viewport.y + m_viewport.h)
            m_scroll = card->box.y + card->box.h - m_viewport.y - m_viewport.h;
        m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, m_layout.height - m_viewport.h));
    }

    void CWindowBoard::select(uint64_t key, bool pointer) {
        if (!key || !item(key))
            return;
        if (m_selected != key) {
            m_selected = key;
            m_lens.reset();
            m_lensProgress->setValueAndWarp(1.F);
            endPan();
        }
        ensureVisible();
        m_session.selectBoardTarget(selectedTarget(), pointer);
        m_session.damage();
    }

    void CWindowBoard::selectWindow(PHLWINDOW window) {
        if (const auto found = std::ranges::find_if(m_items, [&](const auto& candidate) { return candidate.window == window; }); found != m_items.end())
            if (std::ranges::find(m_layout.cards, found->input.key, &SWindowViewCard::key) != m_layout.cards.end())
                select(found->input.key);
    }

    void CWindowBoard::setGrouping(EWindowGrouping grouping) {
        m_grouping = grouping;
        m_scroll   = 0;
        m_dirty    = true;
        refresh();
    }

    void CWindowBoard::cycleGrouping() {
        setGrouping(static_cast<EWindowGrouping>((static_cast<int>(m_grouping) + 1) % 4));
    }

    void CWindowBoard::focusSearch() {
        m_session.cancelZoom();
        m_searchFocused = true;
        m_session.damage();
    }

    void CWindowBoard::clearSearch() {
        m_query.clear();
        m_searchFocused = false;
        m_dirty         = true;
        refresh();
    }

    bool CWindowBoard::reservesKey(xkb_keysym_t sym, uint32_t mods, const std::string& text) const {
        if (!m_active)
            return false;
        if (m_searchFocused && mods == HL_MODIFIER_CTRL && sym == XKB_KEY_u)
            return true;
        if (m_searchFocused && (mods & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)) == 0 && !text.empty())
            return true;
        if (mods == HL_MODIFIER_SHIFT)
            return sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab;
        if (mods != 0)
            return false;
        switch (sym) {
        case XKB_KEY_Escape:
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_Tab:
        case XKB_KEY_Left:
        case XKB_KEY_Right:
        case XKB_KEY_Up:
        case XKB_KEY_Down:
        case XKB_KEY_Home:
        case XKB_KEY_End:
        case XKB_KEY_Page_Up:
        case XKB_KEY_Page_Down:
        case XKB_KEY_BackSpace:
        case XKB_KEY_slash:
        case XKB_KEY_r:
            return true;
        default:
            return false;
        }
    }

    void CWindowBoard::commit() {
        if (selectedTarget())
            m_session.commitWindowView();
    }

    void CWindowBoard::onKey(xkb_keysym_t sym, uint32_t mods, const std::string& text) {
        if (!m_active)
            return;
        if (sym == XKB_KEY_Escape) {
            if (m_searchFocused || !m_query.empty())
                clearSearch();
            else
                (void)m_session.windowView("off");
            return;
        }
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            commit();
            return;
        }
        if (m_searchFocused) {
            if (sym == XKB_KEY_BackSpace)
                windowSearchBackspace(m_query);
            else if (mods == HL_MODIFIER_CTRL && sym == XKB_KEY_u)
                m_query.clear();
            else if (!text.empty() && (mods & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)) == 0 && m_query.size() + text.size() <= 1024)
                m_query += text;
            else
                goto navigation;
            m_dirty  = true;
            m_scroll = 0;
            refresh();
            return;
        }
        if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
            (void)m_session.windowView("off");
            if (auto* view = m_session.keyboardView()) {
                view->onKey(sym, mods, true);
                for (const auto& other : m_session.views)
                    other->close(false);
                m_session.stopInput();
            }
            return;
        }
        if (sym == XKB_KEY_slash) {
            focusSearch();
            return;
        }
        if (sym == XKB_KEY_r) {
            m_recent = !m_recent;
            m_dirty  = true;
            refresh();
            return;
        }
        if (sym == XKB_KEY_s || sym == XKB_KEY_space) {
            commit();
            return;
        }
    navigation:
        if (m_layout.cards.empty())
            return;
        const auto found = std::ranges::find(m_layout.cards, m_selected, &SWindowViewCard::key);
        const int  index = found == m_layout.cards.end() ? 0 : found - m_layout.cards.begin();
        int        next  = index;
        if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)
            next = (index + ((mods & HL_MODIFIER_SHIFT) || sym == XKB_KEY_ISO_Left_Tab ? -1 : 1) + m_layout.cards.size()) % m_layout.cards.size();
        else if (sym == XKB_KEY_Home)
            next = 0;
        else if (sym == XKB_KEY_End)
            next = m_layout.cards.size() - 1;
        else if (sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down) {
            updateScroll(m_scroll + (sym == XKB_KEY_Page_Up ? -1 : 1) * m_viewport.h * 0.85);
            return;
        } else {
            std::optional<EDirection> direction;
            if (sym == XKB_KEY_Left || sym == XKB_KEY_h)
                direction = EDirection::LEFT;
            if (sym == XKB_KEY_Right || sym == XKB_KEY_l)
                direction = EDirection::RIGHT;
            if (sym == XKB_KEY_Up || sym == XKB_KEY_k)
                direction = EDirection::UP;
            if (sym == XKB_KEY_Down || sym == XKB_KEY_j)
                direction = EDirection::DOWN;
            if (!direction)
                return;
            std::vector<STile> tiles;
            for (size_t i = 0; i < m_layout.cards.size(); ++i)
                tiles.push_back({.key = i, .box = m_layout.cards[i].box, .order = i});
            next = navigate(tiles, index, *direction);
        }
        select(m_layout.cards[next].key);
    }

    void CWindowBoard::pointer(const Vector2D& pos, bool userMotion) {
        const auto delta = pos - m_pointer;
        m_pointer        = pos;
        if (panning()) {
            if (userMotion)
                panKey(delta);
            return;
        }
        const auto     target  = targetAt(pos);
        const auto     found   = target ? std::ranges::find_if(m_items, [&](const auto& candidate) { return candidate.window == target->window; }) : m_items.end();
        const uint64_t hovered = found == m_items.end() ? 0 : found->input.key;
        if (hovered != m_hovered) {
            m_hovered = hovered;
            m_session.damage();
        }
        if (userMotion && hovered && config::followMouse() && !m_inspecting)
            select(hovered, true);
    }

    void CWindowBoard::updateScroll(double offset) {
        if (!std::isfinite(offset))
            return;
        const double next = std::clamp(offset, 0.0, std::max(0.0, m_layout.height - m_viewport.h));
        if (next == m_scroll)
            return;
        m_scroll = next;
        m_from.clear();
        m_reflow->setValueAndWarp(1.F);
        m_session.damage();
    }

    void CWindowBoard::scroll(const SScrollInput& event, const Vector2D& pos) {
        const auto mon = monitor();
        if (!mon || !m_viewport.contains(pos.x - mon->m_position.x, pos.y - mon->m_position.y) || event.horizontal || !std::isfinite(event.delta))
            return;
        if (m_inspecting && config::overviewWheelZoom()) {
            if (panning())
                return;
            const auto local = pos - mon->m_position;
            if (m_lens.scroll(event, hooks::scrollFactor(), inspectionFit(), local.x, local.y, m_lensProgress->value())) {
                m_lensProgress->setValueAndWarp(0.F);
                *m_lensProgress = 1.F;
                m_session.damage();
            }
        } else
            updateScroll(m_scroll + scrollDistance(event, m_viewport.h) * m_viewport.h * hooks::scrollFactor());
    }

    bool CWindowBoard::beginInspection() {
        if (!selectedTarget() || m_searchFocused)
            return false;
        m_inspecting = true;
        m_lens.reset();
        m_lensProgress->setValueAndWarp(1.F);
        *m_inspectionMix = 1.F;
        m_session.damage();
        return true;
    }

    void CWindowBoard::endInspection() {
        m_inspecting = false;
        endPan();
        *m_inspectionMix = 0.F;
        m_session.damage();
    }

    void CWindowBoard::zoomStep(int direction, std::optional<Vector2D> pos) {
        if (!m_inspecting || panning())
            return;
        const auto mon = monitor();
        if (!mon)
            return;
        const auto fit   = inspectionFit();
        const auto point = pos ? *pos - mon->m_position : Vector2D{fit.cx(), fit.cy()};
        if (m_lens.zoom(-direction, 1, fit, point.x, point.y, m_lensProgress->value())) {
            m_lensProgress->setValueAndWarp(0.F);
            *m_lensProgress = 1.F;
            m_session.damage();
        }
    }

    void CWindowBoard::panKey(const Vector2D& delta) {
        if (!m_inspecting)
            return;
        m_lens.freeze(m_lensProgress->value());
        m_lensProgress->setValueAndWarp(1.F);
        if (m_lens.pan(inspectionFit(), delta.x, delta.y))
            m_session.damage();
    }

    bool CWindowBoard::panAvailable(const Vector2D& pos) const {
        const auto mon = monitor();
        return m_inspecting && mon && m_viewport.contains(pos.x - mon->m_position.x, pos.y - mon->m_position.y) &&
               m_lens.canPan(inspectionFit(), m_lensProgress->value());
    }

    bool CWindowBoard::beginPan(const Vector2D& pos, bool keyboard) {
        if (!panAvailable(pos) || !m_lens.beginPan(inspectionFit(), m_lensProgress->value()))
            return false;
        m_lensProgress->setValueAndWarp(1.F);
        m_keyboardPan = keyboard;
        m_mousePan    = !keyboard;
        return true;
    }

    void CWindowBoard::endPan() {
        m_mousePan = m_keyboardPan = false;
    }

    SBoxF CWindowBoard::searchBox() const {
        return {m_viewport.x, m_viewport.y - 48, m_viewport.w, 36};
    }
    SBoxF CWindowBoard::returnBox() const {
        return {m_viewport.x + std::max(0.0, m_viewport.w - 146), m_viewport.y - 132, std::min(146.0, m_viewport.w), 30};
    }
    SBoxF CWindowBoard::recentBox() const {
        return {m_viewport.x + m_viewport.w - std::min(100.0, m_viewport.w / 5), m_viewport.y - 92, std::min(100.0, m_viewport.w / 5), 32};
    }
    std::array<SBoxF, 4> CWindowBoard::groupingBoxes() const {
        std::array<SBoxF, 4> boxes;
        const double         width = std::min(120.0, (m_viewport.w - recentBox().w - 20) / 4);
        for (size_t i = 0; i < boxes.size(); ++i)
            boxes[i] = {m_viewport.x + i * width, m_viewport.y - 92, std::max(1.0, width - 6), 32};
        return boxes;
    }

    void CWindowBoard::button(uint32_t button, bool pressed, uint32_t mods, const Vector2D& pos) {
        if (!m_active || !monitor())
            return;
        if (!pressed) {
            if (button == 0x111)
                endPan();
            return;
        }
        if (mods != 0)
            return;
        if (button == 0x111) {
            if (!beginPan(pos, false))
                (void)m_session.windowView("off");
            return;
        }
        if (button != 0x110)
            return;
        const auto local = pos - monitor()->m_position;
        if (returnBox().contains(local.x, local.y)) {
            (void)m_session.windowView("off");
            return;
        }
        if (searchBox().contains(local.x, local.y)) {
            focusSearch();
            return;
        }
        if (recentBox().contains(local.x, local.y)) {
            m_recent        = !m_recent;
            m_searchFocused = false;
            m_dirty         = true;
            refresh();
            return;
        }
        const auto boxes = groupingBoxes();
        for (size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i].contains(local.x, local.y)) {
                m_searchFocused = false;
                setGrouping(static_cast<EWindowGrouping>(i));
                return;
            }
        if (const auto hit = targetAt(pos)) {
            selectWindow(hit->window.lock());
            commit();
        }
    }

    bool CWindowBoard::needsBlur(PHLMONITOR mon) const {
        if (!rendering() || monitor() != mon)
            return false;
        return std::ranges::any_of(m_layout.cards, [&](const auto& card) {
            const auto chosen = item(card.key);
            const auto window = chosen ? chosen->window.lock() : nullptr;
            return window && hidden::shouldBlurWindow(window);
        });
    }

    std::vector<UP<IPassElement>> CWindowBoard::buildPass(PHLMONITOR mon) const {
        std::vector<UP<IPassElement>> out;
        if (!mon || !rendering())
            return out;
        const double scale    = mon->m_scale;
        const float  opacity  = progress();
        const auto   ink      = config::overviewLabelColor();
        const auto   accent   = config::overviewActiveBorder();
        const auto   font     = config::overviewFont();
        const auto   viewKey  = keyLabel(config::overviewWindowViewKey());
        const auto   zoomKey  = keyLabel(config::overviewZoomKey());
        const double rounding = config::overviewRounding();
        const auto   px       = [&](const SBoxF& box) { return CBox{box.x * scale, box.y * scale, box.w * scale, box.h * scale}; };
        auto         rect     = [&](SBoxF box, CHyprColor color, double round = 0, bool clip = false) {
            if (clip)
                box = intersection(box, m_viewport);
            if (box.w <= 0 || box.h <= 0)
                return;
            out.emplace_back(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = px(box), .color = color, .round = static_cast<int>(round * scale)}));
        };
        auto border = [&](SBoxF box, CHyprColor color, double width, double round, bool clip = false) {
            if (clip)
                box = intersection(box, m_viewport);
            if (box.w <= 0 || box.h <= 0)
                return;
            out.emplace_back(makeUnique<CBorderPassElement>(CBorderPassElement::SBorderData{.box        = px(box),
                                                                                            .grad1      = Config::CGradientValueData(color),
                                                                                            .round      = static_cast<int>(round * scale),
                                                                                            .borderSize = static_cast<float>(width * scale),
                                                                                            .outerRound = static_cast<int>((round + width) * scale)}));
        };
        auto text = [&](const std::string& label, const SBoxF& box, CHyprColor color, float alpha, bool centered = false, bool clip = false) {
            if (box.w <= 1 || box.h <= 1)
                return;
            const auto texture = textures().text(label, font, color, static_cast<int>(box.w), scale);
            if (!texture)
                return;
            const Vector2D               size = texture->m_size / scale;
            CTexPassElement::SRenderData data{
                .tex = texture, .box = px({box.x + (centered ? (box.w - size.x) / 2 : 0), box.cy() - size.y / 2, size.x, size.y}), .a = alpha};
            if (clip)
                data.clipBox = px(m_viewport);
            out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
        };

        rect({0, 0, mon->m_size.x, mon->m_size.y}, fade(config::overviewBgColor(), opacity * config::overviewBgDim()));
        if (monitor() != mon)
            return out;

        text(std::format("Windows · {}{}", m_layout.cards.size(), m_query.empty() ? "" : std::format(" of {}", m_items.size())),
             {m_viewport.x, m_viewport.y - 132, std::max(1.0, m_viewport.w - 158), 30}, ink, opacity);
        rect(returnBox(), fade(config::overviewTitleBgColor(), opacity * 0.65F), 8);
        text(viewKey.empty() ? "Workspaces" : viewKey + "  Workspaces", returnBox(), ink, opacity, true);
        const auto                       boxes = groupingBoxes();
        const std::array<const char*, 4> labels{"All windows", "Apps", "Workspaces", "Monitors"};
        const auto                       local = m_pointer - mon->m_position;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const bool selected = static_cast<size_t>(m_grouping) == i;
            const bool hovered  = boxes[i].contains(local.x, local.y);
            rect(boxes[i], fade(selected ? accent : config::overviewTitleBgColor(), opacity * (selected ? 0.17F : hovered ? 0.85F : 0.5F)), 8);
            if (selected)
                border(boxes[i], fade(accent, opacity), 1, 8);
            text(labels[i], boxes[i], selected ? accent : ink, opacity, true);
        }
        rect(recentBox(), fade(m_recent ? accent : config::overviewTitleBgColor(), opacity * (m_recent ? 0.17F : 0.5F)), 8);
        if (m_recent)
            border(recentBox(), fade(accent, opacity), 1, 8);
        text("Recent", recentBox(), m_recent ? accent : ink, opacity, true);
        const auto search = searchBox();
        rect(search, fade(config::overviewTitleBgColor(), opacity * 0.85F), 9);
        border(search, fade(m_searchFocused ? accent : config::overviewTileBorderColor(), opacity), m_searchFocused ? 2 : 1, 9);
        text(m_query.empty() ? (m_searchFocused ? "Type to find a window…" : "/  Search titles, apps, workspaces and monitors") : m_query + (m_searchFocused ? " │" : ""),
             {search.x + 12, search.y, search.w - 24, search.h}, m_query.empty() ? fade(ink, 0.6F) : ink, opacity);

        const float inspection = std::clamp(m_inspectionMix->value(), 0.F, 1.F);
        for (const auto& heading : m_layout.headings) {
            auto box = heading.box;
            box.y -= m_scroll;
            text(std::format("{} · {}", heading.label, heading.count), box, ink, opacity * (1 - inspection), false, true);
        }
        auto drawCard = [&](const SWindowViewCard& card) {
            const auto chosen = item(card.key);
            const auto window = chosen ? chosen->window.lock() : nullptr;
            if (!window || !window->m_isMapped || window->isHidden())
                return;
            const bool  selected = card.key == m_selected, hovered = card.key == m_hovered;
            const float alpha = opacity * (selected ? 1 : 1 - inspection);
            if (alpha <= 0.001F)
                return;
            const auto box     = cardBox(card);
            const auto clipped = intersection(box, m_viewport);
            if (clipped.w <= 0 || clipped.h <= 0)
                return;
            rect(box, fade(config::overviewTileBgColor(), alpha), rounding, true);
            const auto           preview = previewBox(*chosen, box);
            SP<Render::ITexture> capture;
            if (drawableSize(window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)))
                for (const auto& source : m_session.views)
                    if (source->monitor() == window->m_monitor) {
                        capture = source->textureFor(window);
                        break;
                    }
            if (capture) {
                const float actual =
                    windowPreviewOpacity(window->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE), window->m_ruleApplicator->opaque().valueOrDefault(), alpha);
                CTexPassElement::SRenderData data{.tex   = capture,
                                                  .box   = px(preview),
                                                  .a     = actual,
                                                  .blurA = alpha,
                                                  .round = static_cast<int>(rounding * scale),
                                                  .blur  = hidden::shouldBlurWindow(window)};
                data.clipBox = px(m_viewport);
                out.emplace_back(makeUnique<CWindowPreviewPassElement>(std::move(data), window));
            } else {
                if (const auto icon = textures().icon(chosen->input.appClass, static_cast<int>(64 * scale)).texture) {
                    CTexPassElement::SRenderData data{.tex = icon, .box = px({preview.cx() - 32, preview.cy() - 32, 64, 64}), .a = alpha, .clipBox = px(m_viewport)};
                    out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
                }
                text("Preview unavailable", {preview.x, preview.cy() + 40, preview.w, 24}, fade(ink, 0.65F), alpha, true, true);
            }
            const SBoxF caption{box.x, box.y + box.h - 52, box.w, 52};
            rect(caption, fade(config::overviewTitleBgColor(), alpha), 0, true);
            if (const auto icon = textures().icon(chosen->input.appClass, static_cast<int>(24 * scale)).texture) {
                CTexPassElement::SRenderData data{.tex = icon, .box = px({caption.x + 12, caption.cy() - 12, 24, 24}), .a = alpha, .clipBox = px(m_viewport)};
                out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
            }
            text(chosen->input.title.empty() ? chosen->input.appName : chosen->input.title, {caption.x + 46, caption.y + 2, caption.w - 58, 24}, ink, alpha, false, true);
            std::string location = workspaceLabel(chosen->input.workspace, chosen->input.workspaceName);
            if (m_session.views.size() > 1)
                location += " · " + chosen->input.monitorName;
            const auto fullscreen = Fullscreen::controller()->getFullscreenModes(window).internal;
            if (fullscreen != Fullscreen::FSMODE_NONE)
                location += fullscreen == Fullscreen::FSMODE_FULLSCREEN ? " · fullscreen" : " · maximized";
            text(location, {caption.x + 46, caption.y + 25, caption.w - 58, 23}, fade(ink, 0.65F), alpha, false, true);
            border(box,
                   fade(selected  ? accent
                        : hovered ? config::overviewHoverBorder()
                                  : config::overviewTileBorderColor(),
                        alpha),
                   selected || hovered ? std::max(1, config::overviewBorderSize()) : 1, rounding, true);
        };
        // Inspection can overlap its old neighbors; draw the selected card last.
        for (const auto& card : m_layout.cards)
            if (card.key != m_selected)
                drawCard(card);
        if (const auto selected = std::ranges::find(m_layout.cards, m_selected, &SWindowViewCard::key); selected != m_layout.cards.end())
            drawCard(*selected);

        if (m_layout.cards.empty()) {
            text(m_items.empty() ? "No windows on these outputs" : "No matching windows", {m_viewport.x, m_viewport.cy() - 20, m_viewport.w, 40}, ink, opacity, true);
            if (!m_query.empty())
                text("Escape clears the search", {m_viewport.x, m_viewport.cy() + 20, m_viewport.w, 28}, fade(ink, 0.6F), opacity, true);
        }
        const auto hints = m_inspecting ? std::format("Release {} to return · Wheel / + − zoom · Space or right-drag to pan", zoomKey)
                                        : (viewKey.empty() ? "" : std::format("{}  Workspaces    Shift+{}  Group    ", viewKey, viewKey)) + "/  Search    R  Recent    " +
                                              (zoomKey.empty() ? "" : std::format("Hold {}  Inspect    ", zoomKey)) + "Enter  Open";
        text(hints, {m_viewport.x, m_viewport.y + m_viewport.h + 8, m_viewport.w, 26}, fade(ink, 0.65F), opacity, true);
        if (m_layout.height > m_viewport.h && inspection < 0.01F) {
            const double height = std::max(24.0, m_viewport.h * m_viewport.h / m_layout.height);
            rect({m_viewport.x + m_viewport.w + 8, m_viewport.y + m_scroll / std::max(1.0, m_layout.height - m_viewport.h) * (m_viewport.h - height), 3, height},
                 fade(ink, opacity * 0.5F), 1.5);
        }
        return out;
    }
} // namespace hyprspace
