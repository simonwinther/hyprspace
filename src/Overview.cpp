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
                e.name        = WS->m_name.empty() ? std::to_string(WS->m_id) : WS->m_name;
                e.isActive    = WS == MONITOR->m_activeWorkspace || WS == MONITOR->m_activeSpecialWorkspace;
                m_entries.push_back(e);
                it = std::prev(m_entries.end());
            }

            SWindowSlot slot;
            slot.window = w;

            const auto POS = w->m_realPosition->value() - MONITOR->m_position;
            slot.rect      = SBoxF{POS.x, POS.y, SIZE.x, SIZE.y};

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

        // The workspace already on screen shrinks into its cell; the others grow
        // in place, which reads as the desktop folding into the grid. Starting
        // from the usable area rather than the whole output means its windows
        // begin exactly where they really are.
        const SBoxF FULL = m_usable;

        for (auto& e : m_entries) {
            if (e.isActive) {
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

        if (commitSelection)
            commit();

        *m_progress = 0.F;
        damage();
    }

    void COverview::commit() {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return;

        const auto& entry = m_entries[m_tiles[m_selected].key];

        // Clicking a specific window inside a tile goes straight to it.
        if (const auto W = m_clickedWindow.lock()) {
            focusSelection(W, config::warpCursor());
            return;
        }

        const auto WS = g_pCompositor->getWorkspaceByID(entry.workspaceId);
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

        // Number keys jump straight to that workspace's tile.
        if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
            const long WANT = sym == XKB_KEY_0 ? 10 : static_cast<long>(sym - XKB_KEY_0);

            for (size_t i = 0; i < m_tiles.size(); ++i) {
                if (m_entries[m_tiles[i].key].workspaceId == WANT) {
                    m_clickedWindow.reset();
                    selectIndex(static_cast<int>(i));
                    break;
                }
            }
            return true;
        }

        // Everything else is swallowed: the overview owns the keyboard, and there
        // is deliberately no text entry anywhere in hyprspace.
        return true;
    }

    int COverview::tileAtLocal(const Vector2D& local) const {
        return tileAt(m_tiles, local.x, local.y);
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

    void COverview::onMouseMove(const Vector2D& globalPos) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const auto LOCAL = globalPos - MONITOR->m_position;
        const int  IDX   = tileAtLocal(LOCAL);

        if (IDX == m_hovered)
            return;

        m_hovered = IDX;

        if (IDX >= 0 && config::followMouse())
            selectIndex(IDX);

        damage();
    }

    bool COverview::onMouseButton(uint32_t button, bool pressed) {
        constexpr uint32_t MOUSE_LEFT  = 0x110;
        constexpr uint32_t MOUSE_RIGHT = 0x111;

        if (!pressed)
            return true;

        if (button == MOUSE_LEFT) {
            if (m_hovered >= 0) {
                selectIndex(m_hovered);

                if (const auto MONITOR = m_monitor.lock())
                    m_clickedWindow = windowAtLocal(g_pInputManager->getMouseCoordsInternal() - MONITOR->m_position);

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

        m_capture.beginFrame();

        for (auto& e : m_entries) {
            for (auto& slot : e.windows) {
                const auto W = slot.window.lock();
                if (!W || !W->m_isMapped)
                    continue;

                // Re-assert: a workspace change elsewhere can re-suspend these.
                if (!e.isActive)
                    W->setSuspended(false);

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
            const bool   HOVERED  = static_cast<int>(i) == m_hovered;

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
                const bool ACCENT = SELECTED || HOVERED;
                const auto COLOUR = ACCENT ? (SELECTED ? config::overviewActiveBorder() : config::overviewHoverBorder()) : config::overviewTileBorderColor();
                const double W    = ACCENT ? BORDER : 1.0;

                if (W > 0)
                    rect(SBoxF{cell.x - W, cell.y - W, cell.w + W * 2.0, cell.h + W * 2.0}, COLOUR.modifyA(COLOUR.a * PROGRESS * FADE), round + W);
            }

            // A backing plate, so a workspace's empty area reads as a screen
            // rather than a hole punched in the dim.
            const auto TILEBG = config::overviewTileBgColor();
            rect(cell, TILEBG.modifyA(TILEBG.a * FADE), round);

            for (const auto& slot : entry.windows) {
                const auto W = slot.window.lock();
                if (!W)
                    continue;

                auto t = m_capture.textureFor(W);
                if (!t)
                    continue;

                const SBoxF b = windowBoxInCell(slot.rect, cell);
                if (b.w < 1 || b.h < 1)
                    continue;

                // Clip to the cell: a fullscreen window covers the reserved area
                // too, and would otherwise spill past the tile's edges.
                tex(t, b, ALPHA, round, cell);
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

        return out;
    }

} // namespace hyprspace
