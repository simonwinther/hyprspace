// hyprspace - custom render pass elements.
//
// The switcher returns ordinary texture and rect elements. Window previews use
// the existing texture/blur compositor, advertising their blur requirements to
// Hyprland's pass so damage and cached backgrounds remain correct.

#pragma once

#include "globals.hpp"

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

namespace hyprspace {

    class CSwitcher;

    // A captured window keeps the compositor's blur policy, while the capture
    // itself remains independent of monitor framebuffers and blur resources.
    class CWindowPreviewPassElement : public CTexPassElement {
      public:
        CWindowPreviewPassElement(SRenderData data, PHLWINDOW window);

        std::vector<UP<IPassElement>> draw() override;
        bool                          needsLiveBlur() override;
        bool                          needsPrecomputeBlur() override;

        const char* passName() override {
            return "hyprspace::CWindowPreviewPassElement";
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        PHLWINDOWREF m_window;
    };

    class CSwitcherPassElement : public IPassElement {
      public:
        explicit CSwitcherPassElement(CSwitcher* switcher) : m_switcher(switcher) {}
        virtual ~CSwitcherPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override;
        virtual bool                          needsLiveBlur() override;
        virtual bool                          needsPrecomputeBlur() override;
        virtual std::optional<CBox>           boundingBox() override;

        virtual const char* passName() override {
            return "hyprspace::CSwitcherPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        CSwitcher* m_switcher = nullptr;
    };

} // namespace hyprspace
