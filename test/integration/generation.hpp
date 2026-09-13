#pragma once

// Read by the snapshot inventory without loading either shared library.
extern "C" __attribute__((used, visibility("default"))) const char hyprspace_test_generation[] =
    "hyprspace-test-plugin-sha256:" HYPRSPACE_TEST_PLUGIN_SHA256;
