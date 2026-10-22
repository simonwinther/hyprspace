#pragma once

#include "Geometry.hpp"
#include "SearchInput.hpp"
#include "Raster.hpp"
#include "Spaces.hpp"
#include "SpaceStyle.hpp"
#include "SpaceIconPicker.hpp"
#include "InputModel.hpp"

#include <array>
#include <optional>

#include <hyprland/src/render/pass/PassElement.hpp>
#include <xkbcommon/xkbcommon.h>

namespace hyprspace {
    class CSpaceSelector {
      public:
        bool       modal() const;
        void       toggleSidebar(PHLMONITOR monitor = nullptr);
        bool       sidebarVisible() const;
        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }
        void          openManager(PHLMONITOR monitor = nullptr);
        SInputContext inputContext() const;
        bool          perform(EInputAction action);
        void          setInputMode(EInputMode mode);
        void          editInput(ETextAction action, bool extend = false);
        void          insertText(const std::string& text);
        uint64_t      generation() const {
            return m_generation;
        }
        // One geometry contract for the space chrome, workspace grid and board.
        SBoxF  contentBounds(PHLMONITOR monitor) const;
        SBoxF  workspaceButtonBounds(PHLMONITOR monitor) const;
        bool   emptySpace() const;
        double toolbarHeight() const {
            return 0;
        } // retained for status compatibility
        void                          pointer(const Vector2D& point);
        bool                          button(uint32_t button, bool pressed, uint32_t mods, const Vector2D& point);
        void                          refresh();
        void                          close();
        nlohmann::json                inspect() const;
        std::vector<UP<IPassElement>> buildPass(PHLMONITOR monitor) const;

      private:
        enum class EMode { NONE, SIDEBAR, MENU, PICK, CREATE, RENAME, REMOVE, MOVE };
        enum class EAction {
            PREV,
            NEXT,
            SWITCH,
            SIDEBAR,
            MENU,
            PICK,
            CREATE,
            RENAME,
            REMOVE,
            MOVE,
            ENTER,
            ICON,
            ICON_PREV,
            ICON_NEXT,
            COLOR,
            DESTINATION,
            WINDOW,
            WORKSPACE,
            CONFIRM,
            CANCEL
        };
        enum class EEditField { NAME, ICON, COLOR };
        struct SButton {
            SBoxF       box;
            EAction     action;
            std::string id, label;
            bool        active = false;
            std::string detail;
            bool        focused = false;
        };
        struct SLayout {
            SBoxF                rail, header, content, empty, panel, input, search, details, iconGrid, iconLabel, colorLabel, hint;
            size_t               iconColumns = 6, iconRows = 1, iconFirst = 0, iconTotal = 0;
            std::vector<SButton> buttons;
        };
        struct SSpaceSummary {
            std::string id, name, icon, color;
            size_t      occupied = 0, windows = 0;
            bool        active = false;
        };
        struct SStyleCache {
            std::array<double, 16> colors;
            std::string            mode;
            spaceStyle::SPalette   controls, overview;
        };
        struct SChoice {
            std::string id, name, icon, color, detail;
            bool        current = false;
        };
        struct SFrame {
            EMode           mode;
            PHLMONITORREF   monitor;
            PHLWINDOWREF    window;
            PHLWORKSPACEREF workspace;
            std::string     source, destination, error, icon, color, focus, iconSearchOrigin;
            CSearchInput    input, query;
            EEditField      editField;
            EInputMode      inputMode;
            bool            moveWindow;
            int             menuIndex;
        };
        const std::vector<SSpaceSummary>&      summaries() const;
        const std::vector<SChoice>&            choices() const;
        std::string                            selectedSpace() const;
        PHLMONITOR                             inputMonitor(PHLMONITOR monitor = nullptr) const;
        SFrame                                 frame() const;
        void                                   restore(SFrame frame);
        void                                   back();
        void                                   focus(std::string field);
        void                                   tabFocus(int delta);
        void                                   ensureChoice();
        CSearchInput*                          textInput();
        SLayout                                layout(PHLMONITOR monitor, bool chromeOnly = false) const;
        void                                   open(EMode mode, PHLMONITOR monitor);
        void                                   activate(const SButton& button, PHLMONITOR monitor);
        void                                   submit();
        std::vector<spaces::SSpaceInfo>        destinations() const;
        std::string                            details() const;
        const STextCursor&                     inputMetrics(double scale, bool query = false) const;
        bool                                   hasWindow() const;
        bool                                   hasWorkspace() const;
        void                                   setMoveWindow(bool selectedWindow);
        void                                   selectDestination(int delta);
        void                                   chooseAppearance(int delta);
        std::vector<const spaces::SSpaceIcon*> iconChoices() const;

        EMode                              m_mode = EMode::NONE;
        PHLMONITORREF                      m_monitor;
        PHLWINDOWREF                       m_window;
        PHLWORKSPACEREF                    m_workspace;
        std::string                        m_source, m_destination, m_error;
        bool                               m_moveWindow = false;
        int                                m_menuIndex  = 0;
        EEditField                         m_editField  = EEditField::NAME;
        std::string                        m_icon = "grid", m_color = "#89b4fa", m_iconSearchOrigin = "grid";
        CSearchInput                       m_input;
        CSearchInput                       m_query;
        EInputMode                         m_inputMode = EInputMode::Normal;
        std::string                        m_focus     = "list";
        std::vector<SFrame>                m_parents;
        Vector2D                           m_pointer;
        uint64_t                           m_generation = 0;
        mutable std::string                m_measuredText, m_measuredFont;
        mutable size_t                     m_measuredCursor = static_cast<size_t>(-1);
        mutable double                     m_measuredScale  = 0;
        mutable STextCursor                m_inputMetrics;
        mutable nlohmann::json             m_removePreview;
        mutable uint64_t                   m_previewRevision = static_cast<uint64_t>(-1);
        mutable std::string                m_previewSource, m_previewDestination;
        mutable uint64_t                   m_summaryRevision = static_cast<uint64_t>(-1);
        mutable std::vector<SSpaceSummary> m_summaries;
        mutable std::optional<SStyleCache> m_styleCache;
        mutable std::vector<SChoice>       m_choices;
        mutable uint64_t                   m_choicesRevision = static_cast<uint64_t>(-1);
        mutable EMode                      m_choicesMode     = EMode::NONE;
        mutable std::string                m_choicesQuery, m_choicesSource;
    };

    CSpaceSelector& spaceSelector();
} // namespace hyprspace
