// hyprspace - custom render pass elements.
//
// Both overlays hook Hyprland's render pass as EK_CUSTOM elements. Hyprland
// calls draw(), we return a list of ordinary pass elements (textures, rects),
// and the element renderer draws them with the correct damage region. No raw
// GL calls, so this stays backend-agnostic.

#pragma once

#include "globals.hpp"

#include <hyprland/src/render/pass/PassElement.hpp>

namespace hyprspace {

    class COverview;
    class CSwitcher;

    class COverviewPassElement : public IPassElement {
      public:
        explicit COverviewPassElement(COverview* overview) : m_overview(overview) {}
        virtual ~COverviewPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override;
        virtual bool                          needsLiveBlur() override;
        virtual bool                          needsPrecomputeBlur() override;
        virtual std::optional<CBox>           boundingBox() override;

        virtual const char*                   passName() override {
            return "hyprspace::COverviewPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        COverview* m_overview = nullptr;
    };

    class CSwitcherPassElement : public IPassElement {
      public:
        explicit CSwitcherPassElement(CSwitcher* switcher) : m_switcher(switcher) {}
        virtual ~CSwitcherPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override;
        virtual bool                          needsLiveBlur() override;
        virtual bool                          needsPrecomputeBlur() override;
        virtual std::optional<CBox>           boundingBox() override;

        virtual const char*                   passName() override {
            return "hyprspace::CSwitcherPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        CSwitcher* m_switcher = nullptr;
    };

} // namespace hyprspace
