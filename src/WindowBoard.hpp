#pragma once

#include "OverviewSession.hpp"
#include "WindowViewModel.hpp"

#include <hyprland/src/render/pass/PassElement.hpp>
#include <xkbcommon/xkbcommon.h>
#include <unordered_map>
#include <array>

namespace hyprspace {

    class CWindowBoard {
      public:
        explicit CWindowBoard(COverviewSession& owner);
        bool active() const {
            return m_active;
        }
        bool rendering() const {
            return m_openness->value() > 0.001F;
        }
        float progress() const {
            return std::clamp(m_openness->value(), 0.F, 1.F);
        }
        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }
        uint64_t generation() const {
            return m_generation;
        }
        bool searchFocused() const {
            return m_searchFocused;
        }
        EWindowGrouping grouping() const {
            return m_grouping;
        }
        bool recent() const {
            return m_recent;
        }
        const std::string& query() const {
            return m_query;
        }
        const SWindowViewLayout& layout() const {
            return m_layout;
        }
        const SBoxF& viewport() const {
            return m_viewport;
        }
        double scrollOffset() const {
            return m_scroll;
        }
        double inspectionFactor() const {
            return m_lens.current(m_lensProgress->value()).scale;
        }
        double inspectionGoal() const {
            return m_lens.goal().scale;
        }
        bool inspecting() const {
            return m_inspecting;
        }
        bool panning() const {
            return m_mousePan || m_keyboardPan;
        }

        void                           enter(PHLMONITOR monitor, std::optional<EWindowGrouping> grouping = std::nullopt);
        void                           leave();
        void                           refresh();
        void                           cycleGrouping();
        void                           setGrouping(EWindowGrouping grouping);
        bool                           reservesKey(xkb_keysym_t sym, uint32_t mods, const std::string& text) const;
        void                           onKey(xkb_keysym_t sym, uint32_t mods, const std::string& text);
        void                           pointer(const Vector2D& pos, bool userMotion);
        void                           button(uint32_t button, bool pressed, uint32_t mods, const Vector2D& pos);
        void                           scroll(const SScrollInput& event, const Vector2D& pos);
        std::optional<SOverviewTarget> selectedTarget() const;
        std::optional<SOverviewTarget> targetAt(const Vector2D& pos) const;
        std::vector<SOverviewTarget>   inspectTargets() const;
        void                           selectWindow(PHLWINDOW window);
        bool                           beginInspection();
        void                           endInspection();
        void                           zoomStep(int direction, std::optional<Vector2D> pos = std::nullopt);
        void                           panKey(const Vector2D& delta);
        bool                           panAvailable(const Vector2D& pos) const;
        bool                           beginPan(const Vector2D& pos, bool keyboard);
        void                           endPan();
        bool                           needsBlur(PHLMONITOR monitor) const;
        std::vector<UP<IPassElement>>  buildPass(PHLMONITOR monitor) const;

      private:
        struct SItem {
            SWindowViewInput input;
            PHLWINDOWREF     window;
        };
        const SItem*                   item(uint64_t key) const;
        SBoxF                          gridBox(const SWindowViewCard& card) const;
        SBoxF                          cardBox(const SWindowViewCard& card) const;
        SBoxF                          previewBox(const SItem& item, const SBoxF& card) const;
        SBoxF                          inspectionFit() const;
        std::optional<SOverviewTarget> target(const SItem& item, const SBoxF& preview, std::optional<Vector2D> point = std::nullopt) const;
        void                           rebuild();
        void                           select(uint64_t key, bool pointer = false);
        void                           ensureVisible();
        void                           updateScroll(double offset);
        void                           focusSearch();
        void                           clearSearch();
        void                           commit();
        SBoxF                          searchBox() const;
        SBoxF                          returnBox() const;
        SBoxF                          recentBox() const;
        std::array<SBoxF, 4>           groupingBoxes() const;

        COverviewSession&                    m_session;
        PHLMONITORREF                        m_monitor;
        bool                                 m_active = false, m_searchFocused = false, m_recent = false, m_dirty = true, m_inspecting = false;
        bool                                 m_mousePan = false, m_keyboardPan = false;
        uint64_t                             m_generation = 0, m_nextKey = 1, m_selected = 0, m_hovered = 0;
        EWindowGrouping                      m_grouping = EWindowGrouping::FLAT;
        std::string                          m_query;
        std::vector<SItem>                   m_items;
        std::vector<SWindowViewInput>        m_input;
        std::unordered_map<uint64_t, size_t> m_recency;
        std::unordered_map<uint64_t, SBoxF>  m_from;
        SWindowViewLayout                    m_layout;
        SBoxF                                m_usable, m_viewport;
        double                               m_scroll = 0, m_gap = 0;
        Vector2D                             m_pointer;
        CInspectionZoom                      m_lens;
        PHLANIMVAR<float>                    m_openness, m_reflow, m_inspectionMix, m_lensProgress;
    };

} // namespace hyprspace
