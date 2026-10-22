#pragma once
#include "InputModel.hpp"
#include "WindowBoard.hpp"
#include <nlohmann/json.hpp>

namespace hyprspace {
    class CInputController {
      public:
        SInputContext context() const;
        void          reset();
        void          invalidateKeys() {
            m_model.reset();
        }
        uint64_t                      epoch();
        SInputCommand                 preview(xkb_keysym_t sym, uint32_t nativeMods, const std::string& text);
        SInputCommand                 onKey(xkb_keysym_t sym, uint32_t nativeMods, const std::string& text);
        bool                          perform(EInputAction action);
        nlohmann::json                inspect();
        std::vector<UP<IPassElement>> buildPass(PHLMONITOR monitor);

      private:
        struct SParent {
            bool                                        board;
            std::optional<CWindowBoard::SInputSnapshot> snapshot;
        };
        void                 back();
        void                 saveCanvas();
        CInputModel          m_model;
        std::vector<SParent> m_parents;
        uint64_t             m_generation = 0;
    };
    CInputController& inputController();
} // namespace hyprspace
