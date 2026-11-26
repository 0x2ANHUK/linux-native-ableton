"""Linux loader isolation against synthetic libraries; never runs vendor Lua."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "jitlua/probe/maxlua_deepbind.c"


@unittest.skipUnless(
    sys.platform.startswith("linux") and shutil.which("cc"), "Linux C compiler required"
)
class DeepbindTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        (cls.root / "foreign.c").write_text("int luaopen_probe(void) { return 2; }\n")
        (cls.root / "target.c").write_text(
            "int luaopen_probe(void) { return 1; }\n"
            "int (*probe_slot)(void) = luaopen_probe;\n"
            "int probe_result(void) { return probe_slot(); }\n"
        )
        cls.build(cls.root / "foreign.c", cls.root / "foreign.so")
        cls.build(cls.root / "target.c", cls.root / "MaxLua.so")
        cls.build(cls.root / "target.c", cls.root / "Other.so")
        cls.build(PROBE, cls.root / "deepbind.so", "-ldl")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def build(cls, source, output, *extra):
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-shared",
                "-fPIC",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(output),
                *extra,
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def binding(self, enabled, target="MaxLua.so"):
        script = (
            "import ctypes,os,sys; "
            "foreign=ctypes.CDLL(sys.argv[1],mode=os.RTLD_NOW|os.RTLD_GLOBAL); "
            "target=ctypes.CDLL(sys.argv[2],mode=os.RTLD_NOW|os.RTLD_LOCAL); "
            "print(target.probe_result())"
        )
        env = dict(os.environ, LD_PRELOAD=str(self.root / "deepbind.so"))
        if enabled is None:
            env.pop("LIVE_MAXLUA_DEEPBIND", None)
        else:
            env["LIVE_MAXLUA_DEEPBIND"] = enabled
        result = subprocess.run(
            [sys.executable, "-c", script, str(self.root / "foreign.so"), str(self.root / target)],
            env=env,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def test_default_and_nonexact_values_leave_preemption_unchanged(self):
        self.assertEqual(self.binding(None), "2")
        self.assertEqual(self.binding("0"), "2")
        self.assertEqual(self.binding("true"), "2")

    def test_exact_opt_in_isolates_only_maxlua_basename(self):
        self.assertEqual(self.binding("1"), "1")
        self.assertEqual(self.binding("1", "Other.so"), "2")

    def test_cannot_rebind_a_module_loaded_before_opt_in(self):
        script = (
            "import ctypes,os,sys; "
            "foreign=ctypes.CDLL(sys.argv[1],mode=os.RTLD_NOW|os.RTLD_GLOBAL); "
            "first=ctypes.CDLL(sys.argv[2],mode=os.RTLD_NOW|os.RTLD_LOCAL); "
            "os.environ['LIVE_MAXLUA_DEEPBIND']='1'; "
            "second=ctypes.CDLL(sys.argv[2],mode=os.RTLD_NOW|os.RTLD_LOCAL); "
            "print(first.probe_result(),second.probe_result(),first._handle==second._handle)"
        )
        env = dict(os.environ, LD_PRELOAD=str(self.root / "deepbind.so"))
        env.pop("LIVE_MAXLUA_DEEPBIND", None)
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                script,
                str(self.root / "foreign.so"),
                str(self.root / "MaxLua.so"),
            ],
            env=env,
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.stdout.strip(), "2 2 True")


if __name__ == "__main__":
    unittest.main()
