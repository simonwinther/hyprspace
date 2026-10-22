// hyprspace - custom render pass elements.
//
// The switcher returns ordinary texture and rect elements. Window previews use
// the existing texture/blur compositor, advertising their blur requirements to
// Hyprland's pass so damage and cached backgrounds remain correct.

#pragma once

#include "globals.hpp"
#include "RenderGeometry.hpp"

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <optional>
#include <vector>

namespace hyprspace {

    inline CBox physicalRenderBox(const SBoxF& box, double scale) {
        const auto pixels = renderGeometry::physicalBox(box, scale);
        return {pixels.x, pixels.y, pixels.w, pixels.h};
    }

    namespace renderPass {
        // Feature renderers supply logical coordinates only. This boundary owns
        // native shape construction and the physical-pixel geometry contract.
        void appendSolidFill(std::vector<UP<IPassElement>>& out, const SBoxF& logical, const CHyprColor& color, double scale, double radius = 0,
                             std::optional<SBoxF> clip = std::nullopt);
        void appendOutline(std::vector<UP<IPassElement>>& out, const SBoxF& logical, const CHyprColor& color, double scale, double width = 1, double radius = 0);
    } // namespace renderPass

    class CSwitcher;

    // A captured window keeps the compositor's blur policy, while the capture
    // itself remains independent of monitor framebuffers and blur resources.
    class CWindowPreviewPassElement : public CTexPassElement {
      public:
        CWindowPreviewPassElement(SRenderData data, PHLWINDOW window, bool stableSize = true);

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
        bool         m_stableSize = true;
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
