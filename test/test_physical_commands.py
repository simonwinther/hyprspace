#!/usr/bin/env python3
"""Check physical harness Lua commands without executing any desktop command."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / "integration"))
from physical import lua_request, option_value


class PhysicalCommandTests(unittest.TestCase):
    def test_native_boolean_and_legacy_integer_options_restore_consistently(self):
        self.assertEqual(option_value({"bool": True}), 1)
        self.assertEqual(option_value({"bool": False}), 0)
        self.assertEqual(option_value({"int": 2}), 2)

    def test_focus_and_move_use_native_selectors_and_silent_follow(self):
        _, expression = lua_request(("dispatch", "movetoworkspacesilent", "90002,address:0x123"))
        self.assertIn('["workspace"] = "90002"', expression)
        self.assertIn('["window"] = "address:0x123"', expression)
        self.assertIn('["follow"] = false', expression)
        self.assertTrue(lua_request(("dispatch", "focusmonitor", "DP-1"))[1].startswith("hl.dsp.focus("))

    def test_geometry_and_float_have_explicit_target_and_mode(self):
        _, expression = lua_request(("dispatch", "resizewindowpixel", "exact 320 240,address:0x123"))
        self.assertIn("hl.dsp.window.resize", expression)
        self.assertIn('["x"] = 320.0', expression)
        self.assertIn('["window"] = "address:0x123"', expression)
        self.assertIn('["action"] = "on"', lua_request(("dispatch", "setfloating", "address:0x123"))[1])

    def test_workspace_rules_config_and_plugin_calls_use_eval(self):
        request = lua_request(("keyword", "workspace", "90001,monitor:DP-1,persistent:true,layout:scrolling"))
        self.assertEqual(request[0], "eval")
        self.assertIn("hl.workspace_rule", request[1])
        self.assertIn('["persistent"] = true', request[1])
        self.assertIn('["layout_opts"]', lua_request(("keyword", "workspace", "90001,layoutopt:direction:right"))[1])
        self.assertIn('["enabled"] = false', lua_request(("keyword", "animations:enabled", "0"))[1])
        self.assertEqual(lua_request(("dispatch", "hyprspace:overview", "on")), ("eval", 'hl.plugin.hyprspace.overview("on")'))

    def test_unknown_legacy_dispatcher_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Lua translation"):
            lua_request(("dispatch", "unmapped-command"))


if __name__ == "__main__":
    unittest.main()
