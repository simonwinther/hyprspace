// hyprspace - controlled access to protected Hyprland renderer methods.
//
// IHyprRenderer::renderWindow() is `protected`, but it is the only sane way to
// rasterise an arbitrary window (including ones on hidden workspaces) into an
// offscreen framebuffer.
//
// Rather than the widespread `#define private public` hack — which is UB and,
// as of GCC 16 / libstdc++, actually fails to compile because <sstream>
// redeclares a member with different access — this uses the standard-blessed
// explicit-instantiation idiom: [temp.spec]/6 states that the usual access
// checking rules do not apply to names used in explicit instantiation
// arguments. No ODR or ABI games, no macro contamination.

#pragma once

#include <hyprland/src/render/Renderer.hpp>

namespace hyprspace::hidden {

    using PFnRenderWindow = void (Render::IHyprRenderer::*)(PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);

    PFnRenderWindow renderWindowPtr();

    template <PFnRenderWindow M> struct CThief {
        friend PFnRenderWindow renderWindowPtr() {
            return M;
        }
    };

    template struct CThief<&Render::IHyprRenderer::renderWindow>;

    using PFnShouldBlurWindow = bool (Render::IHyprRenderer::*)(PHLWINDOW);

    PFnShouldBlurWindow shouldBlurWindowPtr();

    template <PFnShouldBlurWindow M> struct CBlurThief {
        friend PFnShouldBlurWindow shouldBlurWindowPtr() {
            return M;
        }
    };

    template struct CBlurThief<&Render::IHyprRenderer::shouldBlur>;

    inline bool shouldBlurWindow(PHLWINDOW window) {
        return (g_pHyprRenderer.get()->*shouldBlurWindowPtr())(window);
    }

    // Render `window` through Hyprland's own window renderer into whatever
    // framebuffer is currently bound.
    //
    // standalone=true  -> full alpha, no decorations, no blur, renders even when
    //                     the window sits on a hidden workspace.
    // ignorePosition=true -> draws at the monitor origin instead of the window's
    //                     real desktop position.
    inline void renderWindowStandalone(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now) {
        (g_pHyprRenderer.get()->*renderWindowPtr())(window, monitor, now, /* decorate */ false, Render::RENDER_PASS_MAIN, /* ignorePosition */ true,
                                                    /* standalone */ true);
    }

} // namespace hyprspace::hidden
