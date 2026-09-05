#pragma once

#include "OverviewSession.hpp"
#include <functional>

namespace hyprspace::launch {
    void install();
    void clear();
    void uninstall();
    void duringCommand(const std::optional<SOverviewTarget>& target, const std::function<void()>& action);
} // namespace hyprspace::launch
