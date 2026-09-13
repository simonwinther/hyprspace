#pragma once

namespace hyprspace {
    // Updated before lock teardown, including the lock event that precedes
    // Hyprland's protocol flag. Cleanup is always allowed.
    bool overlaysAllowed();
} // namespace hyprspace
