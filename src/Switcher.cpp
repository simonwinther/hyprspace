#include "Switcher.hpp"

#include "Config.hpp"
#include "Focus.hpp"
#include "Texture.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>

namespace hyprspace {

    CSwitcher::CSwitcher(PHLMONITOR monitor, bool forward) : m_monitor(monitor) {
        collectWindows(forward);
        layoutPanel();

        g_pAnimationManager->createAnimation(0.F, m_alpha, Config::animationTree()->getAnimationPropertyConfig("fadeIn"), AVARDAMAGE_NONE);
        m_alpha->setUpdateCallback([this](auto) { damage(); });
        m_alpha->setValueAndWarp(0.F);
        *m_alpha = 1.F;

        damage();
    }

    CSwitcher::~CSwitcher() {
        textures().clear();
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
                if (!w || !w->m_isMapped || w->m_fadingOut || w->isHidden())
                    return false;
                if (!w->m_workspace)
                    return false;
                if (wsOnly && w->m_workspace != MONITOR->m_activeWorkspace)
                    return false;
                return true;
            };

            std::vector<PHLWINDOW> out;

            for (const auto& ref : HISTORY | std::views::reverse) {
                const auto W = ref.lock();
                if (eligible(W) && std::ranges::find(out, W) == out.end())
                    out.push_back(W);
            }

            // Anything the tracker has not seen yet still belongs in the list.
            for (const auto& w : g_pCompositor->m_windows) {
                if (eligible(w) && std::ranges::find(out, w) == out.end())
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

    void CSwitcher::layoutPanel() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || m_entries.empty())
            return;

        const double ICON    = config::switcherIconSize();
        const double PAD     = config::switcherPadding();
        const double GAP     = config::switcherGap();
        const double CELL    = ICON + GAP;
        const bool   TITLE   = config::switcherShowTitle();
        const double TITLE_H = TITLE ? 34.0 : 0.0;

        const double SCREEN_W = MONITOR->m_size.x;
        const double SCREEN_H = MONITOR->m_size.y;

        // Wrap into rows if a single row would not fit on screen.
        const double maxRowW = SCREEN_W - 2 * PAD - 80;
        const int    perRow  = std::max(1, static_cast<int>((maxRowW + GAP) / CELL));
        const int    n       = static_cast<int>(m_entries.size());
        const int    cols    = std::min(n, perRow);
        const int    rows    = (n + cols - 1) / cols;

        const double gridW = cols * ICON + (cols - 1) * GAP;
        const double gridH = rows * ICON + (rows - 1) * GAP;

        // Widen the panel until the titles actually fit.
        //
        // Sizing it from the icon grid alone gives a three-window switcher
        // about 300px of title, which turns every real window title into
        // "Build h..." — the icons already said which app it is, so a title
        // that cannot show what distinguishes two windows of the same app is
        // dead weight. Measure the longest one and let the panel grow to it,
        // capped so a pathological title cannot span the whole screen.
        double contentW = gridW;

        if (TITLE) {
            const std::string FONT     = config::switcherFont();
            const double      MAX_TEXT = SCREEN_W * 0.66;

            double widest = 0.0;
            for (const auto& e : m_entries) {
                if (e.title.empty())
                    continue;

                int tw = 0, th = 0;
                measureText(e.title, FONT, tw, th);
                widest = std::max(widest, static_cast<double>(tw));
            }

            contentW = std::max(contentW, std::min(widest, MAX_TEXT));
        }

        const double panelW = contentW + 2 * PAD;
        const double panelH = gridH + 2 * PAD + TITLE_H;

        m_panel     = SBoxF{(SCREEN_W - panelW) / 2.0, (SCREEN_H - panelH) / 2.0, panelW, panelH};
        m_titleArea = SBoxF{m_panel.x + PAD, m_panel.y + PAD + gridH + 4, contentW, TITLE_H - 4};

        for (int i = 0; i < n; ++i) {
            const int    row   = i / cols;
            const int    col   = i % cols;
            const int    inRow = std::min(cols, n - row * cols);
            const double rowW  = inRow * ICON + (inRow - 1) * GAP;
            const double rowX  = m_panel.x + (panelW - rowW) / 2.0;

            m_entries[i].box = SBoxF{rowX + col * CELL, m_panel.y + PAD + row * CELL, ICON, ICON};
        }
    }

    void CSwitcher::advance(bool forward) {
        if (m_entries.empty() || m_closing)
            return;

        const int N = static_cast<int>(m_entries.size());
        m_selected  = ((m_selected + (forward ? 1 : -1)) % N + N) % N;
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
        if (!pressed)
            return true;

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

    void CSwitcher::onMouseMove(const Vector2D& globalPos) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const auto LOCAL = globalPos - MONITOR->m_position;

        // Hit test the whole cell, not just the icon.
        //
        // The icons sit a gap apart, so testing the icon box alone leaves a
        // dead strip between every pair: dragging along the row drops the
        // highlight in each gap and the selection appears to stutter and catch
        // on nothing. Grow each box by half the gap so the row is continuous.
        const double PAD = config::switcherGap() / 2.0;

        int hit = -1;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& b = m_entries[i].box;
            if (LOCAL.x >= b.x - PAD && LOCAL.x <= b.x + b.w + PAD && LOCAL.y >= b.y - PAD && LOCAL.y <= b.y + b.h + PAD) {
                hit = static_cast<int>(i);
                break;
            }
        }

        if (hit == m_hovered)
            return;

        m_hovered = hit;
        if (hit >= 0)
            m_selected = hit;

        damage();
    }

    void CSwitcher::onScroll(double delta) {
        if (m_entries.empty() || m_closing || delta == 0.0)
            return;

        advance(delta > 0.0);
    }

    bool CSwitcher::onMouseButton(uint32_t button, bool pressed) {
        constexpr uint32_t MOUSE_LEFT = 0x110;

        if (!pressed)
            return true;

        if (button == MOUSE_LEFT)
            close(m_hovered >= 0);

        return true;
    }

    // --------------------------------------------------------------- render --

    void CSwitcher::damage() {
        if (auto m = m_monitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    std::vector<UP<IPassElement>> CSwitcher::buildPass() {
        std::vector<UP<IPassElement>> out;

        if (m_entries.empty())
            return out;

        const float A = std::clamp(m_alpha->value(), 0.F, 1.F);
        if (A < 0.01F)
            return out;

        const auto FONT     = config::switcherFont();
        const int  ROUNDING = config::switcherRounding();
        const int  ICON     = config::switcherIconSize();

        auto rect = [&](const SBoxF& b, const CHyprColor& col, int round) {
            out.emplace_back(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = CBox{b.x, b.y, b.w, b.h}, .color = col, .round = round}));
        };
        auto tex = [&](SP<Render::ITexture> t, const CBox& box, float a) {
            out.emplace_back(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{.tex = t, .box = box, .a = a}));
        };

        // --- panel -------------------------------------------------------------
        const auto BG = config::switcherBgColor();
        rect(m_panel, BG.modifyA(BG.a * A), ROUNDING);

        // --- selection highlight ----------------------------------------------
        if (m_selected >= 0 && m_selected < static_cast<int>(m_entries.size())) {
            const auto&  sel  = m_entries[m_selected].box;
            const double grow = 10.0;

            const auto HL = config::switcherHighlightColor();
            rect(SBoxF{sel.x - grow, sel.y - grow, sel.w + grow * 2, sel.h + grow * 2}, HL.modifyA(HL.a * A), ROUNDING / 2);
        }

        // --- icons -------------------------------------------------------------
        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& e = m_entries[i];

            auto icon = textures().icon(e.appClass, ICON);
            if (!icon)
                continue;

            // Unselected entries sit back slightly, as in GNOME's switcher.
            const float ALPHA = (static_cast<int>(i) == m_selected ? 1.F : 0.65F) * A;
            tex(icon, CBox{e.box.x, e.box.y, e.box.w, e.box.h}, ALPHA);
        }

        // --- title of the selected entry ---------------------------------------
        if (config::switcherShowTitle() && m_selected >= 0 && m_selected < static_cast<int>(m_entries.size())) {
            const auto& title = m_entries[m_selected].title;

            if (!title.empty()) {
                auto t = textures().text(title, FONT, config::switcherTextColor(), static_cast<int>(m_titleArea.w));
                if (t) {
                    tex(t, CBox{m_titleArea.x + (m_titleArea.w - t->m_size.x) / 2.0, m_titleArea.y + (m_titleArea.h - t->m_size.y) / 2.0, t->m_size.x, t->m_size.y}, A);
                }
            }
        }

        // Discovery is deliberately asynchronous. Keep requesting inexpensive
        // frames only while provisional icons remain, so completed results can
        // replace them without ever blocking the first Alt+Tab.
        if (textures().hasPendingIcons())
            damage();

        return out;
    }

} // namespace hyprspace
