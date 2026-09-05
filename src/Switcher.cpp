#include "Switcher.hpp"

#include "Config.hpp"
#include "Focus.hpp"
#include "Texture.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <format>
#include <unordered_set>

namespace hyprspace {

    CSwitcher::CSwitcher(PHLMONITOR monitor, bool forward) : m_monitor(monitor) {
        collectWindows(forward);
        layoutPanel(true);

        Animation::mgr()->createAnimation(0.F, m_alpha, Config::animationTree()->getAnimationPropertyConfig("fadeIn"), AVARDAMAGE_NONE);
        m_alpha->setUpdateCallback([this](auto) { damage(); });
        m_alpha->setValueAndWarp(0.F);
        *m_alpha = 1.F;

        damage();
    }

    void CSwitcher::collectWindows(bool forward) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        // Hyprland's tracker is ordered oldest -> newest, so walking it backwards
        // gives most-recently-used first, which is what Alt+Tab expects.
        const auto& HISTORY = Desktop::History::windowTracker()->fullHistory();

        auto gather = [&](bool wsOnly) {
            auto eligible = [&](const PHLWINDOW& w) {
                if (!w || !w->m_isMapped || w->isHidden())
                    return false;
                if (!w->m_workspace)
                    return false;
                if (wsOnly && w->m_workspace != MONITOR->m_activeWorkspace)
                    return false;
                return true;
            };

            std::vector<PHLWINDOW>                            out;
            std::unordered_set<const Desktop::View::CWindow*> seen;
            out.reserve(HISTORY.size());
            seen.reserve(HISTORY.size());

            for (const auto& ref : HISTORY | std::views::reverse) {
                const auto W = ref.lock();
                if (eligible(W) && seen.insert(W.get()).second)
                    out.push_back(W);
            }

            // Anything the tracker has not seen yet still belongs in the list.
            for (const auto& w : Desktop::windowState()->windows()) {
                if (eligible(w) && seen.insert(w.get()).second)
                    out.push_back(w);
            }

            return out;
        };

        const auto ordered = gather(config::switcherCurrentWorkspaceOnly());

        for (const auto& w : ordered) {
            SEntry e;
            e.window   = w;
            e.title    = w->m_title.empty() ? w->m_class : w->m_title;
            e.appClass = w->m_class.empty() ? w->m_initialClass : w->m_class;
            m_entries.push_back(e);
        }

        if (m_entries.empty())
            return;

        // Index 0 is the currently focused window, so the first press should
        // already land on the previous one.
        const int N = static_cast<int>(m_entries.size());
        m_selected  = forward ? (1 % N) : ((N - 1) % N);
    }

    void CSwitcher::layoutPanel(bool measureTitles) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || m_entries.empty())
            return;

        // Size to the initial titles, then keep the width stable while picking.
        // Live title changes must not move icons out from under the pointer or
        // measure every window again on each update or page change.
        if (measureTitles) {
            m_titleWidth    = 0.0;
            const auto FONT = config::switcherFont();
            for (const auto& e : m_entries) {
                if (e.title.empty() || !config::switcherShowTitle())
                    continue;
                int tw = 0, th = 0;
                measureText(e.title, FONT, tw, th);
                m_titleWidth = std::max(m_titleWidth, static_cast<double>(tw));
            }
        }

        m_layoutSize = MONITOR->m_size;
        m_layout     = switcherLayout(static_cast<int>(m_entries.size()), m_selected,
                                      SSwitcherLayoutParams{
                                          .screenW     = m_layoutSize.x,
                                          .screenH     = m_layoutSize.y,
                                          .iconSize    = static_cast<double>(config::switcherIconSize()),
                                          .padding     = static_cast<double>(config::switcherPadding()),
                                          .gap         = static_cast<double>(config::switcherGap()),
                                          .titleWidth  = config::switcherShowTitle() ? m_titleWidth : 0.0,
                                          .titleHeight = config::switcherShowTitle() ? 34.0 : 0.0,
                                      });
    }

    void CSwitcher::selectIndex(int index) {
        if (m_closing || index < 0 || index >= static_cast<int>(m_entries.size()) || index == m_selected)
            return;

        m_selected = index;
        if (index < m_layout.first || index >= m_layout.first + m_layout.capacity) {
            m_hovered = -1;
            layoutPanel();
        }
        damage();
    }

    void CSwitcher::advance(bool forward) {
        if (m_entries.empty() || m_closing)
            return;

        const int N = static_cast<int>(m_entries.size());
        selectIndex(((m_selected + (forward ? 1 : -1)) % N + N) % N);
    }

    void CSwitcher::refreshWindows() {
        if (m_closing || m_entries.empty())
            return;

        const auto SELECTED      = m_entries[m_selected].window.lock();
        bool       layoutChanged = std::erase_if(m_entries, [](const SEntry& entry) {
                                 const auto WINDOW = entry.window.lock();
                                 return !WINDOW || !WINDOW->m_isMapped || WINDOW->isHidden() || !WINDOW->m_workspace;
                                   }) > 0;

        if (m_entries.empty()) {
            close(false);
            return;
        }

        // Preserve the selected window when an earlier entry disappears.
        const auto selected = std::ranges::find_if(m_entries, [&](const SEntry& entry) { return entry.window.lock() == SELECTED; });
        m_selected          = selected != m_entries.end() ? static_cast<int>(selected - m_entries.begin()) : std::min(m_selected, static_cast<int>(m_entries.size()) - 1);

        bool contentChanged = false;
        for (auto& entry : m_entries) {
            const auto  WINDOW = entry.window.lock();
            const auto& TITLE  = WINDOW->m_title.empty() ? WINDOW->m_class : WINDOW->m_title;
            const auto& CLASS  = WINDOW->m_class.empty() ? WINDOW->m_initialClass : WINDOW->m_class;
            if (entry.title != TITLE || entry.appClass != CLASS) {
                entry.title    = TITLE;
                entry.appClass = CLASS;
                contentChanged = true;
            }
        }

        if (const auto MONITOR = m_monitor.lock(); MONITOR && MONITOR->m_size != m_layoutSize)
            layoutChanged = true;

        if (layoutChanged) {
            m_hovered = -1;
            layoutPanel();
        }
        if (layoutChanged || contentChanged)
            damage();
    }

    void CSwitcher::reconfigure() {
        layoutPanel(true);
        m_hovered = -1;
        m_scroll.reset();
        damage();
    }

    void CSwitcher::close(bool commitSelection) {
        if (m_closing)
            return;

        m_closing = true;

        if (commitSelection)
            commit();

        *m_alpha = 0.F;
        damage();
    }

    void CSwitcher::commit() {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_entries.size()))
            return;

        focusSelection(m_entries[m_selected].window.lock(), config::warpCursor());
    }

    void CSwitcher::closeSelection() {
        if (m_closing || m_selected < 0 || m_selected >= static_cast<int>(m_entries.size()))
            return;

        const auto WINDOW = m_entries[m_selected].window.lock();
        if (WINDOW) {
            // Address the highlighted window explicitly. It never becomes the
            // active window, so closing something on another workspace does not
            // move the user's real desktop underneath the switcher.
            if (!Config::Actions::closeWindow(WINDOW))
                return;
        }

        m_entries.erase(m_entries.begin() + m_selected);
        m_hovered = -1;

        if (m_entries.empty()) {
            close(false);
            return;
        }

        m_selected = std::min(m_selected, static_cast<int>(m_entries.size()) - 1);
        layoutPanel();
        damage();
    }

    // ---------------------------------------------------------------- input --

    bool CSwitcher::onKey(xkb_keysym_t sym, uint32_t mods, bool pressed) {
        if (!pressed || m_closing || m_entries.empty())
            return true;

        m_scroll.reset();
        const bool SHIFT = mods & HL_MODIFIER_SHIFT;

        switch (sym) {
        case XKB_KEY_Escape:
            close(false);
            return true;

        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            close(true);
            return true;

        case XKB_KEY_Tab:
            advance(!SHIFT);
            return true;
        case XKB_KEY_ISO_Left_Tab:
            advance(false);
            return true;

        case XKB_KEY_Left:
            advance(false);
            return true;
        case XKB_KEY_Right:
            advance(true);
            return true;

        case XKB_KEY_Up:
            selectIndex(switcherRowStep(static_cast<int>(m_entries.size()), m_selected, m_layout.columns, -1));
            return true;
        case XKB_KEY_Down:
            selectIndex(switcherRowStep(static_cast<int>(m_entries.size()), m_selected, m_layout.columns, 1));
            return true;
        case XKB_KEY_Home:
            selectIndex(0);
            return true;
        case XKB_KEY_End:
            selectIndex(static_cast<int>(m_entries.size()) - 1);
            return true;
        case XKB_KEY_Page_Up:
        case XKB_KEY_Page_Down: {
            const int64_t offset = sym == XKB_KEY_Page_Down ? m_layout.capacity : -static_cast<int64_t>(m_layout.capacity);
            selectIndex(static_cast<int>(std::clamp<int64_t>(m_selected + offset, 0, static_cast<int64_t>(m_entries.size()) - 1)));
            return true;
        }

        case XKB_KEY_w:
        case XKB_KEY_W:
            closeSelection();
            return true;

        case XKB_KEY_grave:
        case XKB_KEY_asciitilde:
            advance(!SHIFT);
            return true;

        default:
            break;
        }

        return true;
    }

    void CSwitcher::onModifiersChanged(uint32_t mods) {
        // Releasing Alt is what commits a GNOME-style switcher.
        if (!(mods & HL_MODIFIER_ALT))
            close(true);
    }

    int CSwitcher::entryAt(const Vector2D& globalPos) const {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return -1;

        const auto LOCAL = globalPos - MONITOR->m_position;

        // Hit test the whole cell, not just the icon.
        //
        // The icons sit a gap apart, so testing the icon box alone leaves a
        // dead strip between every pair: dragging along the row drops the
        // highlight in each gap and the selection appears to stutter and catch
        // on nothing. Grow each box by half the gap so the row is continuous.
        const double PAD = m_layout.gap / 2.0;

        for (const auto& tile : m_layout.tiles) {
            const auto& b = tile.box;
            if (LOCAL.x >= b.x - PAD && LOCAL.x < b.x + b.w + PAD && LOCAL.y >= b.y - PAD && LOCAL.y < b.y + b.h + PAD)
                return static_cast<int>(tile.key);
        }

        return -1;
    }

    void CSwitcher::onMouseMove(const Vector2D& globalPos) {
        const int hit = entryAt(globalPos);
        if (hit == m_hovered)
            return;

        m_hovered = hit;
        if (hit >= 0 && config::followMouse()) {
            m_scroll.reset();
            selectIndex(hit);
        }

        damage();
    }

    void CSwitcher::onScroll(const SScrollInput& event) {
        if (m_entries.empty() || m_closing)
            return;

        const int STEPS = m_scroll.steps(event);
        if (STEPS == 0)
            return;
        const int N = static_cast<int>(m_entries.size());
        selectIndex(static_cast<int>(((static_cast<int64_t>(m_selected) + STEPS) % N + N) % N));
    }

    bool CSwitcher::onMouseButton(uint32_t button, bool pressed) {
        constexpr uint32_t MOUSE_LEFT = 0x110;

        if (!pressed)
            return true;

        m_scroll.reset();
        if (button == MOUSE_LEFT) {
            const int HIT = entryAt(g_pInputManager->getMouseCoordsInternal());
            if (HIT >= 0)
                selectIndex(HIT);
            close(HIT >= 0);
        }

        return true;
    }

    // --------------------------------------------------------------- render --

    void CSwitcher::damage() {
        if (auto m = m_monitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    std::vector<UP<IPassElement>> CSwitcher::buildPass() {
        std::vector<UP<IPassElement>> out;

        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || m_entries.empty() || m_layout.tiles.empty())
            return out;

        const float A = std::clamp(m_alpha->value(), 0.F, 1.F);
        if (A < 0.01F)
            return out;

        const auto   FONT     = config::switcherFont();
        const int    ROUNDING = config::switcherRounding();
        const double SCALE    = MONITOR->m_scale;
        const int    ICON     = std::max(1, static_cast<int>(std::ceil(m_layout.iconSize * SCALE)));
        const CBox   CLIP     = {m_layout.panel.x * SCALE, m_layout.panel.y * SCALE, m_layout.panel.w * SCALE, m_layout.panel.h * SCALE};

        auto rect = [&](const SBoxF& b, const CHyprColor& col, int round) {
            out.emplace_back(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
                .box = CBox{b.x * SCALE, b.y * SCALE, b.w * SCALE, b.h * SCALE}, .color = col, .round = static_cast<int>(round * SCALE), .clipBox = CLIP}));
        };
        auto tex = [&](SP<Render::ITexture> t, const CBox& box, float a) {
            out.emplace_back(makeUnique<CTexPassElement>(
                CTexPassElement::SRenderData{.tex = t, .box = CBox{box.x * SCALE, box.y * SCALE, box.w * SCALE, box.h * SCALE}, .a = a, .clipBox = CLIP}));
        };

        // --- panel -------------------------------------------------------------
        const auto BG = config::switcherBgColor();
        rect(m_layout.panel, BG.modifyA(BG.a * A), ROUNDING);

        // --- selection highlight ----------------------------------------------
        const auto selected = std::ranges::find(m_layout.tiles, static_cast<size_t>(m_selected), &STile::key);
        if (selected != m_layout.tiles.end()) {
            const auto&  sel  = selected->box;
            const double grow = 10.0;

            const auto HL = config::switcherHighlightColor();
            rect(SBoxF{sel.x - grow, sel.y - grow, sel.w + grow * 2, sel.h + grow * 2}, HL.modifyA(HL.a * A), ROUNDING / 2);
        }

        // --- icons -------------------------------------------------------------
        bool pendingIcons = false;
        for (const auto& tile : m_layout.tiles) {
            const auto& e = m_entries[tile.key];

            auto icon = textures().icon(e.appClass, ICON);
            pendingIcons |= icon.pending;
            if (!icon.texture)
                continue;

            // Unselected entries sit back slightly, as in GNOME's switcher.
            const float ALPHA = (static_cast<int>(tile.key) == m_selected ? 1.F : 0.65F) * A;
            tex(icon.texture, CBox{tile.box.x, tile.box.y, tile.box.w, tile.box.h}, ALPHA);
        }

        // --- title of the selected entry ---------------------------------------
        if (config::switcherShowTitle() && m_layout.title.h > 0 && m_selected >= 0 && m_selected < static_cast<int>(m_entries.size())) {
            const auto& title = m_entries[m_selected].title;

            if (!title.empty()) {
                auto t = textures().text(title, FONT, config::switcherTextColor(), std::max(1, static_cast<int>(m_layout.title.w)), SCALE);
                if (t) {
                    const auto SIZE = t->m_size / SCALE;
                    tex(t, CBox{m_layout.title.cx() - SIZE.x / 2, m_layout.title.cy() - SIZE.y / 2, SIZE.x, SIZE.y}, A);
                }
            }
        }

        if (m_layout.pages > 1) {
            const auto LABEL = std::format("{} / {}", m_layout.page + 1, m_layout.pages);
            auto       t     = textures().text(LABEL, FONT, config::switcherTextColor(), std::max(1, static_cast<int>(m_layout.pageLabel.w)), SCALE);
            if (t) {
                const auto SIZE = t->m_size / SCALE;
                tex(t, CBox{m_layout.pageLabel.cx() - SIZE.x / 2, m_layout.pageLabel.cy() - SIZE.y / 2, SIZE.x, SIZE.y}, A * 0.6F);
            }
        }

        // Discovery is deliberately asynchronous. Keep requesting inexpensive
        // frames only while visible provisional icons remain. Icons on other
        // pages must not keep the switcher redrawing after discovery finishes.
        if (pendingIcons)
            damage();

        return out;
    }

} // namespace hyprspace
