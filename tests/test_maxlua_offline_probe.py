"""The offline Lua probe rejects absent inputs without starting a child."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools/maxlua_offline_probe.py"


class OfflineProbeTests(unittest.TestCase):
    def test_missing_inputs_return_structured_problem(self):
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--resources",
                "/not/a/max/resources",
                "--foreign",
                "/not/a/lua.so",
                "--shim",
                "/not/a/shim.so",
                "--host-stubs",
                "/not/a/host.so",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        problem = json.loads(result.stderr)
        self.assertEqual(problem["code"], "OFFLINE_PROBE_INPUT")
        self.assertFalse(problem["retryable"])
        self.assertEqual(result.stdout, "")

    def test_child_refuses_to_run_without_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("MaxLua.so", "JitterAPI.so", "foreign.so", "shim.so", "host.so"):
                (root / name).touch()
            env = dict(os.environ)
            env.pop("LD_PRELOAD", None)
            env.pop("LIVE_MAXLUA_DEEPBIND", None)
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--child",
                    "--resources",
                    str(root),
                    "--foreign",
                    str(root / "foreign.so"),
                    "--shim",
                    str(root / "shim.so"),
                    "--host-stubs",
                    str(root / "host.so"),
                ],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("CHILD_REQUIRES_PRELOADED_ISOLATION", result.stderr)

    def test_preload_path_with_separator_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("MaxLua.so", "JitterAPI.so", "foreign.so", "bad shim.so", "host.so"):
                (root / name).touch()
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--resources",
                    str(root),
                    "--foreign",
                    str(root / "foreign.so"),
                    "--shim",
                    str(root / "bad shim.so"),
                    "--host-stubs",
                    str(root / "host.so"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(json.loads(result.stderr)["code"], "OFFLINE_PROBE_INPUT")


class SyntheticOfflineLifecycleTests(unittest.TestCase):
    """Exercise isolation and preflight using invented libraries, never vendor code."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        sources = {
            "MaxLua": (
                "void *lua_class; int luaO_nilobject_; static int state;\n"
                "int luaopen_base(void) { return 1; }\n"
                "void *nil_reference(void) { return &luaO_nilobject_; }\n"
                "void *open_reference(void) { return (void *)luaopen_base; }\n"
                "void *luaL_newstate(void) { return &state; }\n"
                "void luaL_openlibs(void *p) { if (p != &state) __builtin_trap(); }\n"
                "int luaL_loadstring(void *p, const char *s) { (void)p; (void)s; return 0; }\n"
                "int lua_pcall(void *p, int a, int b, int c) { "
                "(void)p; (void)a; (void)b; (void)c; return 0; }\n"
                "double lua_tonumber(void *p, int n) { (void)p; (void)n; return 4.0; }\n"
                "void lua_close(void *p) { if (p != &state) __builtin_trap(); }\n"
            ),
            "JitterAPI": "int fake_jitter(void) { return 0; }\n",
            "foreign": "int luaopen_base(void) { return 2; }\n",
            "host": "int fake_host(void) { return 0; }\n",
        }
        for name, source in sources.items():
            source_path = cls.root / f"{name}.c"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-fPIC",
                    "-shared",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source_path),
                    "-o",
                    str(cls.root / f"{name}.so"),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
        root = SCRIPT.parents[1]
        for name, source in (
            ("shim", root / "jitlua/probe/maxlua_deepbind.c"),
            ("noop", root / "tests/maxlua_noop_loader.c"),
        ):
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-fPIC",
                    "-shared",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source),
                    "-ldl",
                    "-o",
                    str(cls.root / f"{name}.so"),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, shim):
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--resources",
                str(self.root),
                "--foreign",
                str(self.root / "foreign.so"),
                "--shim",
                str(self.root / f"{shim}.so"),
                "--host-stubs",
                str(self.root / "host.so"),
            ],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_fake_lifecycle_passes_only_in_isolated_child(self):
        result = self.run_probe("shim")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertIn("stage=observed_slots_local", data["stages"])
        self.assertIn("stage=script_ok", data["stages"])
        self.assertIn("stage=close_ok", data["stages"])

    def test_foreign_binding_fails_before_lua_execution(self):
        result = self.run_probe("noop")
        self.assertEqual(result.returncode, 1, result.stdout)
        problem = json.loads(result.stderr)
        self.assertEqual(problem["code"], "OFFLINE_PROBE_CHILD_FAILED")
        self.assertIn("nonlocal Lua bindings", problem["fix"])
        self.assertNotIn("stage=before_newstate", problem["stages"])


if __name__ == "__main__":
    unittest.main()
