#include "HelpOverlay.hpp"

#include "CompositorHooks.hpp"
#include "Config.hpp"
#include "Texture.hpp"
#include "PassElements.hpp"
#include "SpaceStyle.hpp"

#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
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
        return {config::overviewZoomKey(),
                config::overviewEmptyWorkspaceKey(),
                config::overviewWindowViewKey(),
                config::overviewFollowKey(),
                config::overviewHelpKey(),
                config::overviewCloseWorkspaceKey(),
                config::overviewWorkspaceDragModifiers(),
                config::overviewWheelZoom(),
                m_session.cursorFollowEnabled(),
                config::followMouse(),
                config::warpCursor(),
                config::overviewShowFollowHint(),
                config::overviewAllMonitors()};
    }

    std::vector<SHelpEntry> CHelpOverlay::catalog(const SSettings& s) const {
        std::vector<SHelpEntry> entries;
        const auto              add = [&](const char* id, size_t topic, std::string title, std::string keys, std::string description, std::string tags = "",
                                          std::string setting = "") {
            entries.push_back({id, std::move(title), std::move(keys), std::move(description), std::move(tags), std::move(setting), topic});
        };
        const auto zoom = keyLabel(s.zoom), follow = keyLabel(s.follow);
        const auto leader = [](EInputAction action) {
            for (const auto& binding : INPUT_LEADER_BINDINGS)
                if (binding.action == action)
                    return std::format("Space → {}", binding.key);
            return std::string{};
        };
        const auto held = [&](std::string keys) { return !s.wheelZoom || zoom.empty() ? "Disabled" : zoom + " + " + keys; };
        add("help", 1, "Find a control", "? / Space → ?",
            "Open this guide from any navigation view. Use / to search every topic. Close help to return to the previous view and its draft.",
            "help documentation guide manual");
        add("overview", 1, "Open the overview", nativeShortcut("hyprspace:overview"),
            "Use your configured Hyprland binding to open workspaces. Your application keeps its place while you browse.", "overview open start launch");
        add("navigate", 1, "Choose a workspace", "hjkl / arrows", "Move to the nearest workspace in a direction. Tab and Shift+Tab step through the tiles.",
            "selection navigation vim keyboard");
        add("open", 1, "Open the selection", "Enter", "In Normal mode, commit the selected workspace. Clicking a window preview also focuses that window.",
            "confirm accept focus enter click");
        add("cancel", 1, "Return to your desktop", "Esc / right-click",
            "In Insert mode, Escape finishes editing and retains the query. In Normal mode, it returns to the previous layer, then to your original desktop.",
            "cancel dismiss exit back");
        add("digits", 1, "Jump to a workspace", "1–9 / 0", "Open a numbered workspace immediately. 0 selects workspace 10.", "number digit direct jump");
        add("ends", 1, "First or last workspace", "gg / G / Home / End", "Select the first or last workspace without committing it. Press Enter to open it.",
            "navigation first last");
        add("scrolling", 1, "Browse a scrolling workspace", "Wheel / PgUp / PgDn",
            "Scroll the workspace under the pointer. Its edge arrows reveal hidden columns while keeping the overview open.", "scroll scrolling columns touchpad tape");
        add("zoom", 2, "Inspect a workspace or window", zoom.empty() ? "Disabled" : "Hold " + zoom,
            "Enlarge the selected workspace, or the selected window in the window board. The cursor follows its point into zoom and back to the grid; repeated presses "
            "without mouse movement keep its position. You can move the mouse throughout.",
            "zoom inspect magnify fit", "overview:zoom_key");
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
        add("window-board", 3, "Switch to the window board", leader(EInputAction::ShowWindows),
            std::format("Gather windows from every covered output onto one board. {} returns to workspaces; Escape returns to the previous layer.",
                        leader(EInputAction::ShowWorkspaces)),
            "windows board global apps monitors");
        add("groups", 3, "Group the window board", leader(EInputAction::CycleGrouping),
            "Cycle All windows, Apps, Workspaces and Monitors. The toolbar selects a group directly.", "group grouping apps applications outputs monitors");
        add("window-search", 3, "Find a window", "/",
            "Press / from workspaces to open the window board and search immediately. Match window titles, applications, workspace names and monitor names. "
            "Combine an app name and title to find a specific window. Enter finishes editing; navigate the results, then Enter opens the selected window.",
            "search find filter title app class monitor");
        add("recent", 3, "Show recent windows first", leader(EInputAction::ToggleRecent),
            "Switch between location order and recent-first order within each group. The selected window keeps its identity.", "recent recency order sort history");
        add("window-navigation", 3, "Choose a window", "hjkl / arrows / Tab",
            "In Normal mode, move through the board; n and N step through matches. Keyboard navigation scrolls the selected window into view. Enter opens its original "
            "window.",
            "navigate vim selection focus");
        add("search-editing", 3, "Edit a search", "Ctrl+A / Ctrl+U",
            "Use / or i to enter Insert mode. Ctrl+A selects the query; Ctrl+U clears it. Backspace, Delete, arrows and Home/End edit text. Shift extends a selection. "
            "Enter or Escape returns to Normal mode and retains the query.",
            "search query text unicode edit erase delete");
        add("empty", 4, "Prepare an empty workspace", leader(EInputAction::NewWorkspace) + " / middle-click",
            "Select an empty workspace on the pointer's output and keep the overview open. Enter opens it; a subsequent launch uses that workspace.",
            "empty new workspace prepare create launch");
        add("move-window", 4, "Move a window", "Super + left-drag",
            "Drag a preview to rearrange its window or move it into a workspace on another output. Escape cancels the drag.",
            "move rearrange window drag monitor output");
        add("close-workspace", 4, "Close an empty workspace", leader(EInputAction::CloseWorkspace),
            "Remove the selected empty normal workspace and keep the overview open. An active workspace switches to the nearest remaining tile on its monitor. "
            "Workspaces with windows and the last normal workspace on a monitor are protected.",
            "close remove dismiss delete empty workspace");
        add("resize-window", 4, "Resize a window", "Super + right-drag", "Resize a window from its preview, scaled into the workspace tile. Escape cancels the resize.",
            "resize size window drag");
        add("move-workspace", 4, "Move a whole workspace", s.workspaceMods ? modifiers(s.workspaceMods) + " + drag" : "Disabled",
            "Left-drag a regular workspace to another output. Its number, name and layout travel with it; pinned windows stay on their original monitor.",
            "move workspace monitor output drag pinned", "overview:workspace_drag_modifiers");
        add("bindings", 5, "Configure keybindings", "Hyprland config",
            "Use Hyprland bindings for opening the overview. Inside it, the shared Normal and Insert modes provide navigation, commands and text editing. "
            "Zoom and cursor-follow settings remain under plugin:hyprspace:overview.",
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
            "Search across all topics in Insert mode. Enter or Escape returns to Normal mode and retains the query. j/k move between entries, n/N step through matches "
            "and h/l choose topics. Escape in Normal mode returns to the previous view.",
            "help search find filter topics vim");
        add("input-modes", 1, "Navigate or edit", "Normal / Insert",
            "Normal mode uses hjkl and arrows to navigate, Space for commands, / to search and i to edit a text field. Insert mode treats letters as text. "
            "Enter or Escape finishes editing without clearing the draft.",
            "modes normal insert keyboard editing navigation draft");
        add("reorder-spaces", 4, "Reorder spaces", "Shift+J / Shift+K",
            "In the Spaces chooser or manager, move the selected space down or up. Its saved order changes without switching spaces. Filtered lists move between "
            "matching spaces and retain the order of hidden entries.",
            "spaces reorder order sort move keyboard");
        add("search-icons", 4, "Find a space icon", "Tab → Icon → /",
            "Search icons by name or meaning, such as school or gaming. Enter or Escape finishes the search; hjkl and Page Up/Down choose an icon. Tab reaches "
            "Color and Ctrl+Enter saves the complete draft.",
            "space icon icons search appearance color colour");
        for (const auto& binding : INPUT_LEADER_BINDINGS) {
            const char* id    = nullptr;
            size_t      topic = 1;
            std::string description;
            switch (binding.action) {
            case EInputAction::ToggleSpaces:
                id = "spaces";
                description =
                    "Open the Spaces chooser inside the overview. Search or navigate its contexts, then Enter switches context. Escape returns to the previous layer.";
                break;
            case EInputAction::OpenManager:
                id          = "manage-spaces";
                topic       = 4;
                description = "Open the complete space manager. Shift+J/K reorders spaces; the shared leader commands create, edit and remove the selected space.";
                break;
            case EInputAction::Edit:
                id          = "edit-space";
                topic       = 4;
                description = "Edit the chosen space's name, icon and color. Use i to edit its name, Tab to reach Icon, / to search icons, and Ctrl+Enter to save.";
                break;
            case EInputAction::Create:
                id          = "create-space";
                topic       = 4;
                description = "Create a space with its own workspace context. Choose a name, icon and color; Ctrl+Enter saves the form.";
                break;
            case EInputAction::ShowWorkspaces:
                id          = "workspaces";
                description = "Return to the workspace grid from another navigation view.";
                break;
            case EInputAction::MoveWindow:
                id          = "move-window-to-space";
                topic       = 4;
                description = "Move only the selected window to another space, from either the workspace grid or window view. "
                              "The destination picker starts with Selected window; you can explicitly change the transfer type.";
                break;
            case EInputAction::MoveWorkspace:
                id          = "move-workspace-to-space";
                topic       = 4;
                description = "Move the selected workspace and every window it contains to another space, from either the workspace grid or window view. "
                              "The destination picker starts with Whole workspace; you can explicitly change the transfer type.";
                break;
            case EInputAction::Remove:
                id          = "remove-space";
                topic       = 4;
                description = "Review removal of the chosen space before confirming. The form identifies the affected space and destination.";
                break;
            default:
                break;
            }
            if (id)
                add(id, topic, std::string(binding.label), leader(binding.action), std::move(description), "space command navigation context");
        }
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

    void CHelpOverlay::focusSearch(bool selectAll) {
        if (!m_searchFocused) {
            m_searchFocused = true;
            ++m_generation;
        }
        if (selectAll)
            m_search.selectAll();
        resetCaret();
    }

    SInputContext CHelpOverlay::inputContext() const {
        return {EInputContext::Help, m_searchFocused ? EInputMode::Insert : EInputMode::Normal, "query", m_generation, true};
    }

    void CHelpOverlay::setInputMode(EInputMode mode) {
        if (mode == EInputMode::Insert)
            focusSearch(false);
        else
            blurSearch();
        m_session.damage();
    }

    void CHelpOverlay::editInput(ETextAction action, bool extend) {
        if (!m_active || !m_searchFocused)
            return;
        const auto previous = m_search.text();
        switch (action) {
        case ETextAction::Backspace:
            m_search.backspace();
            break;
        case ETextAction::Delete:
            m_search.eraseForward();
            break;
        case ETextAction::Left:
            m_search.left(extend);
            break;
        case ETextAction::Right:
            m_search.right(extend);
            break;
        case ETextAction::Home:
            m_search.home(extend);
            break;
        case ETextAction::End:
            m_search.end(extend);
            break;
        case ETextAction::SelectAll:
            m_search.selectAll();
            break;
        case ETextAction::Clear:
            m_search.clear();
            break;
        }
        if (previous != m_search.text())
            queryChanged();
        else
            resetCaret();
    }

    void CHelpOverlay::insertText(const std::string& text) {
        if (!m_active || !m_searchFocused)
            return;
        const auto previous = m_search.text();
        m_search.insert(text);
        if (previous != m_search.text())
            queryChanged();
        else
            resetCaret();
    }

    bool CHelpOverlay::perform(EInputAction action) {
        if (!m_active)
            return false;
        switch (action) {
        case EInputAction::Search:
            focusSearch();
            return true;
        case EInputAction::FocusText:
            setInputMode(EInputMode::Insert);
            return true;
        case EInputAction::FinishEditing:
        case EInputAction::Accept:
            setInputMode(EInputMode::Normal);
            return true;
        case EInputAction::MoveLeft:
            setTopic(m_browser.topic() ? m_browser.topic() - 1 : HELP_TOPICS.size() - 1);
            return true;
        case EInputAction::MoveRight:
            setTopic((m_browser.topic() + 1) % HELP_TOPICS.size());
            return true;
        case EInputAction::MoveDown:
        case EInputAction::NextField:
            m_browser.move(1);
            break;
        case EInputAction::MoveUp:
        case EInputAction::PrevField:
            m_browser.move(-1);
            break;
        case EInputAction::NextMatch:
            if (m_browser.count())
                m_browser.select((m_browser.selected() + 1) % m_browser.count());
            break;
        case EInputAction::PrevMatch:
            if (m_browser.count())
                m_browser.select((m_browser.selected() + m_browser.count() - 1) % m_browser.count());
            break;
        case EInputAction::PageNext:
            m_browser.move(std::max(1, static_cast<int>(m_layout.results.h / m_layout.rowHeight)));
            break;
        case EInputAction::PagePrevious:
            m_browser.move(-std::max(1, static_cast<int>(m_layout.results.h / m_layout.rowHeight)));
            break;
        case EInputAction::First:
            m_browser.edge(false);
            break;
        case EInputAction::Last:
            m_browser.edge(true);
            break;
        case EInputAction::Back:
            return false; // The controller restores the parent layer.
        default:
            return false;
        }
        ensureVisible();
        m_session.damage();
        return true;
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
        const double scale      = mon->m_scale;
        const auto   rgba       = [](const CHyprColor& c) { return SRgba{c.r, c.g, c.b, c.a}; };
        const auto   color      = [](const SRgba& c) { return CHyprColor{c.r, c.g, c.b, c.a}; };
        const auto   background = rgba(config::overviewBgColor()), label = rgba(config::overviewLabelColor()), active = rgba(config::overviewActiveBorder());
        const std::array<double, 12> colors{background.r, background.g, background.b, background.a, label.r,  label.g,
                                            label.b,      label.a,      active.r,     active.g,     active.b, active.a};
        const auto                   theme = config::spacesTheme();
        struct SStyleCache {
            std::array<double, 12> colors;
            std::string            theme;
            spaceStyle::SPalette   controls;
        };
        static std::optional<SStyleCache> cached;
        if (!cached || cached->colors != colors || cached->theme != theme)
            cached = SStyleCache{colors, theme, spaceStyle::palette(background, {}, label, active, theme)};
        const auto& style = cached->controls;
        const auto  ink = color(style.text), muted = color(style.muted), accent = color(style.focus);
        const auto  plate       = color(style.surface);
        const auto  px          = [&](const SBoxF& box) { return CBox{box.x * scale, box.y * scale, box.w * scale, box.h * scale}; };
        const auto  rect        = [&](SBoxF box, CHyprColor color, double round = 0) { renderPass::appendSolidFill(out, box, color, scale, round); };
        const auto  border      = [&](SBoxF box, CHyprColor color, double round = 0) { renderPass::appendOutline(out, box, color, scale, 1, round); };
        const auto  drawTexture = [&](SP<Render::ITexture> texture, SBoxF box, std::optional<SBoxF> clip = std::nullopt, bool centerY = true) {
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
        border(panel, color(style.border), 16);
        text("Hyprspace help", {m_layout.search.x, panel.y + 20, m_layout.search.w - 80, 30}, ink, true);
        rect(m_layout.close, color(style.hover), 6);
        text("Esc", {m_layout.close.x + 15, m_layout.close.y, m_layout.close.w - 20, m_layout.close.h}, ink);
        rect(m_layout.search, color(style.raised), 9);
        border(m_layout.search, m_searchFocused ? accent : color(style.border), 9);
        const auto content = searchContent();
        searchMetrics();
        const auto [from, to] = m_searchFocused && hooks::keyboardOwned() ? m_search.selection() : std::pair<size_t, size_t>{0, 0};
        const auto input =
            m_search.text().empty()
                ? textures().text(m_searchFocused ? "Type to search all help…" : "/  Search controls and settings", m_font, muted, static_cast<int>(content.w), scale)
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
                rect(box, color(style.selected), 7);
            text(HELP_TOPICS[i], {box.x + 12, box.y, box.w - 20, box.h}, i == m_browser.topic() && m_search.text().empty() ? ink : muted);
        }
        text(std::format("{} · {} {}", m_search.text().empty() ? HELP_TOPICS[m_browser.topic()] : "Search results", m_browser.count(),
                         m_browser.count() == 1 ? "entry" : "entries"),
             m_layout.heading, muted);
        const auto local = m_pointer - mon->m_position;
        for (size_t i = 0; i < m_browser.count(); ++i) {
            const auto box = rowBox(i);
            if (box.y + box.h <= m_layout.results.y || box.y >= m_layout.results.y + m_layout.results.h)
                continue;
            const auto&  entry    = m_browser.entry(i);
            const double clippedY = std::max(box.y, m_layout.results.y);
            const SBoxF  clipped{box.x, clippedY, box.w, std::max(0.0, std::min(box.y + box.h, m_layout.results.y + m_layout.results.h) - clippedY)};
            if (i == m_browser.selected() || box.contains(local.x, local.y))
                rect(clipped, color(i == m_browser.selected() ? style.selected : style.hover), 7);
            const double keysW = std::min(190.0, box.w * .37);
            text(entry.keys, {box.x + 12, box.y, keysW - 20, box.h}, i == m_browser.selected() ? ink : muted, false, m_layout.results);
            text(entry.title, {box.x + keysW, box.y, box.w - keysW - 12, box.h}, ink, i == m_browser.selected(), m_layout.results);
        }
        const double listHeight = m_browser.count() * m_layout.rowHeight;
        if (listHeight > m_layout.results.h) {
            const auto&  box   = m_layout.results;
            const double thumb = std::min(box.h, std::max(20.0, box.h * box.h / listHeight));
            rect({box.x + box.w - 3, box.y, 3, box.h}, color(style.raised), 1.5);
            rect({box.x + box.w - 3, box.y + m_scroll / (listHeight - box.h) * (box.h - thumb), 3, thumb}, color(style.border), 1.5);
        }
        if (!m_browser.count()) {
            text("No matching help", m_layout.results, ink, true);
            text("Try “keybinds”, “follow” or “resize”.", {m_layout.results.x, m_layout.results.cy() + 24, m_layout.results.w, 26}, muted);
        }
        if (const auto entry = m_browser.selection()) {
            const auto box = m_layout.detail;
            rect(box, color(style.raised), 9);
            const bool compact = box.h < 150;
            text(entry->title, {box.x + 16, box.y + (compact ? 8 : 10), box.w - 32, compact ? 26.0 : 30.0}, ink, true);
            const double settingH = entry->setting.empty() ? 0 : 22;
            const double bodyTop  = compact ? 36 : 45;
            const SBoxF  body{box.x + 16, box.y + bodyTop, box.w - 32, std::max(1.0, box.h - bodyTop - 8 - settingH)};
            drawTexture(textures().paragraph(entry->description, m_font, ink, static_cast<int>(body.w), static_cast<int>(body.h), scale), body, box, false);
            if (!entry->setting.empty())
                text(entry->setting, {box.x + 16, box.y + box.h - 28, box.w - 32, 22}, muted);
        }
        const bool compact = panel.w < 720;
        text(m_searchFocused ? (compact ? "Enter/Esc Done · Ctrl+U Clear" : "Enter / Esc  Finish editing     Ctrl+U  Clear")
                             : (compact ? "j/k ↑↓ · / Search · Esc Back" : "j/k  Entries     h/l  Topics     /  Search     Esc  Back"),
             m_layout.footer, muted);
        return out;
    }
} // namespace hyprspace
