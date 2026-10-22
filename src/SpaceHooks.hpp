#pragma once

#include "globals.hpp"

#include <functional>
#include <string>

namespace hyprspace::spacehooks {
    // These hooks are pinned to the same native ABI as the interactive hooks.
    void install();
    void uninstall();

    SDispatchResult dispatch(const std::string& name, std::string args, const std::function<SDispatchResult(std::string)>& original);
} // namespace hyprspace::spacehooks
