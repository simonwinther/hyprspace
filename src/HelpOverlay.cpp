#include "HelpOverlay.hpp"

#include "CompositorHooks.hpp"
#include "Config.hpp"
#include "Texture.hpp"

#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <format>
#include <cctype>

namespace hyprspace {
    namespace {
        std::string keyLabel(xkb_keysym_t key) {
            switch (key) {
            case XKB_KEY_NoSymbol:
                return {};
            case XKB_KEY_question:
                return "?";
            case XKB_KEY_slash:
                return "/";
            case XKB_KEY_space:
                return "Space";
            case XKB_KEY_plus:
                return "+";
            case XKB_KEY_minus:
                return "−";
            default:
                break;
            }
            char name[64]{};
            if (xkb_keysym_get_name(key, name, sizeof(name)) <= 0)
                return {};
            std::string label{name};
            if (label.size() == 1)
                label[0] = std::toupper(static_cast<unsigned char>(label[0]));
            return label;
        }

        std::string modifiers(uint32_t mask) {
            std::string result;
            for (const auto& [bit, name] : {std::pair{HL_MODIFIER_META, "Super"},
                                            {HL_MODIFIER_CTRL, "Ctrl"},
                                            {HL_MODIFIER_ALT, "Alt"},
                                            {HL_MODIFIER_SHIFT, "Shift"},
                                            {HL_MODIFIER_CAPS, "Caps"},
                                            {HL_MODIFIER_MOD2, "Mod2"},
                                            {HL_MODIFIER_MOD3, "Mod3"},
                                            {HL_MODIFIER_MOD5, "Mod5"}})
                if (mask & bit)
                    result += (result.empty() ? "" : " + ") + std::string(name);
            return result;
        }

        std::string nativeShortcut(const std::string& handler) {
            std::string result;
            for (const auto& binding : g_pKeybindManager->m_keybinds) {
                if (!binding || binding->handler != handler || binding->key.empty() || binding->release || binding->mouse)
                    continue;
                const auto mods  = modifiers(binding->modmask);
                const auto label = (mods.empty() ? "" : mods + " + ") + binding->key;
                if (result.find(label) == std::string::npos)
                    result += (result.empty() ? "" : " / ") + label;
            }
            return result.empty() ? "Hyprland binding" : result;
        }

        CHyprColor fade(const CHyprColor& color, float alpha) {
            return color.modifyA(color.a * alpha);
        }
    } // namespace

    CHelpOverlay::~CHelpOverlay() {
        stopCaret();
    }

    CHelpOverlay::SSettings CHelpOverlay::settings() const {
        return {config::overviewZoomKey(),   config::overviewEmptyWorkspaceKey(), config::overviewWindowViewKey(),
                config::overviewFollowKey(), config::overviewHelpKey(),           config::overviewCloseWorkspaceKey(), config::overviewWorkspaceDragModifiers(),
                config::overviewWheelZoom(), m_session.cursorFollowEnabled(),     config::followMouse(),
                config::warpCursor(),        config::overviewShowFollowHint(),    config::overviewAllMonitors()};
    }

    std::vector<SHelpEntry> CHelpOverlay::catalog(const SSettings& s) const {
        std::vector<SHelpEntry> entries;
        const auto              add = [&](const char* id, size_t topic, std::string title, std::string keys, std::string description, std::string tags = "",
                                          std::string setting = "") {
            entries.push_back({id, std::move(title), std::move(keys), std::move(description), std::move(tags), std::move(setting), topic});
        };
        const auto zoom = keyLabel(s.zoom), windows = keyLabel(s.windows), empty = keyLabel(s.empty), follow = keyLabel(s.follow), help = keyLabel(s.help);
        const auto held = [&](std::string keys) { return !s.wheelZoom || zoom.empty() ? "Disabled" : zoom + " + " + keys; };
        add("help", 1, "Find a control", help.empty() ? "Disabled" : help,
            "Open this guide from workspaces or the window board. Use / to search every topic, including your configured shortcuts.", "help documentation guide manual",
            "overview:help_key");
        add("overview", 1, "Open the overview", nativeShortcut("hyprspace:overview"),
            "Use your configured Hyprland binding to open workspaces. Your application keeps its place while you browse.", "overview open start launch");
        add("navigate", 1, "Choose a workspace", "hjkl / arrows", "Move to the nearest workspace in a direction. Tab and Shift+Tab step through the tiles.",
            "selection navigation vim keyboard");
        add("open", 1, "Open the selection", "Enter / Space / S", "Commit the selected workspace. Clicking a window preview also focuses that window.",
            "confirm accept focus enter click");
        add("cancel", 1, "Return to your desktop", "Esc / right-click",
            "Close the overview and return to your original workspace. Escape first cancels an active drag or exits the window board.", "cancel dismiss exit back");
        add("digits", 1, "Jump to a workspace", "1–9 / 0", "Open a numbered workspace immediately. 0 selects workspace 10.", "number digit direct jump");
        add("ends", 1, "First or last workspace", "Home / End", "Select the first or last workspace without committing it. Press Enter to open it.",
            "navigation first last");
        add("scrolling", 1, "Browse a scrolling workspace", "Wheel / PgUp / PgDn",
            "Scroll the workspace under the pointer. Its edge arrows reveal hidden columns while keeping the overview open.", "scroll scrolling columns touchpad tape");
        add("zoom", 2, "Inspect a workspace or window", zoom.empty() ? "Disabled" : "Hold " + zoom,
            "Enlarge the selected workspace, or the selected window in the window board. The cursor follows its point into zoom and back to the grid; repeated presses without mouse movement keep its position. You can move the mouse throughout.", "zoom inspect magnify fit",
            "overview:zoom_key");
        add("magnify", 2, "Zoom in or out", held("wheel / + / −"),
            "Magnify from 1× to 4× the fitted view. Wheel zoom anchors at the pointer; + and − use the viewport center.", "zoom magnify plus minus scroll touchpad",
            "overview:wheel_zoom");
        add("pan", 2, "Grab the zoomed picture", held("Space / right-drag"),
            "Hold Space and move the pointer, or hold the right mouse button and drag. Works with a mouse or a one-finger touchpad motion.", "pan grab drag touchpad",
            "overview:wheel_zoom");
        add("keyboard-pan", 2, "Pan with the keyboard", held("Shift + arrows"),
            "Move the magnified picture by 40 logical pixels per press. Holding an arrow repeats the movement.", "pan shift keyboard move", "overview:wheel_zoom");
        add("follow", 2, "Follow the cursor", follow.empty() ? "Disabled" : held(follow),
            std::format("Follow is {}. Move the pointer to steer a magnified picture, including at the corners and over Waybar. With Follow enabled and zoom held above "
                        "the fitted view, the cursor stays inside this monitor, including during panning. Return to the fitted view, release the zoom key or disable "
                        "Follow to cross monitors. Your choice survives restarts.",
                        s.followEnabled ? "enabled" : "disabled"),
            "follow cursor mouse persistent toggle enabled disabled waybar corners boundary confine monitor", "overview:follow_key");
        add("zoom-neighbor", 2, "Browse while inspecting", "Pointer at an edge",
            "At the fitted zoom level, pause over a directional edge arrow to inspect its neighboring workspace. Extra magnification pauses this navigation.",
            "zoom edge arrows neighbors dwell");
        add("window-board", 3, "Switch to the window board", windows.empty() ? "Disabled" : windows,
            "Gather windows from every covered output onto one board. Press the same key to return to workspaces.", "windows board global apps monitors",
            "overview:window_view_key");
        add("groups", 3, "Group the window board", windows.empty() ? "Toolbar" : "Shift + " + windows,
            "Cycle All windows, Apps, Workspaces and Monitors. The toolbar selects a group directly.", "group grouping apps applications outputs monitors");
        add("window-search", 3, "Find a window", "/",
            "Search window titles, applications, workspace names and monitor names. Type normally while the search field has focus.",
            "search find filter title app class monitor");
        add("recent", 3, "Show recent windows first", "R",
            "Switch between location order and recent-first order within each group. The selected window keeps its identity.", "recent recency order sort history");
        add("window-navigation", 3, "Choose a window", "hjkl / arrows / Tab",
            "Move through the board. Keyboard navigation scrolls the selected window into view. Enter opens its original window.", "navigate vim selection focus");
        add("search-editing", 3, "Edit a search", "Ctrl+A / Ctrl+U",
            "Ctrl+A selects the query; Ctrl+U clears it. Backspace, Delete, arrows and Home/End edit text. Shift extends a selection.",
            "search query text unicode edit erase delete");
        add("empty", 4, "Prepare an empty workspace", empty.empty() ? "Middle-click / + Empty" : empty + " / middle-click",
            "Select an empty workspace on the pointer's output and keep the overview open. Enter opens it; a subsequent launch uses that workspace.",
            "empty new workspace prepare create launch", "overview:empty_workspace_key");
        add("move-window", 4, "Move a window", "Super + left-drag",
            "Drag a preview to rearrange its window or move it into a workspace on another output. Escape cancels the drag.",
            "move rearrange window drag monitor output");
        add("close-workspace", 4, "Close an empty workspace", s.closeWorkspace == XKB_KEY_NoSymbol ? "Disabled" : keyLabel(s.closeWorkspace),
            "Remove the selected empty normal workspace and keep the overview open. An active workspace switches to the nearest remaining tile on its monitor. Workspaces with windows and the last normal workspace on a monitor are protected.",
            "close remove dismiss delete empty workspace quit q", "overview:close_workspace_key");
        add("resize-window", 4, "Resize a window", "Super + right-drag", "Resize a window from its preview, scaled into the workspace tile. Escape cancels the resize.",
            "resize size window drag");
        add("move-workspace", 4, "Move a whole workspace", s.workspaceMods ? modifiers(s.workspaceMods) + " + drag" : "Disabled",
            "Left-drag a regular workspace to another output. Its number, name and layout travel with it; pinned windows stay on their original monitor.",
            "move workspace monitor output drag pinned", "overview:workspace_drag_modifiers");
        add("bindings", 5, "Configure keybindings", "Hyprland config",
            "Change zoom_key, empty_workspace_key, close_workspace_key, window_view_key, follow_key or help_key under plugin:hyprspace:overview. An empty key disables its shortcut.",
            "keys keybind keybinds keybinding keybindings bindings shortcuts controls configuration customize remap", "overview:*_key");
        add("hint", 5, "Show or hide the follow hint", s.showFollowHint ? "Shown" : "Hidden",
            "Set show_follow_hint to false to hide the zoom hint. Your saved follow preference stays available through its toggle key.",
            "hint follow badge indicator clutter ui hide show configuration", "overview:show_follow_hint");
        add("mouse-selection", 5, "Let hover choose the selection", s.followMouse ? "Enabled" : "Disabled",
            "follow_mouse controls whether real pointer movement selects a preview. Cursor-follow magnification is a separate preference.",
            "mouse hover selection pointer configuration", "follow_mouse");
        add("monitors", 5, "Open on every monitor", s.allMonitors ? "All monitors" : "Pointer's monitor",
            "all_monitors controls whether the overview covers every output or just the one under the pointer when it opens.",
            "monitors outputs display multi configuration", "overview:all_monitors");
        add("warp", 5, "Move the pointer after opening a window", s.warpCursor ? "Enabled" : "Disabled",
            "warp_cursor controls whether committing a selection moves the pointer into that window.", "pointer mouse warp focus configuration", "warp_cursor");
        add("help-search", 1, "Search this guide", "/ → type → Enter",
            "Search across all topics. Enter returns to browsing results; j/k move between entries and h/l choose topics. Escape leaves search, then closes help.",
            "help search find filter topics vim");
        return entries;
    }

    void CHelpOverlay::open(PHLMONITOR mon) {
        if (!mon)
            return;
        m_monitor       = mon;
        m_active        = true;
        m_searchFocused = false;
        m_keyboardOwned = hooks::keyboardOwned();
        ++m_generation;
        m_search.clear();
        m_browser.setTopic(0);
        m_scroll = 0;
        refresh(true);
        m_session.damage();
    }

    void CHelpOverlay::close() {
        m_active = m_searchFocused = false;
        ++m_generation;
        stopCaret();
        m_search.clear();
        m_browser.setQuery("");
        m_monitor.reset();
        m_session.damage();
    }

    void CHelpOverlay::refresh(bool force) {
        if (!m_active)
            return;
        syncKeyboardOwnership(hooks::keyboardOwned());
        const auto mon = monitor();
        if (!mon) {
            close();
            return;
        }
        const auto current = settings();
        if (force || !m_settings || *m_settings != current) {
            m_settings = current;
            m_browser.setEntries(catalog(current));
        }
        const auto font = config::overviewFont();
        if (font != m_font || m_scale != mon->m_scale) {
            m_font           = font;
            m_scale          = mon->m_scale;
            m_measuredCursor = std::numeric_limits<size_t>::max();
            int width = 0, height = 0;
            measureText("Mg", m_font, width, height, m_scale);
            m_lineHeight = height / m_scale;
        }
        const auto& reserved = mon->m_reservedArea;
        const SBoxF usable{reserved.left(), reserved.top(), std::max(1.0, mon->m_size.x - reserved.left() - reserved.right()),
                           std::max(1.0, mon->m_size.y - reserved.top() - reserved.bottom())};
        m_layout = helpLayout(usable, m_lineHeight);
        ensureVisible();
    }

    void CHelpOverlay::syncKeyboardOwnership(bool owned) {
        if (!m_active || owned == m_keyboardOwned)
            return;
        m_keyboardOwned = owned;
        ++m_generation;
        if (owned && m_searchFocused)
            resetCaret();
    }

    SBoxF CHelpOverlay::topicBox(size_t topic) const {
        return {m_layout.topics.x, m_layout.topics.y + topic * 40, m_layout.topics.w, 34};
    }
    SBoxF CHelpOverlay::rowBox(size_t row) const {
        return {m_layout.results.x, m_layout.results.y + row * m_layout.rowHeight - m_scroll, m_layout.results.w, m_layout.rowHeight};
    }
    SBoxF CHelpOverlay::searchContent() const {
        return {m_layout.search.x + 14, m_layout.search.y + 6, std::max(1.0, m_layout.search.w - 28), m_layout.search.h - 12};
    }

    bool CHelpOverlay::overSearch(const Vector2D& point) const {
        const auto mon = monitor();
        return m_active && mon && m_layout.search.contains(point.x - mon->m_position.x, point.y - mon->m_position.y);
    }

    bool CHelpOverlay::reservesKey(xkb_keysym_t sym, uint32_t mods) const {
        if (!m_active || sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req || (sym >= 0x10080000 && sym <= 0x1008FFFF) ||
            (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) || sym == XKB_KEY_Mode_switch ||
            sym == XKB_KEY_Num_Lock || sym == XKB_KEY_Scroll_Lock)
            return false;
        return (mods & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)) == 0 ||
               (m_searchFocused && mods == HL_MODIFIER_CTRL && (sym == XKB_KEY_a || sym == XKB_KEY_u));
    }

    bool CHelpOverlay::oneShot(xkb_keysym_t sym, const std::string& text) const {
        if (!m_searchFocused && sym != XKB_KEY_h && sym != XKB_KEY_j && sym != XKB_KEY_k && sym != XKB_KEY_l && sym != XKB_KEY_Left && sym != XKB_KEY_Right &&
            sym != XKB_KEY_Up && sym != XKB_KEY_Down && sym != XKB_KEY_Tab && sym != XKB_KEY_ISO_Left_Tab && sym != XKB_KEY_Home && sym != XKB_KEY_End &&
            sym != XKB_KEY_Page_Up && sym != XKB_KEY_Page_Down)
            return true;
        return sym == XKB_KEY_Escape || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter ||
               (text.empty() && config::overviewHelpKey() != XKB_KEY_NoSymbol && xkb_keysym_to_lower(sym) == config::overviewHelpKey()) ||
               (!m_searchFocused && (text == "/" || sym == XKB_KEY_slash || xkb_keysym_to_lower(sym) == config::overviewHelpKey()));
    }

    void CHelpOverlay::focusSearch(bool selectAll) {
        if (!m_searchFocused) {
            m_searchFocused = true;
            ++m_generation;
        }
        if (selectAll)
            m_search.selectAll();
        resetCaret();
    }
    void CHelpOverlay::blurSearch() {
        if (m_searchFocused) {
            m_searchFocused = false;
            ++m_generation;
        }
        stopCaret();
        m_session.damage();
    }
    void CHelpOverlay::setTopic(size_t topic) {
        blurSearch();
        m_search.clear();
        m_browser.setTopic(topic);
        m_scroll = 0;
        m_session.damage();
    }
    void CHelpOverlay::queryChanged() {
        m_browser.setQuery(m_search.text());
        m_scroll = 0;
        ensureVisible();
        resetCaret();
    }
    void CHelpOverlay::ensureVisible() {
        if (!m_browser.count()) {
            m_scroll = 0;
            return;
        }
        const double top = m_browser.selected() * m_layout.rowHeight, bottom = top + m_layout.rowHeight;
        if (top < m_scroll)
            m_scroll = top;
        else if (bottom > m_scroll + m_layout.results.h)
            m_scroll = bottom - m_layout.results.h;
        m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, m_browser.count() * m_layout.rowHeight - m_layout.results.h));
    }

    void CHelpOverlay::onKey(xkb_keysym_t sym, uint32_t mods, const std::string& text) {
        if (!m_active)
            return;
        if (sym == XKB_KEY_Escape) {
            if (m_searchFocused)
                blurSearch();
            else
                m_session.closeHelp();
            return;
        }
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            blurSearch();
            return;
        }
        if (text.empty() && config::overviewHelpKey() != XKB_KEY_NoSymbol && xkb_keysym_to_lower(sym) == config::overviewHelpKey()) {
            m_session.closeHelp();
            return;
        }
        if (m_searchFocused) {
            const auto before = m_search.text();
            if (sym == XKB_KEY_BackSpace)
                m_search.backspace();
            else if (sym == XKB_KEY_Delete)
                m_search.eraseForward();
            else if (mods == HL_MODIFIER_CTRL && sym == XKB_KEY_a)
                m_search.selectAll();
            else if (mods == HL_MODIFIER_CTRL && sym == XKB_KEY_u)
                m_search.clear();
            else if (sym == XKB_KEY_Left)
                m_search.left(mods == HL_MODIFIER_SHIFT);
            else if (sym == XKB_KEY_Right)
                m_search.right(mods == HL_MODIFIER_SHIFT);
            else if (sym == XKB_KEY_Home)
                m_search.home(mods == HL_MODIFIER_SHIFT);
            else if (sym == XKB_KEY_End)
                m_search.end(mods == HL_MODIFIER_SHIFT);
            else if (sym == XKB_KEY_Up || sym == XKB_KEY_Down || sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
                blurSearch();
                m_browser.move(sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab || mods == HL_MODIFIER_SHIFT ? -1 : 1);
                ensureVisible();
            } else if (!text.empty() && (mods & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)) == 0)
                m_search.insert(text);
            if (before != m_search.text())
                queryChanged();
            else if (m_searchFocused)
                resetCaret();
        } else if ((text == "/" || sym == XKB_KEY_slash) && !(mods & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)))
            focusSearch();
        else if (xkb_keysym_to_lower(sym) == config::overviewHelpKey() && config::overviewHelpKey() != XKB_KEY_NoSymbol)
            m_session.closeHelp();
        else if (sym == XKB_KEY_h || sym == XKB_KEY_Left)
            setTopic(m_browser.topic() ? m_browser.topic() - 1 : HELP_TOPICS.size() - 1);
        else if (sym == XKB_KEY_l || sym == XKB_KEY_Right)
            setTopic((m_browser.topic() + 1) % HELP_TOPICS.size());
        else {
            if (sym == XKB_KEY_j || sym == XKB_KEY_Down || (sym == XKB_KEY_Tab && mods != HL_MODIFIER_SHIFT))
                m_browser.move(1);
            else if (sym == XKB_KEY_k || sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab || (sym == XKB_KEY_Tab && mods == HL_MODIFIER_SHIFT))
                m_browser.move(-1);
            else if (sym == XKB_KEY_Page_Down)
                m_browser.move(std::max(1, static_cast<int>(m_layout.results.h / m_layout.rowHeight)));
            else if (sym == XKB_KEY_Page_Up)
                m_browser.move(-std::max(1, static_cast<int>(m_layout.results.h / m_layout.rowHeight)));
            else if (sym == XKB_KEY_Home || sym == XKB_KEY_End)
                m_browser.edge(sym == XKB_KEY_End);
            ensureVisible();
        }
        m_session.damage();
    }

    void CHelpOverlay::pointer(const Vector2D& point) {
        if (m_pointer == point)
            return;
        m_pointer = point;
        m_session.damage();
    }
    void CHelpOverlay::button(uint32_t button, bool pressed, uint32_t mods, const Vector2D& point) {
        if (!m_active || !pressed || button != 0x110 || !monitor())
            return;
        const auto local = point - monitor()->m_position;
        if (m_layout.close.contains(local.x, local.y) || !m_layout.panel.contains(local.x, local.y)) {
            m_session.closeHelp();
            return;
        }
        if (overSearch(point)) {
            focusSearch(false);
            m_search.setCursor(textIndexAtX(m_search.text(), m_font, local.x - searchContent().x + m_searchOffset, m_scale), mods == HL_MODIFIER_SHIFT);
            resetCaret();
            return;
        }
        for (size_t i = 0; i < HELP_TOPICS.size(); ++i)
            if (m_layout.topics.w > 0 && topicBox(i).contains(local.x, local.y)) {
                setTopic(i);
                return;
            }
        if (m_layout.results.contains(local.x, local.y)) {
            const auto index = static_cast<size_t>((local.y - m_layout.results.y + m_scroll) / m_layout.rowHeight);
            blurSearch();
            m_browser.select(index);
            ensureVisible();
            m_session.damage();
        }
    }
    void CHelpOverlay::scroll(const SScrollInput& input) {
        if (!m_active || input.horizontal || input.delta == 0)
            return;
        blurSearch();
        m_browser.move((input.delta > 0 ? 1 : -1) * std::max(1, static_cast<int>(std::abs(input.delta) / 15)));
        ensureVisible();
        m_session.damage();
    }

    const STextCursor& CHelpOverlay::searchMetrics() const {
        if (m_measuredText != m_search.text() || m_measuredCursor != m_search.cursor()) {
            m_measuredText   = m_search.text();
            m_measuredCursor = m_search.cursor();
            m_searchMetrics  = measureTextCursor(m_measuredText, m_font, m_measuredCursor, m_scale);
        }
        const double width = searchContent().w, caret = m_searchMetrics.x / m_scale;
        if (caret < m_searchOffset)
            m_searchOffset = caret;
        else if (caret > m_searchOffset + width - 2)
            m_searchOffset = caret - width + 2;
        m_searchOffset = std::clamp(m_searchOffset, 0.0, std::max(0.0, m_searchMetrics.width / m_scale - width + 2));
        return m_searchMetrics;
    }
    void CHelpOverlay::resetCaret() {
        m_caretVisible = true;
        if (!m_caretTimer) {
            m_caretTimer = makeShared<CEventLoopTimer>(
                std::nullopt,
                [this](SP<CEventLoopTimer> self, void*) {
                    if (!m_active || !m_searchFocused)
                        return;
                    m_caretVisible = !m_caretVisible;
                    if (hooks::keyboardOwned())
                        m_session.damage();
                    self->updateTimeout(std::chrono::milliseconds(530));
                },
                nullptr);
            g_pEventLoopManager->addTimer(m_caretTimer);
        }
        m_caretTimer->updateTimeout(std::chrono::milliseconds(530));
        m_session.damage();
    }
    void CHelpOverlay::stopCaret() {
        if (m_caretTimer) {
            m_caretTimer->cancel();
            g_pEventLoopManager->removeTimer(m_caretTimer);
            m_caretTimer.reset();
        }
        m_caretVisible = false;
    }

    std::vector<UP<IPassElement>> CHelpOverlay::buildPass(PHLMONITOR mon) const {
        std::vector<UP<IPassElement>> out;
        if (!m_active || !mon || !m_session.covers(mon))
            return out;
        const double scale = mon->m_scale;
        const auto   ink = config::overviewLabelColor(), accent = config::overviewActiveBorder();
        const auto   plate = config::overviewTitleBgColor().modifyA(1.F);
        const auto   px    = [&](const SBoxF& box) { return CBox{box.x * scale, box.y * scale, box.w * scale, box.h * scale}; };
        const auto   rect  = [&](SBoxF box, CHyprColor color, double round = 0) {
            if (box.w > 0 && box.h > 0)
                out.emplace_back(
                    makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = px(box).round(), .color = color, .round = static_cast<int>(round * scale)}));
        };
        const auto border = [&](SBoxF box, CHyprColor color, double round = 0) {
            out.emplace_back(makeUnique<CBorderPassElement>(CBorderPassElement::SBorderData{
                .box = px(box).round(), .grad1 = Config::CGradientValueData(color), .round = static_cast<int>(round * scale), .borderSize = 1}));
        };
        const auto drawTexture = [&](SP<Render::ITexture> texture, SBoxF box, std::optional<SBoxF> clip = std::nullopt, bool centerY = true) {
            if (!texture)
                return;
            const auto                   size = texture->m_size / scale;
            CTexPassElement::SRenderData data{.tex = texture, .box = px({box.x, centerY ? box.cy() - size.y / 2 : box.y, size.x, size.y}), .a = 1};
            if (clip)
                data.clipBox = px(*clip);
            out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
        };
        const auto text = [&](const std::string& value, SBoxF box, CHyprColor color, bool bold = false, std::optional<SBoxF> clip = std::nullopt) {
            if (box.w <= 1 || box.h <= 1)
                return;
            drawTexture(textures().text(value, m_font + (bold ? " Bold" : ""), color, static_cast<int>(box.w), scale), box, clip);
        };
        rect({0, 0, mon->m_size.x, mon->m_size.y}, CHyprColor{0, 0, 0, .55F});
        if (mon != monitor())
            return out;
        const auto& panel = m_layout.panel;
        rect(panel, plate, 16);
        border(panel, fade(ink, .16F), 16);
        text("Hyprspace help", {m_layout.search.x, panel.y + 20, m_layout.search.w - 80, 30}, ink, true);
        rect(m_layout.close, fade(ink, .055F), 6);
        text("Esc", {m_layout.close.x + 15, m_layout.close.y, m_layout.close.w - 20, m_layout.close.h}, fade(ink, .7F));
        rect(m_layout.search, fade(ink, .045F), 9);
        border(m_layout.search, m_searchFocused ? accent : fade(ink, .13F), 9);
        const auto content = searchContent();
        searchMetrics();
        const auto [from, to] = m_searchFocused && hooks::keyboardOwned() ? m_search.selection() : std::pair<size_t, size_t>{0, 0};
        const auto input = m_search.text().empty()
                               ? textures().text(m_searchFocused ? "Type to search all help…" : "/  Search controls and settings", m_font, fade(ink, .5F),
                                                 static_cast<int>(content.w), scale)
                               : textures().inputText(m_search.text(), m_font, ink, static_cast<int>(content.w), m_searchOffset, scale, from, to, fade(accent, .25F));
        drawTexture(input, content, content);
        if (m_searchFocused && m_caretVisible && hooks::keyboardOwned()) {
            const auto&  metrics = searchMetrics();
            const double x       = content.x + metrics.x / scale - m_searchOffset;
            if (x >= content.x && x < content.x + content.w)
                rect({x, content.cy() - metrics.caretHeight / (2 * scale), 1, metrics.caretHeight / scale}, ink);
        }
        for (size_t i = 0; i < HELP_TOPICS.size() && m_layout.topics.w > 0; ++i) {
            const auto box = topicBox(i);
            if (i == m_browser.topic() && m_search.text().empty())
                rect(box, fade(accent, .12F), 7);
            text(HELP_TOPICS[i], {box.x + 12, box.y, box.w - 20, box.h}, i == m_browser.topic() && m_search.text().empty() ? accent : fade(ink, .72F));
        }
        text(std::format("{} · {} {}", m_search.text().empty() ? HELP_TOPICS[m_browser.topic()] : "Search results", m_browser.count(),
                         m_browser.count() == 1 ? "entry" : "entries"),
             m_layout.heading, fade(ink, .6F));
        const auto local = m_pointer - mon->m_position;
        for (size_t i = 0; i < m_browser.count(); ++i) {
            const auto box = rowBox(i);
            if (box.y + box.h <= m_layout.results.y || box.y >= m_layout.results.y + m_layout.results.h)
                continue;
            const auto&  entry    = m_browser.entry(i);
            const double clippedY = std::max(box.y, m_layout.results.y);
            const SBoxF  clipped{box.x, clippedY, box.w, std::max(0.0, std::min(box.y + box.h, m_layout.results.y + m_layout.results.h) - clippedY)};
            if (i == m_browser.selected() || box.contains(local.x, local.y))
                rect(clipped, fade(i == m_browser.selected() ? accent : ink, i == m_browser.selected() ? .12F : .035F), 7);
            const double keysW = std::min(190.0, box.w * .37);
            text(entry.keys, {box.x + 12, box.y, keysW - 20, box.h}, i == m_browser.selected() ? accent : fade(ink, .6F), false, m_layout.results);
            text(entry.title, {box.x + keysW, box.y, box.w - keysW - 12, box.h}, ink, i == m_browser.selected(), m_layout.results);
        }
        const double listHeight = m_browser.count() * m_layout.rowHeight;
        if (listHeight > m_layout.results.h) {
            const auto&  box   = m_layout.results;
            const double thumb = std::min(box.h, std::max(20.0, box.h * box.h / listHeight));
            rect({box.x + box.w - 3, box.y, 3, box.h}, fade(ink, .035F), 1.5);
            rect({box.x + box.w - 3, box.y + m_scroll / (listHeight - box.h) * (box.h - thumb), 3, thumb}, fade(ink, .25F), 1.5);
        }
        if (!m_browser.count()) {
            text("No matching help", m_layout.results, ink, true);
            text("Try “keybinds”, “follow” or “resize”.", {m_layout.results.x, m_layout.results.cy() + 24, m_layout.results.w, 26}, fade(ink, .6F));
        }
        if (const auto entry = m_browser.selection()) {
            const auto box = m_layout.detail;
            rect(box, fade(ink, .035F), 9);
            const bool compact = box.h < 150;
            text(entry->title, {box.x + 16, box.y + (compact ? 8 : 10), box.w - 32, compact ? 26.0 : 30.0}, ink, true);
            const double settingH = entry->setting.empty() ? 0 : 22;
            const double bodyTop  = compact ? 36 : 45;
            const SBoxF  body{box.x + 16, box.y + bodyTop, box.w - 32, std::max(1.0, box.h - bodyTop - 8 - settingH)};
            drawTexture(textures().paragraph(entry->description, m_font, fade(ink, .8F), static_cast<int>(body.w), static_cast<int>(body.h), scale), body, box, false);
            if (!entry->setting.empty())
                text(entry->setting, {box.x + 16, box.y + box.h - 28, box.w - 32, 22}, fade(accent, .85F));
        }
        const bool compact = panel.w < 720;
        text(m_searchFocused ? (compact ? "Enter  Results   Esc  Back   Ctrl+U  Clear" : "Enter  Browse results     Esc  Leave search     Ctrl+U  Clear")
                             : (compact ? "j/k ↑↓ · h/l ←→ · / Search · Esc Back" : "j/k  Entries     h/l  Topics     /  Search     Esc Close help"),
             m_layout.footer, fade(ink, .55F));
        return out;
    }
} // namespace hyprspace
