"""Offline tests for the read-only MaxLua relocation inspector."""

import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "maxlua_bindings", Path(__file__).parents[1] / "tools/maxlua_bindings.py"
)
if spec is None or spec.loader is None:
    raise RuntimeError("Cannot load maxlua_bindings.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BindingTests(unittest.TestCase):
    def test_maps_preserve_paths_with_spaces_and_offsets(self):
        rows = module.parse_maps("1000-2000 r-xp 00001000 08:01 7 /tmp/Max Support/MaxLua.so\n")
        self.assertEqual(rows, [(0x1000, 0x2000, 0x1000, "/tmp/Max Support/MaxLua.so")])
        self.assertEqual(module.provider(rows, 0x1234), "/tmp/Max Support/MaxLua.so")
        self.assertEqual(module.provider(rows, 0x2000), "[unmapped]")

    def test_only_lua_dynamic_relocations_are_read(self):
        lines = (
            "00000000000cffe0 0000010100000006 R_X86_64_GLOB_DAT "
            "00000000000986f0 luaO_nilobject_ + 0\n"
            "00000000000d0130 000000bf00000007 R_X86_64_JUMP_SLOT "
            "0000000000050770 lua_pushcclosure + 0\n"
            "00000000000d0138 000000bf00000007 R_X86_64_JUMP_SLOT 0000000000050770 other + 0\n"
        )
        self.assertEqual(
            module.parse_relocations(lines),
            [(0xCFFE0, "luaO_nilobject_"), (0xD0130, "lua_pushcclosure")],
        )

    def test_invalid_pid_rejected_before_reading_proc(self):
        with self.assertRaisesRegex(ValueError, "positive"):
            module.inspect(0)


if __name__ == "__main__":
    unittest.main()
