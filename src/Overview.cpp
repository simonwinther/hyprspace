#include "Overview.hpp"

#include "Config.hpp"
#include "Focus.hpp"
#include "PassElements.hpp"
#include "Texture.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <format>

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

        collectWindows();
        computeLayout();
        hideRealWindows();

        // Start on whatever was focused, else the first tile.
        m_selected = 0;
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            if (m_entries[m_tiles[i].key].window == m_originalFocus) {
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
        // the compositor restore the correct state.
        g_pCompositor->updateSuspendedStates();
    }

    void COverview::collectWindows() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const bool ALL     = config::overviewAllWorkspaces();
        const bool SPECIAL = config::overviewIncludeSpecial();

        for (const auto& w : g_pCompositor->m_windows) {
            if (!w || !w->m_isMapped || w->m_fadingOut)
                continue;
            if (w->m_monitor.lock() != MONITOR)
                continue;

            const auto WS = w->m_workspace;
            if (!WS)
                continue;

            if (WS->m_isSpecialWorkspace) {
                if (!SPECIAL)
                    continue;
            } else if (!ALL && WS != MONITOR->m_activeWorkspace)
                continue;

            const auto SIZE = w->m_realSize->value();
            if (SIZE.x < 1.0 || SIZE.y < 1.0)
                continue;

            SEntry e;
            e.window               = w;
            e.workspace            = WS->m_id;
            e.title                = w->m_title;
            e.fromVisibleWorkspace = WS->isVisible();

            // Monitor-local logical coordinates.
            const auto POS = w->m_realPosition->value() - MONITOR->m_position;
            e.start        = SBoxF{POS.x, POS.y, SIZE.x, SIZE.y};

            m_entries.push_back(e);

            // Clients on hidden workspaces are suspended by the compositor and
            // would otherwise render as a frozen last frame.
            w->setSuspended(false);
        }
    }

    // Warp every collected window to zero alpha: Hyprland's normal pass then
    // skips them entirely (renderWindow bails on effectiveAlpha() == 0), so the
    // wallpaper and bar are what shows through the dim. Offscreen captures are
    // unaffected because standalone renders force alpha to 1.
    void COverview::hideRealWindows() {
        for (auto& e : m_entries) {
            const auto W = e.window.lock();
            if (!W)
                continue;

            auto& av       = W->alpha(Desktop::View::WINDOW_ALPHA_FADE);
            e.savedAlpha   = av->goal();
            e.alphaHidden  = true;
            av->setValueAndWarp(0.F);
        }
    }

    void COverview::restoreRealWindows() {
        for (auto& e : m_entries) {
            if (!e.alphaHidden)
                continue;

            const auto W = e.window.lock();
            if (!W)
                continue;

            W->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(e.savedAlpha);
            e.alphaHidden = false;
        }
    }

    void COverview::computeLayout() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        std::vector<STileInput> input;
        input.reserve(m_entries.size());

        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& e = m_entries[i];
            input.push_back(STileInput{
                .key         = i,
                .aspect      = e.start.h > 0 ? e.start.w / e.start.h : 1.0,
                .workspaceId = e.workspace,
            });
        }

        SLayoutParams params;
        params.screenW    = MONITOR->m_size.x;
        params.screenH    = MONITOR->m_size.y;
        params.padding    = config::overviewPadding();
        params.gap        = config::overviewGap();
        params.bandGap    = config::overviewBandGap();
        params.labelGutter = config::overviewLabelGutter();
        params.showLabels  = config::overviewShowLabels();

        auto result = layout(input, params);
        m_tiles     = std::move(result.tiles);
        m_bands     = std::move(result.bands);

        for (const auto& t : m_tiles)
            m_entries[t.key].target = t.box;

        // Windows that live on a hidden workspace have no meaningful on-screen
        // origin, so grow them out of their own tile instead of flying in from
        // an arbitrary position.
        for (auto& e : m_entries) {
            if (e.fromVisibleWorkspace)
                continue;

            const double shrink = 0.86;
            e.start             = SBoxF{
                            e.target.x + e.target.w * (1 - shrink) / 2,
                            e.target.y + e.target.h * (1 - shrink) / 2,
                            e.target.w * shrink,
                            e.target.h * shrink,
            };
        }
    }

    SBoxF COverview::interpolate(const SEntry& e) const {
        return lerpBox(e.start, e.target, m_progress->value());
    }

    void COverview::selectIndex(int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_tiles.size()))
            return;
        if (idx == m_selected)
            return;

        m_selected = idx;
        damage();
    }

    void COverview::close(bool commitSelection) {
        if (m_closing)
            return;

        m_closing = true;
        m_commit  = commitSelection;

        if (commitSelection)
            commit();

        *m_progress = 0.F;
        damage();
    }

    void COverview::commit() {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
            return;

        focusSelection(m_entries[m_tiles[m_selected].key].window.lock(), config::warpCursor());
    }

    // ---------------------------------------------------------------- input --

    bool COverview::onKey(xkb_keysym_t sym, uint32_t mods, bool pressed) {
        if (!pressed)
            return true; // swallow releases too while we hold the grab

        const bool SHIFT = mods & HL_MODIFIER_SHIFT;

        auto move = [&](EDirection dir) { selectIndex(navigate(m_tiles, m_selected, dir)); };

        auto cycle = [&](int delta) {
            if (m_tiles.empty())
                return;
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

        // Everything else is swallowed: the overview owns the keyboard, and there
        // is deliberately no text entry anywhere in hyprspace.
        return true;
    }

    void COverview::onMouseMove(const Vector2D& globalPos) {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const auto LOCAL = globalPos - MONITOR->m_position;
        const int  IDX   = tileAt(m_tiles, LOCAL.x, LOCAL.y);

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
            const auto W = e.window.lock();
            if (!W || !W->m_isMapped)
                continue;

            // Re-assert: a workspace change elsewhere can re-suspend these.
            if (!e.fromVisibleWorkspace)
                W->setSuspended(false);

            m_capture.capture(W, MONITOR);
        }

        m_capture.endFrame();

        // Keep the animation running smoothly.
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

        // Everything above is laid out in logical coordinates; rect and texture
        // pass elements are drawn in physical pixels, so scale on the way out.
        // (boundingBox()/opaqueRegion() stay logical — the pass scales those.)
        auto px = [&](const SBoxF& b) { return CBox{b.x * SCALE, b.y * SCALE, b.w * SCALE, b.h * SCALE}; };

        auto rect = [&](const SBoxF& b, const CHyprColor& col, double round = 0) {
            out.emplace_back(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = px(b), .color = col, .round = static_cast<int>(round * SCALE)}));
        };
        auto tex = [&](SP<Render::ITexture> t, const SBoxF& b, float a, double round = 0) {
            out.emplace_back(
                makeUnique<CTexPassElement>(CTexPassElement::SRenderData{.tex = t, .box = px(b), .a = a, .round = static_cast<int>(round * SCALE)}));
        };

        // Text textures are rasterised at native resolution, so convert their
        // pixel size back to logical for placement.
        auto logicalSize = [&](const SP<Render::ITexture>& t) { return Vector2D{t->m_size.x / SCALE, t->m_size.y / SCALE}; };

        const SBoxF MONBOX{0, 0, MONITOR->m_size.x, MONITOR->m_size.y};

        // --- backdrop -----------------------------------------------------------
        // The real windows are alpha-0 while the overview is up, so what sits
        // underneath is the wallpaper and the bar. Dim those rather than painting
        // over them, which is what makes this read like the GNOME overview.
        rect(MONBOX, config::overviewBgColor().modifyA(config::overviewBgDim() * PROGRESS));

        // --- workspace labels --------------------------------------------------
        if (config::overviewShowLabels() && PROGRESS > 0.3F) {
            const float LABEL_A = (PROGRESS - 0.3F) / 0.7F;

            for (const auto& band : m_bands) {
                if (band.tileCount == 0)
                    continue;

                const auto  WS   = g_pCompositor->getWorkspaceByID(band.workspaceId);
                std::string NAME = WS ? WS->m_name : "";
                if (NAME.empty())
                    NAME = std::format("Workspace {}", band.workspaceId);

                auto t = textures().text(NAME, FONT, config::overviewLabelColor(), static_cast<int>(band.labelBox.w), SCALE);
                if (!t)
                    continue;

                const auto SZ = logicalSize(t);
                tex(t, SBoxF{band.labelBox.x, band.labelBox.y + (band.labelBox.h - SZ.y) / 2.0, SZ.x, SZ.y}, LABEL_A);
            }
        }

        // --- tiles -------------------------------------------------------------
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            const auto& tile = m_tiles[i];
            auto&       e    = m_entries[tile.key];

            const auto W = e.window.lock();
            if (!W)
                continue;

            auto t = m_capture.textureFor(W);
            if (!t)
                continue;

            const SBoxF box = interpolate(e);
            const float A   = e.fromVisibleWorkspace ? 1.F : PROGRESS;

            const bool  SELECTED = static_cast<int>(i) == m_selected;
            const bool  HOVERED  = static_cast<int>(i) == m_hovered;

            // Rounding ramps in so that a tile sitting exactly on top of its real
            // window at progress 0 is pixel-identical to the real thing.
            const double round = ROUNDING * PROGRESS;

            if (BORDER > 0 && (SELECTED || HOVERED) && PROGRESS > 0.2F) {
                auto colour = SELECTED ? config::overviewActiveBorder() : config::overviewHoverBorder();
                rect(SBoxF{box.x - BORDER, box.y - BORDER, box.w + BORDER * 2.0, box.h + BORDER * 2.0}, colour.modifyA(colour.a * PROGRESS), round + BORDER);
            }

            tex(t, box, A, round);

            // --- title of the selected tile -----------------------------------
            // Drawn inside the bottom of the tile rather than below it: bands sit
            // close together, and a title hanging off a tile would overlap the
            // workspace underneath.
            if (SELECTED && config::overviewShowTitles() && PROGRESS > 0.5F && !e.title.empty() && box.h > 48) {
                const float TITLE_A = (PROGRESS - 0.5F) / 0.5F;

                auto        t2 = textures().text(e.title, FONT, config::overviewTitleColor(), static_cast<int>(std::max(48.0, box.w - 24)), SCALE);
                if (!t2)
                    continue;

                const auto       SZ = logicalSize(t2);
                constexpr double PAD_X = 10, PAD_Y = 4;

                const SBoxF      bgBox{
                         box.x + (box.w - SZ.x) / 2.0 - PAD_X,
                         box.y + box.h - SZ.y - PAD_Y * 2 - 8,
                         SZ.x + PAD_X * 2,
                         SZ.y + PAD_Y * 2,
                };

                const auto BGCOL = config::overviewTitleBgColor();
                rect(bgBox, BGCOL.modifyA(BGCOL.a * TITLE_A), 6);
                tex(t2, SBoxF{bgBox.x + PAD_X, bgBox.y + PAD_Y, SZ.x, SZ.y}, TITLE_A);
            }
        }

        return out;
    }

} // namespace hyprspace
