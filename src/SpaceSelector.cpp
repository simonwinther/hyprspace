#include "SpaceSelector.hpp"

#include "CompositorHooks.hpp"
#include "Config.hpp"
#include "OverviewSession.hpp"
#include "Overview.hpp"
#include "Texture.hpp"
#include "PassElements.hpp"
#include "SpaceAppearance.hpp"
#include "SpaceStyle.hpp"
#include "WindowViewModel.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>

namespace hyprspace {
    CSpaceSelector& spaceSelector() {
        static CSpaceSelector selector;
        return selector;
    }

    namespace {
        SBoxF usableSpace(PHLMONITOR monitor) {
            if (!monitor)
                return {};
            const auto& r = monitor->m_reservedArea;
            return {r.left(), r.top(), std::max(1.0, monitor->m_size.x - r.left() - r.right()), std::max(1.0, monitor->m_size.y - r.top() - r.bottom())};
        }

        constexpr std::array<const char*, 8> SPACE_COLORS{"#89b4fa", "#a6e3a1", "#f9e2af", "#fab387", "#f38ba8", "#cba6f7", "#94e2d5", "#bac2de"};

        bool matchesFilter(const std::string& value, const std::string& query) {
            const auto  haystack = foldWindowSearch(value), needle = foldWindowSearch(query);
            std::string term;
            for (const char* p = needle.c_str(); *p; p = g_utf8_next_char(p)) {
                if (g_unichar_isspace(g_utf8_get_char(p))) {
                    if (!term.empty() && haystack.find(term) == std::string::npos)
                        return false;
                    term.clear();
                } else
                    term.append(p, g_utf8_next_char(p) - p);
            }
            return term.empty() || haystack.find(term) != std::string::npos;
        }

        CHyprColor appearanceColor(const std::string& value) {
            const auto normalized = spaces::normalizeSpaceColor(value).value_or(std::string(spaces::DEFAULT_SPACE_COLOR));
            const auto rgb        = std::stoul(normalized.substr(1), nullptr, 16);
            return CHyprColor{static_cast<double>((rgb >> 16) & 255) / 255, static_cast<double>((rgb >> 8) & 255) / 255, static_cast<double>(rgb & 255) / 255, 1};
        }

        double contextNameWidth(const std::string& name, double scale) {
            struct SMeasurement {
                std::string name, font;
                double      scale = 0, width = 0;
            };
            static std::array<SMeasurement, 32> measurements;
            static size_t                       next = 0;
            const auto                          font = config::overviewFont();
            const auto found = std::ranges::find_if(measurements, [&](const auto& value) { return value.name == name && value.font == font && value.scale == scale; });
            if (found != measurements.end())
                return found->width;
            int width = 0, height = 0;
            measureText(name, font, width, height, scale * 1.15);
            // Pango hinting depends on physical font size; leave room for the
            // integer maximum width used by the text texture cache as well.
            const double logicalWidth = std::ceil(width / scale) + 2;
            measurements[next]        = {name, font, scale, logicalWidth};
            next                      = (next + 1) % measurements.size();
            return logicalWidth;
        }
    } // namespace

    SBoxF CSpaceSelector::contentBounds(PHLMONITOR monitor) const {
        const auto usable = usableSpace(monitor);
        if (!spaces::enabled())
            return usable;
        const double inset = usable.w < 600 ? 12 : 24;
        const double top   = std::min(94.0, usable.h * .22);
        return {usable.x + inset, usable.y + top, std::max(1.0, usable.w - inset * 2), std::max(1.0, usable.h - top - inset)};
    }

    SBoxF CSpaceSelector::workspaceButtonBounds(PHLMONITOR monitor) const {
        const auto   content = contentBounds(monitor), usable = usableSpace(monitor);
        const double width = content.w >= 560 ? 220 : 36;
        return {content.x + content.w - width - 48, usable.y + 25, width, 34};
    }

    const std::vector<CSpaceSelector::SSpaceSummary>& CSpaceSelector::summaries() const {
        const auto revision = spaces::revision();
        if (m_summaryRevision == revision)
            return m_summaries;
        m_summaryRevision = revision;
        m_summaries.clear();
        const auto state = spaces::snapshot();
        for (const auto& value : state.value("spaces", nlohmann::json::array())) {
            SSpaceSummary summary{.id     = value.value("id", std::string{}),
                                  .name   = value.value("name", std::string{}),
                                  .icon   = value.value("icon", std::string(spaces::DEFAULT_SPACE_ICON)),
                                  .color  = value.value("color", std::string(spaces::DEFAULT_SPACE_COLOR)),
                                  .active = value.value("active", false)};
            for (const auto& workspace : value.value("workspaces", nlohmann::json::array())) {
                const auto windows = workspace.value("windows", size_t{0});
                summary.windows += windows;
                if (windows > 0 && !workspace.value("special", false))
                    ++summary.occupied;
            }
            m_summaries.push_back(std::move(summary));
        }
        return m_summaries;
    }

    bool CSpaceSelector::emptySpace() const {
        if (!spaces::enabled())
            return false;
        const auto& all    = summaries();
        const auto  active = std::ranges::find(all, true, &SSpaceSummary::active);
        return active != all.end() && active->windows == 0;
    }

    bool CSpaceSelector::modal() const {
        return m_mode != EMode::NONE;
    }

    bool CSpaceSelector::sidebarVisible() const {
        return m_mode == EMode::SIDEBAR;
    }

    PHLMONITOR CSpaceSelector::inputMonitor(PHLMONITOR monitor) const {
        if (monitor)
            return monitor;
        if (const auto existing = m_monitor.lock(); existing && existing->m_enabled)
            return existing;
        if (g_overviewSession)
            if (const auto view = session().keyboardView())
                return view->monitor();
        return nullptr;
    }

    void CSpaceSelector::openManager(PHLMONITOR monitor) {
        if (!spaces::enabled() || !g_overviewSession || !session().live())
            return;
        if (const auto target = inputMonitor(monitor))
            open(EMode::PICK, target);
    }

    SInputContext CSpaceSelector::inputContext() const {
        const auto context = m_mode == EMode::MENU                                ? EInputContext::SpaceMenu
                             : m_mode == EMode::PICK                              ? EInputContext::SpacePicker
                             : m_mode == EMode::CREATE || m_mode == EMode::RENAME ? EInputContext::SpaceEditor
                             : m_mode == EMode::REMOVE                            ? EInputContext::SpaceRemove
                             : m_mode == EMode::MOVE                              ? EInputContext::SpaceMove
                                                                                  : EInputContext::Spaces;
        return {context, m_inputMode, m_focus, m_generation,
                m_mode == EMode::SIDEBAR || m_mode == EMode::PICK || ((m_mode == EMode::CREATE || m_mode == EMode::RENAME) && (m_focus == "name" || m_focus == "icon"))};
    }

    CSpaceSelector::SFrame CSpaceSelector::frame() const {
        return {m_mode,  m_monitor,          m_window, m_workspace, m_source,    m_destination, m_error,      m_icon,     m_color,
                m_focus, m_iconSearchOrigin, m_input,  m_query,     m_editField, m_inputMode,   m_moveWindow, m_menuIndex};
    }

    void CSpaceSelector::restore(SFrame saved) {
        m_mode             = saved.mode;
        m_monitor          = saved.monitor;
        m_window           = saved.window;
        m_workspace        = saved.workspace;
        m_source           = std::move(saved.source);
        m_destination      = std::move(saved.destination);
        m_error            = std::move(saved.error);
        m_icon             = std::move(saved.icon);
        m_color            = std::move(saved.color);
        m_focus            = std::move(saved.focus);
        m_iconSearchOrigin = std::move(saved.iconSearchOrigin);
        m_input            = std::move(saved.input);
        m_query            = std::move(saved.query);
        m_editField        = saved.editField;
        m_inputMode        = saved.inputMode;
        m_moveWindow       = saved.moveWindow;
        m_menuIndex        = saved.menuIndex;
        ++m_generation;
        ensureChoice();
        session().damage();
    }

    void CSpaceSelector::back() {
        if (m_inputMode == EInputMode::Insert) {
            setInputMode(EInputMode::Normal);
            return;
        }
        if (m_parents.empty()) {
            close();
            return;
        }
        auto parent = std::move(m_parents.back());
        m_parents.pop_back();
        restore(std::move(parent));
    }

    void CSpaceSelector::focus(std::string field) {
        if (m_focus == field)
            return;
        m_focus     = std::move(field);
        m_inputMode = EInputMode::Normal;
        if (m_focus == "name")
            m_editField = EEditField::NAME;
        if (m_focus == "icon")
            m_editField = EEditField::ICON;
        if (m_focus == "color")
            m_editField = EEditField::COLOR;
        ++m_generation;
    }

    void CSpaceSelector::setInputMode(EInputMode mode) {
        if (mode == EInputMode::Insert && !textInput())
            return;
        if (m_inputMode != mode) {
            if (mode == EInputMode::Insert && m_focus == "icon")
                m_iconSearchOrigin = m_icon;
            m_inputMode = mode;
            ++m_generation;
            session().damage();
        }
    }

    CSearchInput* CSpaceSelector::textInput() {
        if (m_focus == "query")
            return &m_query;
        if (m_mode == EMode::CREATE || m_mode == EMode::RENAME) {
            if (m_focus == "icon")
                return &m_query;
            if (m_focus == "name")
                return &m_input;
        }
        return nullptr;
    }

    void CSpaceSelector::editInput(ETextAction action, bool extend) {
        auto* text = textInput();
        if (!text || m_inputMode != EInputMode::Insert)
            return;
        switch (action) {
        case ETextAction::Backspace:
            text->backspace();
            break;
        case ETextAction::Delete:
            text->eraseForward();
            break;
        case ETextAction::Left:
            text->left(extend);
            break;
        case ETextAction::Right:
            text->right(extend);
            break;
        case ETextAction::Home:
            text->home(extend);
            break;
        case ETextAction::End:
            text->end(extend);
            break;
        case ETextAction::SelectAll:
            text->selectAll();
            break;
        case ETextAction::Clear:
            text->clear();
            break;
        }
        m_error.clear();
        ensureChoice();
        session().damage();
    }

    void CSpaceSelector::insertText(const std::string& value) {
        if (auto* text = textInput(); text && m_inputMode == EInputMode::Insert) {
            text->insert(value);
            m_error.clear();
            ensureChoice();
            session().damage();
        }
    }

    const std::vector<CSpaceSelector::SChoice>& CSpaceSelector::choices() const {
        const auto revision = spaces::revision();
        if (m_choicesRevision == revision && m_choicesMode == m_mode && m_choicesQuery == m_query.text() && m_choicesSource == m_source)
            return m_choices;
        m_choicesRevision = revision;
        m_choicesMode     = m_mode;
        m_choicesQuery    = m_query.text();
        m_choicesSource   = m_source;
        auto& result      = m_choices;
        result.clear();
        for (const auto& space : summaries()) {
            if ((m_mode == EMode::REMOVE || m_mode == EMode::MOVE) && space.id == m_source)
                continue;
            if (!matchesFilter(space.name, m_query.text()))
                continue;
            result.push_back({space.id, space.name, space.icon, space.color,
                              space.windows ? std::format("{} window{}", space.windows, space.windows == 1 ? "" : "s") : "Empty", space.active});
        }
        return result;
    }

    std::string CSpaceSelector::selectedSpace() const {
        if (m_mode == EMode::SIDEBAR || m_mode == EMode::PICK) {
            const auto& values = choices();
            if (const auto selected = std::ranges::find(values, m_destination, &SChoice::id); selected != values.end())
                return selected->id;
        }
        return spaces::activeId();
    }

    std::vector<const spaces::SSpaceIcon*> CSpaceSelector::iconChoices() const {
        return spaces::filterSpaceIcons(m_query.text());
    }

    void CSpaceSelector::ensureChoice() {
        if (m_mode == EMode::CREATE || m_mode == EMode::RENAME) {
            const auto icons = iconChoices();
            if (icons.empty()) {
                // Matching prefixes may preview other icons while a query is
                // typed. No results must keep the user's pre-search choice.
                m_icon = m_iconSearchOrigin;
            } else if (std::ranges::none_of(icons, [&](const auto* icon) { return icon->id == m_icon; }))
                m_icon = icons.front()->id;
            return;
        }
        if (m_mode == EMode::MENU || m_mode == EMode::NONE)
            return;
        const auto& values = choices();
        if (std::ranges::find(values, m_destination, &SChoice::id) == values.end())
            m_destination = values.empty() ? std::string{} : values.front().id;
    }

    void CSpaceSelector::tabFocus(int delta) {
        std::vector<std::string> fields;
        if (m_mode == EMode::CREATE || m_mode == EMode::RENAME)
            fields = {"name", "icon", "color", "cancel", "confirm"};
        else if (m_mode == EMode::SIDEBAR)
            fields = {"list", "query"};
        else if (m_mode == EMode::PICK)
            fields = {"list", "query", "create", "rename", "remove", "move", "cancel", "confirm"};
        else if (m_mode == EMode::MOVE)
            fields = {"list", "type", "cancel", "confirm"};
        else if (m_mode == EMode::REMOVE)
            fields = {"list", "cancel", "confirm"};
        if (fields.empty())
            return;
        const auto it    = std::ranges::find(fields, m_focus);
        const int  index = it == fields.end() ? 0 : static_cast<int>(it - fields.begin());
        focus(fields[(index + delta + fields.size()) % fields.size()]);
    }

    void CSpaceSelector::toggleSidebar(PHLMONITOR monitor) {
        if (!spaces::enabled() || !g_overviewSession || !session().live())
            return;
        if (sidebarVisible()) {
            close();
            return;
        }
        if (!monitor)
            if (const auto view = session().keyboardView())
                monitor = view->monitor();
        if (!monitor)
            for (const auto& view : session().views)
                if (!view->closing() && view->monitor()) {
                    monitor = view->monitor();
                    break;
                }
        if (monitor)
            open(EMode::SIDEBAR, monitor);
    }

    void CSpaceSelector::close() {
        if (modal())
            ++m_generation;
        m_mode = EMode::NONE;
        m_monitor.reset();
        m_window.reset();
        m_workspace.reset();
        m_source.clear();
        m_destination.clear();
        m_error.clear();
        m_input.clear();
        m_query.clear();
        m_parents.clear();
        m_inputMode = EInputMode::Normal;
        m_focus     = "list";
        if (g_overviewSession)
            session().damage();
    }

    void CSpaceSelector::refresh() {
        if (modal() && (!spaces::enabled() || !m_monitor || !m_monitor->m_enabled || !session().covers(m_monitor.lock())))
            close();
        ensureChoice();
        if (g_overviewSession)
            session().damage();
    }

    bool CSpaceSelector::hasWindow() const {
        const auto window = m_window.lock();
        return hasWorkspace() && window && window->m_isMapped && !window->isHidden() && window->m_workspace == m_workspace.lock();
    }

    bool CSpaceSelector::hasWorkspace() const {
        const auto workspace = m_workspace.lock();
        return valid(workspace) && spaces::owner(workspace) == m_source;
    }

    void CSpaceSelector::setMoveWindow(bool selectedWindow) {
        m_moveWindow = selectedWindow;
        m_error.clear();
        if (m_destination.empty())
            m_error = "Create another space first.";
        else if (m_moveWindow && !hasWindow())
            m_error = "Select a window to move.";
        else if (!m_moveWindow && !hasWorkspace())
            m_error = "Select a workspace to move.";
    }

    std::vector<spaces::SSpaceInfo> CSpaceSelector::destinations() const {
        auto result = spaces::list();
        if (m_mode == EMode::REMOVE || m_mode == EMode::MOVE)
            std::erase_if(result, [&](const auto& value) { return value.id == m_source; });
        return result;
    }

    void CSpaceSelector::open(EMode mode, PHLMONITOR monitor) {
        if (!monitor || mode == m_mode)
            return;
        // Capture the command before closing overlays or cancelling inspection.
        // Modal pointer positions and native desktop focus are not replacements.
        const auto target        = session().selection.command();
        const bool keepSelection = (m_mode == EMode::MENU || m_mode == EMode::PICK || m_mode == EMode::SIDEBAR) && mode == EMode::MOVE;
        const auto source        = mode == EMode::RENAME || mode == EMode::REMOVE ? selectedSpace() : spaces::activeId();
        if (modal()) {
            if (m_parents.size() == 16)
                m_parents.erase(m_parents.begin());
            m_parents.push_back(frame());
        }
        session().cancelZoom();
        session().cancelPan();
        session().cancelDrag();
        session().closeHelp();
        hooks::suspendOverviewKeys();
        m_mode      = mode;
        m_menuIndex = 0;
        ++m_generation;
        m_monitor = monitor;
        m_source  = source;
        m_destination.clear();
        m_error.clear();
        m_input.clear();
        m_query.clear();
        m_inputMode = mode == EMode::CREATE ? EInputMode::Insert : EInputMode::Normal;
        m_focus     = mode == EMode::CREATE || mode == EMode::RENAME ? "name" : "list";
        m_editField = EEditField::NAME;
        m_icon      = spaces::DEFAULT_SPACE_ICON;
        m_color     = spaces::DEFAULT_SPACE_COLOR;
        if (mode == EMode::RENAME)
            for (const auto& space : spaces::list())
                if (space.id == m_source) {
                    m_input.insert(space.name);
                    m_input.selectAll();
                    m_icon  = space.icon;
                    m_color = space.color;
                }
        m_iconSearchOrigin = m_icon;
        if (!keepSelection) {
            m_window.reset();
            m_workspace.reset();
            if (target) {
                const auto workspace = State::workspaceState()->query().id(target->workspace.id).run();
                if (valid(workspace) && workspace->m_name == target->workspace.name) {
                    m_workspace = workspace;
                    m_window    = target->window;
                }
            }
        }
        if (target) {
            // Modal controls can overlap live previews. Their pointer position
            // must not replace the selection the management action was opened for.
            session().selection.keyboard(*target);
        }
        // The grid selects a workspace even when its target also identifies
        // a window under the pointer. Only the window board defaults to moving
        // an individual window; the user can still change the explicit toggle.
        m_moveWindow       = session().windowViewActive() && hasWindow();
        const auto choices = destinations();
        if (!choices.empty())
            m_destination = mode == EMode::PICK || mode == EMode::SIDEBAR ? spaces::activeId() : choices.front().id;
        ensureChoice();
        if ((mode == EMode::REMOVE || mode == EMode::MOVE) && choices.empty())
            m_error = "Create another space first.";
        if (mode == EMode::MOVE)
            setMoveWindow(m_moveWindow);
        session().damage();
    }

    void CSpaceSelector::selectDestination(int delta) {
        const auto& values = choices();
        if (values.empty())
            return;
        const auto it    = std::ranges::find(values, m_destination, &SChoice::id);
        const int  index = it == values.end() ? 0 : static_cast<int>(it - values.begin());
        m_destination    = values[std::clamp(index + delta, 0, static_cast<int>(values.size()) - 1)].id;
        if (m_mode == EMode::MOVE)
            setMoveWindow(m_moveWindow);
        else
            m_error.clear();
    }

    bool CSpaceSelector::perform(EInputAction action) {
        const bool editing = m_mode == EMode::CREATE || m_mode == EMode::RENAME;
        const bool form    = editing || m_mode == EMode::REMOVE || m_mode == EMode::MOVE;
        switch (action) {
        case EInputAction::MoveWindow:
        case EInputAction::MoveWorkspace:
            if (form)
                return true;
            if (const auto monitor = inputMonitor()) {
                open(EMode::MOVE, monitor);
                setMoveWindow(action == EInputAction::MoveWindow);
                session().damage();
            }
            return true;
        case EInputAction::Edit:
        case EInputAction::Create:
        case EInputAction::Move:
        case EInputAction::Remove:
            if (form)
                return true;
            if (const auto monitor = inputMonitor())
                open(action == EInputAction::Edit     ? EMode::RENAME
                     : action == EInputAction::Create ? EMode::CREATE
                     : action == EInputAction::Move   ? EMode::MOVE
                                                      : EMode::REMOVE,
                     monitor);
            return true;
        case EInputAction::ToggleSpaces:
            if (!form)
                toggleSidebar();
            return true;
        case EInputAction::OpenManager:
            if (!form)
                openManager();
            return true;
        default:
            break;
        }
        if (!modal())
            return false;
        switch (action) {
        case EInputAction::Back:
            back();
            return true;
        case EInputAction::ReorderEarlier:
        case EInputAction::ReorderLater: {
            if (m_mode != EMode::SIDEBAR && m_mode != EMode::PICK)
                return true;
            const auto& values   = choices();
            const auto  selected = std::ranges::find(values, m_destination, &SChoice::id);
            if (selected == values.end())
                return true;
            const auto index   = static_cast<size_t>(selected - values.begin());
            const bool earlier = action == EInputAction::ReorderEarlier;
            if ((earlier && index == 0) || (!earlier && index + 1 == values.size()))
                return true;
            const auto adjacent = values[earlier ? index - 1 : index + 1].id;
            const auto result =
                spaces::dispatch(nlohmann::json{{"action", "reorder"}, {"id", m_destination}, {"target", adjacent}, {"position", earlier ? "before" : "after"}}.dump());
            m_error = result.success ? "" : result.error;
            session().damage();
            return true;
        }
        case EInputAction::Save:
            if (form)
                submit();
            return true;
        case EInputAction::FinishEditing:
            setInputMode(EInputMode::Normal);
            if (m_focus == "query")
                focus("list");
            return true;
        case EInputAction::FocusText:
            if (editing)
                focus(m_focus == "icon" ? "icon" : "name");
            else if (m_mode == EMode::SIDEBAR || m_mode == EMode::PICK)
                focus("query");
            setInputMode(EInputMode::Insert);
            return true;
        case EInputAction::Search:
            if (editing && m_focus == "icon") {
                setInputMode(EInputMode::Insert);
                return true;
            }
            if (m_mode != EMode::SIDEBAR && m_mode != EMode::PICK)
                return true;
            focus("query");
            setInputMode(EInputMode::Insert);
            return true;
        case EInputAction::NextField:
        case EInputAction::PrevField:
            if (m_mode == EMode::MENU)
                m_menuIndex = (m_menuIndex + (action == EInputAction::NextField ? 1 : 2)) % 3;
            else
                tabFocus(action == EInputAction::NextField ? 1 : -1);
            break;
        case EInputAction::Accept:
            if (m_inputMode == EInputMode::Insert)
                return perform(EInputAction::FinishEditing);
            if (m_mode == EMode::MENU) {
                open(m_menuIndex == 0 ? EMode::RENAME : m_menuIndex == 1 ? EMode::MOVE : EMode::REMOVE, m_monitor.lock());
            } else if (m_focus == "cancel") {
                back();
            } else if (m_focus == "confirm") {
                submit();
            } else if (m_focus == "query" || (editing && m_focus == "name")) {
                setInputMode(EInputMode::Insert);
            } else if (editing) {
                focus("confirm");
            } else if (m_focus == "create" || m_focus == "rename" || m_focus == "move" || m_focus == "remove") {
                return perform(m_focus == "create"   ? EInputAction::Create
                               : m_focus == "rename" ? EInputAction::Edit
                               : m_focus == "move"   ? EInputAction::Move
                                                     : EInputAction::Remove);
            } else if (m_focus == "pick") {
                open(EMode::PICK, m_monitor.lock());
            } else {
                submit();
            }
            break;
        case EInputAction::MoveLeft:
        case EInputAction::MoveDown:
        case EInputAction::MoveUp:
        case EInputAction::MoveRight:
        case EInputAction::NextMatch:
        case EInputAction::PrevMatch:
        case EInputAction::PagePrevious:
        case EInputAction::PageNext: {
            const bool previous =
                action == EInputAction::MoveLeft || action == EInputAction::MoveUp || action == EInputAction::PrevMatch || action == EInputAction::PagePrevious;
            const int step = previous ? -1 : 1;
            if (m_mode == EMode::MENU)
                m_menuIndex = std::clamp(m_menuIndex + step, 0, 2);
            else if (editing) {
                if (m_focus == "icon" || m_focus == "color") {
                    const auto grid     = layout(m_monitor.lock());
                    const bool vertical = action == EInputAction::MoveUp || action == EInputAction::MoveDown;
                    const bool page     = action == EInputAction::PagePrevious || action == EInputAction::PageNext;
                    const int  distance = m_focus == "icon" ? (page ? grid.iconColumns * grid.iconRows : vertical ? grid.iconColumns : 1) : 1;
                    chooseAppearance(step * distance);
                } else if (m_focus != "name")
                    tabFocus(step);
            } else if (m_focus == "type" && m_mode == EMode::MOVE)
                setMoveWindow(previous);
            else {
                focus("list");
                selectDestination(step * (action == EInputAction::PagePrevious || action == EInputAction::PageNext ? 5 : 1));
            }
            break;
        }
        case EInputAction::First:
        case EInputAction::Last: {
            const bool last = action == EInputAction::Last;
            if (m_mode == EMode::MENU)
                m_menuIndex = last ? 2 : 0;
            else if (editing && m_focus == "icon") {
                const auto icons = iconChoices();
                if (!icons.empty())
                    m_icon = std::string(last ? icons.back()->id : icons.front()->id);
            } else if (editing && m_focus == "color")
                m_color = last ? SPACE_COLORS.back() : SPACE_COLORS.front();
            else if (!editing) {
                const auto& values = choices();
                if (!values.empty())
                    m_destination = last ? values.back().id : values.front().id;
            }
            break;
        }
        default:
            return false;
        }
        session().damage();
        return true;
    }

    void CSpaceSelector::chooseAppearance(int delta) {
        if (m_editField == EEditField::ICON) {
            const auto icons = iconChoices();
            if (icons.empty())
                return;
            const auto it    = std::ranges::find_if(icons, [&](const auto* icon) { return icon->id == m_icon; });
            const int  index = it == icons.end() ? 0 : static_cast<int>(it - icons.begin());
            m_icon           = icons[std::clamp(index + delta, 0, static_cast<int>(icons.size()) - 1)]->id;
        } else if (m_editField == EEditField::COLOR) {
            const auto it    = std::ranges::find(SPACE_COLORS, m_color);
            const int  index = it == SPACE_COLORS.end() ? 0 : static_cast<int>(it - SPACE_COLORS.begin());
            m_color          = SPACE_COLORS[(index + delta % static_cast<int>(SPACE_COLORS.size()) + SPACE_COLORS.size()) % SPACE_COLORS.size()];
        }
    }

    void CSpaceSelector::submit() {
        nlohmann::json request;
        switch (m_mode) {
        case EMode::CREATE:
            request = {{"action", "create"}, {"name", m_input.text()}, {"icon", m_icon}, {"color", m_color}};
            break;
        case EMode::RENAME:
            request = {{"action", "rename"}, {"id", m_source}, {"name", m_input.text()}, {"icon", m_icon}, {"color", m_color}};
            break;
        case EMode::REMOVE:
            if (m_destination.empty())
                return;
            request = {{"action", "delete"}, {"id", m_source}, {"target", m_destination}};
            break;
        case EMode::SIDEBAR:
        case EMode::PICK:
            if (m_destination.empty())
                return;
            request = {{"action", "switch"}, {"id", m_destination}};
            break;
        case EMode::MOVE:
            if (m_destination.empty())
                return;
            request = {{"space_id", m_destination}, {"target", m_moveWindow ? "window" : "workspace"}};
            if (m_moveWindow) {
                if (!hasWindow()) {
                    m_error = "The selected window is no longer available.";
                    session().damage();
                    return;
                }
                request["window"] = std::format("0x{:x}", reinterpret_cast<uintptr_t>(m_window.lock().get()));
            } else {
                const auto workspace = m_workspace.lock();
                if (!hasWorkspace()) {
                    m_error = "The selected workspace is no longer available.";
                    session().damage();
                    return;
                }
                request["workspace"] = workspace->m_id;
            }
            break;
        default:
            return;
        }
        const auto mode   = m_mode;
        const auto result = mode == EMode::MOVE ? spaces::move(request.dump()) : spaces::dispatch(request.dump());
        if (result.success) {
            if (modal() && (mode == EMode::RENAME || mode == EMode::REMOVE || mode == EMode::MOVE)) {
                m_inputMode = EInputMode::Normal;
                back();
            } else
                close();
        } else {
            m_error = result.error;
            session().damage();
        }
    }

    void CSpaceSelector::pointer(const Vector2D& point) {
        if (point != m_pointer) {
            m_pointer = point;
            session().damage();
        }
    }

    const STextCursor& CSpaceSelector::inputMetrics(double scale, bool query) const {
        const auto  font  = config::overviewFont();
        const auto& input = !query && (m_mode == EMode::CREATE || m_mode == EMode::RENAME) ? m_input : m_query;
        if (m_measuredText != input.text() || m_measuredCursor != input.cursor() || m_measuredFont != font || m_measuredScale != scale) {
            m_measuredText   = input.text();
            m_measuredCursor = input.cursor();
            m_measuredFont   = font;
            m_measuredScale  = scale;
            m_inputMetrics   = measureTextCursor(m_measuredText, m_measuredFont, m_measuredCursor, scale);
        }
        return m_inputMetrics;
    }

    CSpaceSelector::SLayout CSpaceSelector::layout(PHLMONITOR monitor, bool chromeOnly) const {
        SLayout result;
        if (!monitor)
            return result;
        const auto&  reserved = monitor->m_reservedArea;
        const double width    = std::max(1.0, monitor->m_size.x - reserved.left() - reserved.right() - 24);
        const auto   usable   = usableSpace(monitor);
        result.content        = contentBounds(monitor);
        result.header         = {result.content.x, usable.y + 16, result.content.w, 64};
        if (!modal() || chromeOnly) {
            const auto&  all          = summaries();
            const auto   active       = std::ranges::find(all, true, &SSpaceSummary::active);
            const auto   name         = active == all.end() ? std::string("Spaces") : active->name;
            const double actions      = result.content.w >= 560 ? 292 : 104;
            const double maximum      = std::min(280.0, std::max(96.0, result.header.w - actions - 12));
            const double triggerWidth = std::min(maximum, std::max(144.0, contextNameWidth(name, monitor->m_scale) + 80));
            result.buttons.push_back({{result.header.x, result.header.y + 2, triggerWidth, 38}, EAction::SIDEBAR, "", name});
            result.buttons.push_back({{result.header.x + result.header.w - 36, result.header.y + 9, 36, 34}, EAction::MENU, "", "···"});
            if (emptySpace() && !session().windowViewActive()) {
                const auto&  c = result.content;
                const double w = std::min(440.0, c.w - 24), h = std::min(220.0, c.h - 24);
                result.empty = {c.cx() - w / 2, c.cy() - h / 2, w, h};
                result.buttons.push_back({{result.empty.cx() - 72, result.empty.y + 132, 144, 38}, EAction::ENTER, "", "Enter space", true});
            }
            return result;
        }
        if (monitor != m_monitor.lock())
            return result;
        if (m_mode == EMode::SIDEBAR) {
            const auto&      all       = choices();
            const double     top       = result.header.y + 81;
            const double     available = std::max(1.0, usable.y + usable.h - top - 12);
            constexpr double overhead = 110, rowHeight = 44;
            const size_t     capacity = std::max(1, static_cast<int>((available - overhead) / rowHeight));
            const size_t     rows     = std::min(std::max<size_t>(1, all.size()), capacity);
            result.rail          = {result.header.x, top, std::min(304.0, usable.w - (result.header.x - usable.x) * 2), std::min(available, overhead + rows * rowHeight)};
            result.panel         = result.rail;
            const auto& rail     = result.rail;
            result.search        = {rail.x + 12, rail.y + 8, rail.w - 24, 34};
            const double listTop = rail.y + 50;
            const auto   chosen  = std::ranges::find(all, m_destination, &SChoice::id);
            const size_t selected = chosen == all.end() ? 0 : chosen - all.begin();
            const size_t first    = all.size() <= rows ? 0 : std::min(selected > rows / 2 ? selected - rows / 2 : 0, all.size() - rows);
            for (size_t i = first; i < std::min(all.size(), first + rows); ++i) {
                const auto& value = all[i];
                result.buttons.push_back({{rail.x + 8, listTop + (i - first) * rowHeight, rail.w - 16, rowHeight - 4},
                                          EAction::SWITCH,
                                          value.id,
                                          value.name,
                                          value.current,
                                          value.detail,
                                          m_focus == "list" && value.id == m_destination});
            }
            return result;
        }
        if (m_mode == EMode::MENU) {
            result.panel  = {result.header.x + std::max(0.0, result.header.w - 240), result.header.y + 50, std::min(240.0, result.header.w), 166};
            const auto& p = result.panel;
            result.buttons.push_back({{p.x + 8, p.y + 8, p.w - 16, 44}, EAction::RENAME, "", "Edit space", m_menuIndex == 0});
            result.buttons.push_back({{p.x + 8, p.y + 58, p.w - 16, 44}, EAction::MOVE, "", "Move selection to…", m_menuIndex == 1});
            result.buttons.push_back({{p.x + 8, p.y + 108, p.w - 16, 44}, EAction::REMOVE, "", "Remove space…", m_menuIndex == 2});
            return result;
        }
        const double usableHeight = monitor->m_size.y - reserved.top() - reserved.bottom();
        const bool   editing      = m_mode == EMode::CREATE || m_mode == EMode::RENAME;
        const double panelWidth   = std::min(editing ? 432.0 : 650.0, width - 16);
        const bool   compact      = m_mode == EMode::PICK && panelWidth < 520;
        const auto&  choices      = this->choices();
        const double searchHeight = m_mode == EMode::PICK ? 42 : 0;
        const size_t rows = editing ? 0 : std::min<size_t>(choices.size(), std::max(1, static_cast<int>((usableHeight - 340 - searchHeight - (compact ? 42 : 0)) / 42)));
        const double panelHeight = editing ? std::min(560.0, usableHeight - 24) : std::min(usableHeight - 32, 295.0 + searchHeight + rows * 42 + (compact ? 42 : 0));
        result.panel             = {reserved.left() + (width + 24 - panelWidth) / 2, reserved.top() + (usableHeight - panelHeight) / 2, panelWidth, panelHeight};
        const auto& p            = result.panel;
        if (editing) {
            const bool   compactEditor = p.h < 480;
            const double nameY         = compactEditor ? 44 : 58;
            const double inputY        = compactEditor ? 66 : 84;
            const double iconY         = compactEditor ? 110 : 140;
            const double searchY       = compactEditor ? 134 : 166;
            const double gridY         = compactEditor ? 174 : 210;
            const double cellHeight    = compactEditor ? 34 : 42;
            result.input               = {p.x + 24, p.y + inputY, p.w - 48, compactEditor ? 34.0 : 40.0};
            result.search              = {p.x + 24, p.y + searchY, p.w - 48, compactEditor ? 30.0 : 34.0};
            result.iconLabel           = {p.x + 24, p.y + iconY, p.w - 48, 22};
            result.colorLabel          = {p.x + 24, p.y + p.h - 154, p.w - 48, 22};
            result.hint                = {p.x + 24, p.y + p.h - 86, p.w - 48, 20};
            result.details             = {p.x + 24, p.y + nameY, p.w - 48, 20};
            const auto icons           = iconChoices();
            result.iconColumns         = std::clamp<size_t>(static_cast<size_t>(std::max(1.0, (p.w - 48) / 48)), 1, 6);
            result.iconRows            = std::clamp<size_t>(static_cast<size_t>(std::max(1.0, (p.h - 166 - gridY) / cellHeight)), 1, 4);
            const double cellWidth     = (p.w - 48) / result.iconColumns;
            result.iconGrid            = {p.x + 24, p.y + gridY, p.w - 48, result.iconRows * cellHeight};
            const auto selected        = std::ranges::find_if(icons, [&](const auto* icon) { return icon->id == m_icon; });
            const auto viewport = spaces::spaceIconViewport(icons.size(), selected == icons.end() ? 0 : selected - icons.begin(), result.iconColumns, result.iconRows);
            result.iconFirst    = viewport.first;
            result.iconTotal    = icons.size();
            for (size_t i = viewport.first; i < viewport.first + viewport.count; ++i) {
                const auto& icon   = *icons[i];
                const auto  offset = i - viewport.first;
                result.buttons.push_back({{result.iconGrid.x + (offset % result.iconColumns) * cellWidth + 2,
                                           result.iconGrid.y + (offset / result.iconColumns) * cellHeight, cellWidth - 4, cellHeight - 4},
                                          EAction::ICON,
                                          std::string(icon.id),
                                          std::string(icon.label),
                                          icon.id == m_icon,
                                          "",
                                          m_focus == "icon" && icon.id == m_icon});
            }
            if (viewport.previous)
                result.buttons.push_back({{p.x + p.w - 72, result.iconLabel.y, 24, 22}, EAction::ICON_PREV, "", "↑"});
            if (viewport.next)
                result.buttons.push_back({{p.x + p.w - 48, result.iconLabel.y, 24, 22}, EAction::ICON_NEXT, "", "↓"});
            const double swatchStep = std::min(38.0, (p.w - 48) / SPACE_COLORS.size());
            for (size_t i = 0; i < SPACE_COLORS.size(); ++i)
                result.buttons.push_back({{p.x + 24 + i * swatchStep, p.y + p.h - 126, std::min(28.0, swatchStep - 4), 28},
                                          EAction::COLOR,
                                          SPACE_COLORS[i],
                                          "",
                                          SPACE_COLORS[i] == m_color,
                                          "",
                                          m_focus == "color" && SPACE_COLORS[i] == m_color});
        } else {
            double top = p.y + 70;
            if (m_mode == EMode::PICK) {
                result.search = {p.x + 24, top, p.w - 48, 34};
                top += 42;
            }
            if (m_mode == EMode::MOVE) {
                result.buttons.push_back(
                    {{p.x + 24, top, (p.w - 56) / 2, 32}, EAction::WINDOW, "", "Selected window", m_moveWindow, "", m_focus == "type" && m_moveWindow});
                result.buttons.push_back({{p.x + 32 + (p.w - 56) / 2, top, (p.w - 56) / 2, 32},
                                          EAction::WORKSPACE,
                                          "",
                                          "Whole workspace",
                                          !m_moveWindow,
                                          "",
                                          m_focus == "type" && !m_moveWindow});
                top += 42;
            }
            const auto   chosen   = std::ranges::find(choices, m_destination, &SChoice::id);
            const size_t selected = chosen == choices.end() ? 0 : chosen - choices.begin();
            const size_t first    = rows ? std::min(selected > rows / 2 ? selected - rows / 2 : 0, choices.size() - rows) : 0;
            for (size_t i = first; i < first + rows; ++i)
                result.buttons.push_back({{p.x + 24, top + (i - first) * 42, p.w - 48, 36},
                                          EAction::DESTINATION,
                                          choices[i].id,
                                          choices[i].name + (choices[i].current ? " · current" : ""),
                                          choices[i].id == m_destination,
                                          "",
                                          m_focus == "list" && choices[i].id == m_destination});
            top += rows * 42 + 8;
            result.details = {p.x + 24, top, p.w - 48, std::max(0.0, p.y + p.h - (m_mode == EMode::PICK ? (compact ? 154 : 112) : 72) - top)};
        }
        const double footerY      = p.y + p.h - 54;
        const double confirmWidth = m_mode == EMode::REMOVE ? 186 : m_mode == EMode::CREATE || m_mode == EMode::RENAME ? 132 : 100;
        result.buttons.push_back({{p.x + p.w - confirmWidth - 126, footerY, 94, 34}, EAction::CANCEL, "", "Cancel", false, "", m_focus == "cancel"});
        const char* confirm = m_mode == EMode::REMOVE   ? "Transfer & remove"
                              : m_mode == EMode::MOVE   ? "Move"
                              : m_mode == EMode::PICK   ? "Switch"
                              : m_mode == EMode::CREATE ? "Create space"
                                                        : "Save changes";
        result.buttons.push_back({{p.x + p.w - confirmWidth - 24, footerY, confirmWidth, 34}, EAction::CONFIRM, "", confirm, true, "", m_focus == "confirm"});
        if (m_mode == EMode::PICK) {
            const double                          w          = compact ? (p.w - 56) / 2 : (p.w - 72) / 4;
            const std::pair<EAction, const char*> controls[] = {
                {EAction::CREATE, "+ Create"}, {EAction::RENAME, "Rename"}, {EAction::REMOVE, "Remove"}, {EAction::MOVE, "Move to…"}};
            for (size_t i = 0; i < std::size(controls); ++i) {
                const double bx = p.x + 24 + (compact ? i % 2 : i) * (w + 8);
                const double by = compact ? footerY - 84 + (i / 2) * 42 : footerY - 42;
                result.buttons.push_back({{bx, by, w, 32},
                                          controls[i].first,
                                          "",
                                          controls[i].second,
                                          false,
                                          "",
                                          m_focus == (i == 0   ? "create"
                                                      : i == 1 ? "rename"
                                                      : i == 2 ? "remove"
                                                               : "move")});
            }
        }
        return result;
    }

    std::string CSpaceSelector::details() const {
        if (!m_error.empty())
            return m_error;
        if (m_mode == EMode::REMOVE && !m_destination.empty()) {
            const auto revision = spaces::revision();
            if (m_previewRevision != revision || m_previewSource != m_source || m_previewDestination != m_destination) {
                m_removePreview      = spaces::previewRemove(m_source, m_destination);
                m_previewRevision    = revision;
                m_previewSource      = m_source;
                m_previewDestination = m_destination;
            }
            const auto& preview = m_removePreview;
            if (preview.contains("error") && preview["error"].is_string() && !preview["error"].get_ref<const std::string&>().empty())
                return preview["error"].get<std::string>();
            const auto  total = preview.value("total", size_t{0}), windows = preview.value("windows", size_t{0});
            std::string text = std::format("{} workspace{} · {} window{}. Every workspace moves intact; destination workspaces stay separate.", total,
                                           total == 1 ? "" : "s", windows, windows == 1 ? "" : "s");
            if (preview.contains("workspaces")) {
                size_t count = 0;
                for (const auto& item : preview["workspaces"]) {
                    if (count++ >= 4) {
                        text += "  …";
                        break;
                    }
                    const auto old = item.value("number", int64_t{0}), next = item.value("new_number", old);
                    text += std::format("  {} → {}", old > 0 ? std::to_string(old) : item.value("name", std::string("Named")),
                                        next > 0 ? std::to_string(next) : item.value("new_name", item.value("name", std::string("Named"))));
                }
            }
            return text;
        }
        if (m_mode == EMode::MOVE)
            return m_moveWindow ? "Move only the selected window to another space. Other windows stay here."
                                : "Move the selected workspace and all its windows to another space.";
        if (m_mode == EMode::PICK)
            return "Shift + J/K to reorder · Enter switches space";
        return "";
    }

    void CSpaceSelector::activate(const SButton& value, PHLMONITOR monitor) {
        switch (value.action) {
        case EAction::PREV:
            spaces::dispatch("prev");
            break;
        case EAction::NEXT:
            spaces::dispatch("next");
            break;
        case EAction::SWITCH:
            close();
            spaces::switchTo(value.id);
            break;
        case EAction::SIDEBAR:
            toggleSidebar(monitor);
            break;
        case EAction::MENU:
            open(EMode::MENU, monitor);
            break;
        case EAction::PICK:
            open(EMode::PICK, monitor);
            break;
        case EAction::CREATE:
            open(EMode::CREATE, monitor);
            break;
        case EAction::RENAME:
            open(EMode::RENAME, monitor);
            break;
        case EAction::REMOVE:
            open(EMode::REMOVE, monitor);
            break;
        case EAction::MOVE:
            open(EMode::MOVE, monitor);
            break;
        case EAction::ENTER:
            for (const auto& view : session().views)
                if (view->monitor() == monitor && !view->closing()) {
                    view->close(true);
                    break;
                }
            for (const auto& view : session().views)
                view->close(false);
            session().stopInput();
            break;
        case EAction::ICON:
            m_icon = value.id;
            setInputMode(EInputMode::Normal);
            focus("icon");
            break;
        case EAction::ICON_PREV:
        case EAction::ICON_NEXT: {
            setInputMode(EInputMode::Normal);
            focus("icon");
            const auto positions = layout(monitor);
            chooseAppearance((value.action == EAction::ICON_PREV ? -1 : 1) * static_cast<int>(positions.iconColumns * positions.iconRows));
            break;
        }
        case EAction::COLOR:
            m_color = value.id;
            focus("color");
            break;
        case EAction::DESTINATION:
            focus("list");
            m_destination = value.id;
            if (m_mode == EMode::MOVE)
                setMoveWindow(m_moveWindow);
            else
                m_error.clear();
            break;
        case EAction::WINDOW:
            focus("type");
            setMoveWindow(true);
            break;
        case EAction::WORKSPACE:
            focus("type");
            setMoveWindow(false);
            break;
        case EAction::CONFIRM:
            submit();
            break;
        case EAction::CANCEL:
            m_inputMode = EInputMode::Normal;
            back();
            break;
        }
        session().damage();
    }

    bool CSpaceSelector::button(uint32_t button, bool pressed, uint32_t mods, const Vector2D& point) {
        if (!spaces::enabled() || session().helpActive() || !session().live() || session().zoomHeld())
            return false;
        // A release belongs to the gesture which captured its press, even
        // when that gesture ends over the toolbar.
        if (!modal() && (!pressed || session().gestureActive() || session().pendingResize))
            return false;
        const auto monitor = State::monitorState()->query().vec(point).run();
        if (!monitor || !session().covers(monitor))
            return modal();
        const auto local     = point - monitor->m_position;
        const auto positions = layout(monitor);
        for (const auto& value : positions.buttons)
            if (value.box.contains(local.x, local.y)) {
                if (pressed && button == 0x110)
                    activate(value, monitor);
                return true;
            }
        if (!modal())
            return positions.rail.contains(local.x, local.y) ||
                   (positions.header.contains(local.x, local.y) && !workspaceButtonBounds(monitor).contains(local.x, local.y)) ||
                   (emptySpace() && positions.content.contains(local.x, local.y));
        if ((m_mode == EMode::MENU || m_mode == EMode::SIDEBAR) && pressed && button == 0x110 && !positions.panel.contains(local.x, local.y)) {
            m_inputMode = EInputMode::Normal;
            back();
            return true;
        }
        if (pressed && button == 0x110 && positions.search.contains(local.x, local.y)) {
            focus(m_mode == EMode::CREATE || m_mode == EMode::RENAME ? "icon" : "query");
            setInputMode(EInputMode::Insert);
            const auto&  metrics = inputMetrics(monitor->m_scale, true);
            const double offset  = std::max(0.0, metrics.x / monitor->m_scale - (positions.search.w - 24) + 2);
            m_query.setCursor(textIndexAtX(m_query.text(), config::overviewFont(), local.x - positions.search.x - 12 + offset, monitor->m_scale),
                              mods == HL_MODIFIER_SHIFT);
            session().damage();
            return true;
        }
        if (pressed && button == 0x110 && positions.input.contains(local.x, local.y)) {
            focus("name");
            setInputMode(EInputMode::Insert);
            const auto   font    = config::overviewFont();
            const auto&  metrics = inputMetrics(monitor->m_scale);
            const double offset  = std::max(0.0, metrics.x / monitor->m_scale - (positions.input.w - 24) + 2);
            m_input.setCursor(textIndexAtX(m_input.text(), font, local.x - positions.input.x - 12 + offset, monitor->m_scale), mods == HL_MODIFIER_SHIFT);
            session().damage();
        }
        return true;
    }

    nlohmann::json CSpaceSelector::inspect() const {
        const char*    mode = m_mode == EMode::CREATE    ? "create"
                              : m_mode == EMode::RENAME  ? "rename"
                              : m_mode == EMode::REMOVE  ? "remove"
                              : m_mode == EMode::MOVE    ? "move"
                              : m_mode == EMode::PICK    ? "pick"
                              : m_mode == EMode::MENU    ? "menu"
                              : m_mode == EMode::SIDEBAR ? "sidebar"
                                                         : "none";
        nlohmann::json result{
            {"active", modal()},
            {"sidebar_visible", sidebarVisible()},
            {"toolbar_visible", spaces::enabled() && g_overviewSession && session().live() && !modal() && !session().helpActive() && !session().zoomHeld()},
            {"mode", mode},
            {"generation", m_generation},
            {"input_mode", inputModeName(m_inputMode)},
            {"field", m_focus},
            {"parent_depth", m_parents.size()},
            {"query", m_query.text()},
            {"query_cursor", m_query.cursor()},
            {"input", m_input.text()},
            {"input_cursor", m_input.cursor()},
            {"icon", m_icon},
            {"icon_matches", (m_mode == EMode::CREATE || m_mode == EMode::RENAME) ? iconChoices().size() : 0},
            {"color", m_color},
            {"edit_field", m_editField == EEditField::NAME   ? "name"
                           : m_editField == EEditField::ICON ? "icon"
                                                             : "color"},
            {"source", m_source},
            {"destination", m_destination},
            {"move_window", m_moveWindow},
            {"transfer_scope", m_moveWindow ? "window" : "workspace"},
            {"selected_window", std::format("0x{:x}", reinterpret_cast<uintptr_t>(m_window.lock().get()))},
            {"selected_workspace", m_workspace ? m_workspace->m_id : 0},
            {"error", m_error},
            {"buttons", nlohmann::json::array()},
            {"toolbar_height", toolbarHeight()}};
        if (!spaces::enabled() || !g_overviewSession || !session().live())
            return result;
        const auto actionName = [](EAction action) {
            switch (action) {
            case EAction::PREV:
                return "prev";
            case EAction::NEXT:
                return "next";
            case EAction::SWITCH:
                return "switch";
            case EAction::SIDEBAR:
                return "sidebar";
            case EAction::MENU:
                return "menu";
            case EAction::PICK:
                return "pick";
            case EAction::CREATE:
                return "create";
            case EAction::RENAME:
                return "rename";
            case EAction::REMOVE:
                return "remove";
            case EAction::MOVE:
                return "move";
            case EAction::ENTER:
                return "enter";
            case EAction::ICON:
                return "icon";
            case EAction::ICON_PREV:
                return "icon_previous";
            case EAction::ICON_NEXT:
                return "icon_next";
            case EAction::COLOR:
                return "color";
            case EAction::DESTINATION:
                return "destination";
            case EAction::WINDOW:
                return "window";
            case EAction::WORKSPACE:
                return "workspace";
            case EAction::CONFIRM:
                return "confirm";
            case EAction::CANCEL:
                return "cancel";
            }
            return "";
        };
        for (const auto& view : session().views) {
            const auto monitor = view->monitor();
            if (!monitor || view->closing())
                continue;
            const auto positions = layout(monitor);
            const auto box       = [&](SBoxF value) {
                return nlohmann::json{{"x", value.x + monitor->m_position.x}, {"y", value.y + monitor->m_position.y}, {"w", value.w}, {"h", value.h}};
            };
            result["layout"][monitor->m_name] = {
                {"rail", box(positions.rail)}, {"header", box(positions.header)}, {"content", box(positions.content)}, {"empty", box(positions.empty)}};
            if (!session().zoomHeld())
                for (const auto& value : positions.buttons)
                    result["buttons"].push_back({{"action", actionName(value.action)},
                                                 {"id", value.id},
                                                 {"label", value.label},
                                                 {"active", value.active},
                                                 {"focused", value.focused},
                                                 {"monitor", monitor->m_name},
                                                 {"box", box(value.box)}});
            if (modal() && monitor == m_monitor.lock()) {
                result["monitor"]     = monitor->m_name;
                result["panel"]       = box(positions.panel);
                result["input_box"]   = box(positions.input);
                result["search_box"]  = box(positions.search);
                result["details_box"] = box(positions.details);
                if (m_mode == EMode::CREATE || m_mode == EMode::RENAME) {
                    result["icon_grid"]     = box(positions.iconGrid);
                    result["icon_viewport"] = {
                        {"first", positions.iconFirst}, {"total", positions.iconTotal}, {"columns", positions.iconColumns}, {"rows", positions.iconRows}};
                }
            }
        }
        result["details"]     = details();
        result["empty_space"] = emptySpace();
        result["spaces"]      = nlohmann::json::array();
        for (const auto& space : summaries())
            result["spaces"].push_back({{"id", space.id},
                                        {"name", space.name},
                                        {"icon", space.icon},
                                        {"color", space.color},
                                        {"occupied_workspaces", space.occupied},
                                        {"windows", space.windows},
                                        {"active", space.active}});
        if (m_mode == EMode::REMOVE)
            result["preview"] = m_removePreview;
        return result;
    }

    std::vector<UP<IPassElement>> CSpaceSelector::buildPass(PHLMONITOR monitor) const {
        std::vector<UP<IPassElement>> output;
        if (!spaces::enabled() || !monitor || !session().live() || !session().covers(monitor) || session().helpActive() || session().zoomHeld())
            return output;
        const auto   positions = layout(monitor);
        const double scale     = monitor->m_scale;
        const auto   rgba      = [](const CHyprColor& color) { return SRgba{color.r, color.g, color.b, color.a}; };
        const auto   color     = [](const SRgba& color) {
            return CHyprColor{static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), static_cast<float>(color.a)};
        };
        const auto background = rgba(config::overviewBgColor()), title = rgba(config::overviewTitleBgColor()), label = rgba(config::overviewLabelColor()),
                   activeBorder = rgba(config::overviewActiveBorder());
        const std::array<double, 16> colors{background.r, background.g, background.b, background.a, title.r,        title.g,        title.b,        title.a,
                                            label.r,      label.g,      label.b,      label.a,      activeBorder.r, activeBorder.g, activeBorder.b, activeBorder.a};
        const auto                   mode = config::spacesTheme();
        // Every output uses the same configured colors. Contrast correction is
        // independent of monitor geometry and only needs to run when they change.
        if (!m_styleCache || m_styleCache->colors != colors || m_styleCache->mode != mode)
            m_styleCache = SStyleCache{colors, mode, spaceStyle::palette(background, title, label, activeBorder, mode),
                                       spaceStyle::palette(background, background, label, activeBorder)};
        const auto& style         = m_styleCache->controls;
        const auto& overviewStyle = m_styleCache->overview;
        const auto  ink = color(style.text), muted = color(style.muted), subtle = color(style.subtle), accent = color(style.focus), plate = color(style.surface);
        const auto  overviewInk = color(overviewStyle.text), overviewMuted = color(overviewStyle.muted);
        const auto  identityColor    = [&](const std::string& value) { return color(spaceStyle::identity(rgba(appearanceColor(value)), style)); };
        const auto  overviewIdentity = [&](const std::string& value) { return color(spaceStyle::identity(rgba(appearanceColor(value)), overviewStyle)); };
        const auto  px               = [&](SBoxF box) { return CBox{box.x * scale, box.y * scale, box.w * scale, box.h * scale}; };
        const auto  rect             = [&](SBoxF box, CHyprColor color, int round = 7) { renderPass::appendSolidFill(output, box, color, scale, round); };
        const auto  text             = [&](const std::string& content, SBoxF box, CHyprColor color, bool paragraph = false, double factor = 1, bool centered = false,
                                           bool rightAligned = false) {
            if (content.empty() || box.w < 1 || box.h < 1)
                return;
            const auto texture = paragraph ? textures().paragraph(content, config::overviewFont(), color, box.w, box.h, scale)
                                           : textures().text(content, config::overviewFont(), color, box.w / factor, scale * factor);
            if (!texture)
                return;
            const auto size = texture->m_size / scale;
            output.emplace_back(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{.tex     = texture,
                                                                                         .box     = px({rightAligned ? box.x + box.w - size.x
                                                                                                        : centered   ? box.cx() - size.x / 2
                                                                                                                     : box.x,
                                                                                                        paragraph ? box.y : box.cy() - size.y / 2, size.x, size.y}),
                                                                                         .a       = 1,
                                                                                         .clipBox = px(box)}));
        };
        const auto icon = [&](const std::string& key, CHyprColor color, SBoxF box) {
            if (const auto texture = textures().spaceIcon(key, color, static_cast<int>(std::ceil(std::min(box.w, box.h) * scale))))
                output.emplace_back(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{.tex = texture, .box = px(box), .a = 1}));
        };
        const auto  outline     = [&](SBoxF box, CHyprColor color, int round = 6) { renderPass::appendOutline(output, box, color, scale, 1, round); };
        const auto  chrome      = modal() ? layout(monitor, true) : positions;
        const auto  pointer     = m_pointer - monitor->m_position;
        const auto& all         = summaries();
        const auto& listChoices = choices();
        const auto  drawButton  = [&](const SButton& value, bool overview = false) {
            const bool hover = value.box.contains(pointer.x, pointer.y);
            if (value.action == EAction::SWITCH) {
                const auto summary  = std::ranges::find(listChoices, value.id, &SChoice::id);
                const auto identity = summary == listChoices.end() ? accent : identityColor(summary->color);
                if (hover || value.focused)
                    rect(value.box, color(value.focused ? style.selected : style.hover), 6);
                icon(summary == listChoices.end() ? "grid" : summary->icon, identity, {value.box.x + 10, value.box.cy() - 10, 20, 20});
                text(value.label, {value.box.x + 42, value.box.y, value.box.w - 164, value.box.h}, ink, false, .95);
                text(value.detail, {value.box.x + value.box.w - 116, value.box.y, 86, value.box.h}, subtle, false, .72, false, true);
                if (value.active)
                    text("✓", {value.box.x + value.box.w - 24, value.box.y, 16, value.box.h}, muted, false, .8, true);
                return;
            }
            if (value.action == EAction::ICON) {
                if (value.active || hover)
                    rect(value.box, color(value.active ? style.selected : style.hover), 6);
                if (value.focused)
                    rect({value.box.cx() - 7, value.box.y + value.box.h - 3, 14, 2}, muted, 1);
                const double size = std::min(22.0, value.box.h - 8);
                icon(value.id, identityColor(m_color), {value.box.cx() - size / 2, value.box.cy() - size / 2, size, size});
                return;
            }
            if (value.action == EAction::ICON_PREV || value.action == EAction::ICON_NEXT) {
                if (hover)
                    rect(value.box, color(style.hover), 5);
                text(value.label, value.box, muted, false, .85, true);
                return;
            }
            if (value.action == EAction::COLOR) {
                if (value.active)
                    outline(value.box, muted, 14);
                if (value.focused)
                    rect({value.box.cx() - 7, value.box.y + value.box.h + 3, 14, 2}, muted, 1);
                const SBoxF swatch{value.box.x + 4, value.box.y + 4, value.box.w - 8, value.box.h - 8};
                rect(swatch, appearanceColor(value.id), 12);
                outline(swatch, color(style.border), 12);
                return;
            }
            if (value.action == EAction::SIDEBAR) {
                const auto active = std::ranges::find(all, true, &SSpaceSummary::active);
                if (hover || sidebarVisible())
                    rect(value.box, color(overviewStyle.hover), 6);
                icon(active == all.end() ? "grid" : active->icon, active == all.end() ? overviewMuted : overviewIdentity(active->color),
                     {value.box.x + 10, value.box.cy() - 10, 20, 20});
                text(value.label, {value.box.x + 42, value.box.y, value.box.w - 80, value.box.h}, overviewInk, false, 1.15);
                text("⌄", {value.box.x + value.box.w - 25, value.box.y, 16, value.box.h}, overviewMuted, false, 1.1, true);
                return;
            }
            if (m_mode == EMode::SIDEBAR && !overview) {
                if (hover || value.focused)
                    rect(value.box, color(value.focused ? style.selected : style.hover), 6);
                text(value.label, {value.box.x + 10, value.box.y, value.box.w - 20, value.box.h}, muted, false, .85);
                return;
            }
            const bool  primary      = value.action == EAction::ENTER || value.action == EAction::CONFIRM;
            const auto& buttonStyle  = overview ? overviewStyle : style;
            const auto  primaryHover = spaceStyle::mix(
                buttonStyle.primary, spaceStyle::luminance(buttonStyle.primaryText) > .5 ? spaceStyle::rgb(0xff000000) : spaceStyle::rgb(0xffffffff), .08);
            if (primary || value.active || hover || value.focused)
                rect(value.box,
                     color(primary                         ? (hover ? primaryHover : buttonStyle.primary)
                           : value.active || value.focused ? buttonStyle.selected
                                                           : buttonStyle.hover),
                     7);
            if (value.focused)
                rect({value.box.x + 10, value.box.y + value.box.h - 3, value.box.w - 20, 2}, accent, 1);
            const auto buttonInk = primary                           ? color(buttonStyle.primaryText)
                                   : overview                        ? overviewInk
                                   : value.action == EAction::CREATE ? accent
                                   : value.action == EAction::REMOVE ? muted
                                                                     : ink;
            text(value.label, {value.box.x + 10, value.box.y, value.box.w - 20, value.box.h}, buttonInk, false, 1, primary || value.action == EAction::MENU);
        };

        const auto drawSearch = [&] {
            const auto& box = positions.search;
            if (box.w <= 0 || box.h <= 0)
                return;
            rect(box, color(style.raised), 6);
            outline(box, m_focus == "query" || ((m_mode == EMode::CREATE || m_mode == EMode::RENAME) && m_focus == "icon") ? accent : color(style.border), 6);
            const SBoxF content{box.x + 10, box.y + 3, box.w - 20, box.h - 6};
            const bool  iconSearch   = m_mode == EMode::CREATE || m_mode == EMode::RENAME;
            const bool  queryFocused = m_focus == (iconSearch ? "icon" : "query");
            const bool  editing      = queryFocused && m_inputMode == EInputMode::Insert;
            if (m_query.text().empty() && !editing)
                text(iconSearch ? "/ Search icons" : "/ Search spaces", content, subtle, false, .8);
            else {
                const auto&  metrics  = inputMetrics(scale, true);
                const double offset   = std::max(0.0, metrics.x / scale - content.w + 2);
                const auto [from, to] = editing ? m_query.selection() : std::pair<size_t, size_t>{0, 0};
                if (const auto texture = textures().inputText(m_query.text(), config::overviewFont(), ink, content.w, offset, scale, from, to, accent.modifyA(.35F))) {
                    const auto size = texture->m_size / scale;
                    output.emplace_back(makeUnique<CTexPassElement>(
                        CTexPassElement::SRenderData{.tex = texture, .box = px({content.x, content.cy() - size.y / 2, size.x, size.y}), .a = 1, .clipBox = px(content)}));
                }
                if (editing)
                    rect({content.x + metrics.x / scale - offset, content.y + 3, 1.5, content.h - 6}, accent, 0);
            }
        };

        const auto active = std::ranges::find(all, true, &SSpaceSummary::active);
        if (active != all.end()) {
            const double actions = chrome.content.w >= 560 ? 292 : 104;
            const auto   subtitle =
                std::format("{} workspace{} · {} window{}", active->occupied, active->occupied == 1 ? "" : "s", active->windows, active->windows == 1 ? "" : "s");
            text(subtitle, {chrome.header.x + 42, chrome.header.y + 41, std::max(1.0, chrome.header.w - actions - 42), 22}, overviewMuted, false, .8);
        }
        rect({chrome.header.x, chrome.header.y + 72, chrome.header.w, 1}, color(spaceStyle::mix(overviewStyle.background, overviewStyle.text, .075)), 0);
        for (const auto& value : chrome.buttons)
            if (value.action != EAction::ENTER)
                drawButton(value, true);
        if (chrome.empty.w > 0) {
            const auto&  e  = chrome.empty;
            const double cx = e.cx(), top = e.y;
            icon(active == all.end() ? "grid" : active->icon, active == all.end() ? color(overviewStyle.focus) : overviewIdentity(active->color), {cx - 16, top, 32, 32});
            text("This space is empty", {e.x, top + 43, e.w, 34}, overviewInk, false, 1.25, true);
            text("Open an app to make it yours.", {e.x, top + 82, e.w, 24}, overviewMuted, false, .9, true);
            for (const auto& value : chrome.buttons)
                if (value.action == EAction::ENTER)
                    drawButton(value, true);
        }
        if (modal()) {
            if (m_mode != EMode::MENU && m_mode != EMode::SIDEBAR)
                rect({0, 0, monitor->m_size.x, monitor->m_size.y}, color(style.veil), 0);
            if (monitor != m_monitor.lock())
                return output;
            const int panelRounding = m_mode == EMode::SIDEBAR ? 12 : 14;
            rect({positions.panel.x - 3, positions.panel.y + 4, positions.panel.w + 6, positions.panel.h + 3}, color(style.shadow).modifyA(style.shadow.a * .35F),
                 panelRounding + 2);
            rect({positions.panel.x - 1, positions.panel.y + 2, positions.panel.w + 2, positions.panel.h + 2}, color(style.shadow).modifyA(style.shadow.a * .65F),
                 panelRounding + 1);
            rect(positions.panel, plate, panelRounding);
            if (m_mode != EMode::SIDEBAR)
                outline(positions.panel, color(style.border), panelRounding);
            if (m_mode == EMode::SIDEBAR) {
                drawSearch();
                text("Enter Open   / Search   Esc Back", {positions.rail.x + 16, positions.rail.y + positions.rail.h - 56, positions.rail.w - 32, 20}, subtle, false,
                     .64);
                text("Shift + J/K to reorder", {positions.rail.x + 16, positions.rail.y + positions.rail.h - 34, positions.rail.w - 32, 20}, subtle, false, .64);
                if (listChoices.empty())
                    text("No matches", {positions.search.x + 4, positions.rail.y + 50, positions.search.w - 8, 32}, subtle, false, .85);
                for (const auto& value : positions.buttons)
                    drawButton(value);
                return output;
            }
            if (m_mode == EMode::MENU) {
                for (const auto& value : positions.buttons)
                    drawButton(value);
                return output;
            }
            const char* title         = m_mode == EMode::CREATE   ? "Create a space"
                                        : m_mode == EMode::RENAME ? "Edit space"
                                        : m_mode == EMode::REMOVE ? "Remove this space"
                                        : m_mode == EMode::MOVE   ? "Move to another space"
                                                                  : "Your spaces";
            const bool  compactEditor = (m_mode == EMode::CREATE || m_mode == EMode::RENAME) && positions.panel.h < 480;
            text(title, {positions.panel.x + 24, positions.panel.y + (compactEditor ? 12 : 18), positions.panel.w - 48, compactEditor ? 28.0 : 36.0}, ink);
            drawSearch();
            if (positions.input.w > 0) {
                rect(positions.input, color(style.raised));
                outline(positions.input, m_focus == "name" ? accent : color(style.border));
                const auto content    = SBoxF{positions.input.x + 12, positions.input.y + 4, positions.input.w - 24, positions.input.h - 8};
                const auto [from, to] = m_focus == "name" && m_inputMode == EInputMode::Insert ? m_input.selection() : std::pair<size_t, size_t>{0, 0};
                const auto&  metrics  = inputMetrics(scale, true);
                const double offset   = std::max(0.0, metrics.x / scale - content.w + 2);
                const auto   texture  = textures().inputText(m_input.text(), config::overviewFont(), ink, content.w, offset, scale, from, to, accent.modifyA(.35F));
                if (texture) {
                    const auto size = texture->m_size / scale;
                    output.emplace_back(makeUnique<CTexPassElement>(
                        CTexPassElement::SRenderData{.tex = texture, .box = px({content.x, content.cy() - size.y / 2, size.x, size.y}), .a = 1, .clipBox = px(content)}));
                }
                if (m_focus == "name" && m_inputMode == EInputMode::Insert)
                    rect({content.x + metrics.x / scale - offset, content.y + 6, 1.5, content.h - 12}, accent, 0);
                const auto selected = spaces::spaceIcon(m_icon);
                text("Name", positions.details, muted, false, .82);
                auto iconLabel = positions.iconLabel;
                iconLabel.w    = std::max(1.0, iconLabel.w - 56);
                text(selected ? std::string("Icon · ") + std::string(selected->label) : "Icon", iconLabel, muted, false, .82);
                if (!positions.iconTotal)
                    text(m_inputMode == EInputMode::Insert ? "No icons match · Ctrl+U clears" : "No icons match · i edits the search", positions.iconGrid, subtle, false,
                         .8, true);
                text("Color", positions.colorLabel, muted, false, .82);
                text(m_error.empty() ? (m_inputMode == EInputMode::Insert ? "Enter / Esc finishes editing · Ctrl+Enter saves"
                                                                          : (m_focus == "icon"   ? "/ or i searches · hjkl selects · PgUp/PgDn more"
                                                                             : m_focus == "name" ? "i edits name · Tab field · Ctrl+Enter saves"
                                                                                                 : "Tab field · Ctrl+Enter saves · Esc back"))
                                     : m_error,
                     positions.hint, m_error.empty() ? subtle : color(style.error), false, .7);
            } else
                text(details(), positions.details, m_error.empty() ? muted : color(style.error), true);
        }
        if (modal())
            for (const auto& value : positions.buttons)
                drawButton(value);
        return output;
    }
} // namespace hyprspace
