#!/usr/bin/env python3
"""Test vendor MaxLua initialization in a disposable, non-Max process only.

Inputs are locally supplied files; no vendor binaries are copied or distributed.
The child preloads an opt-in deepbind shim and linker-only Max stubs, loads a
foreign Lua globally, then initializes MaxLua and runs a fixed standard-library
script. It never attaches to Live or Max. It does not test Jitter or devices.
Run the CLI as a detached agent job; its child can crash or hang.
"""

import argparse
import ctypes
import json
import os
import resource
import subprocess
import sys
from pathlib import Path

from maxlua_bindings import inspect as inspect_bindings


def child(resources: Path, foreign: Path) -> int:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    print("stage=load_foreign", flush=True)
    foreign_library = ctypes.CDLL(str(foreign), mode=os.RTLD_NOW | os.RTLD_GLOBAL)
    print("stage=load_maxlua", flush=True)
    lua = ctypes.CDLL(str(resources / "MaxLua.so"), mode=os.RTLD_LAZY | os.RTLD_LOCAL)
    jitter = ctypes.CDLL(str(resources / "JitterAPI.so"), mode=os.RTLD_LAZY | os.RTLD_LOCAL)
    if not foreign_library or not jitter:
        raise RuntimeError("required library handle missing")
    bindings = inspect_bindings(os.getpid())
    if bindings["non_module_symbols"] or bindings["nilobject_bound_to_module"] is not True:
        raise RuntimeError("MaxLua relocation preflight found nonlocal Lua bindings")
    print("stage=observed_slots_local", flush=True)
    new_state = lua.luaL_newstate
    new_state.restype = ctypes.c_void_p
    open_libraries = lua.luaL_openlibs
    open_libraries.argtypes = [ctypes.c_void_p]
    close = lua.lua_close
    close.argtypes = [ctypes.c_void_p]
    load = lua.luaL_loadstring
    load.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    load.restype = ctypes.c_int
    call = lua.lua_pcall
    call.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    call.restype = ctypes.c_int
    number = lua.lua_tonumber
    number.argtypes = [ctypes.c_void_p, ctypes.c_int]
    number.restype = ctypes.c_double
    print("stage=before_newstate", flush=True)
    state = new_state()
    if not state:
        raise RuntimeError("luaL_newstate returned null")
    try:
        print("stage=before_openlibs", flush=True)
        open_libraries(state)
        print("stage=openlibs_ok", flush=True)
        if load(state, b"return math.floor(4.9)") != 0 or call(state, 0, 1, 0) != 0:
            raise RuntimeError("fixed Lua math script failed")
        if number(state, -1) != 4.0:
            raise RuntimeError("fixed Lua math script returned an unexpected value")
        print("stage=script_ok", flush=True)
    finally:
        close(state)
        print("stage=close_ok", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--resources", required=True, type=Path, help="local Max resources/support directory"
    )
    parser.add_argument(
        "--foreign", required=True, type=Path, help="local Lua library to load globally"
    )
    parser.add_argument("--shim", required=True, type=Path, help="locally built deepbind probe")
    parser.add_argument(
        "--host-stubs", required=True, type=Path, help="locally built offline-only stubs"
    )
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    paths = (
        args.resources / "MaxLua.so",
        args.resources / "JitterAPI.so",
        args.foreign,
        args.shim,
        args.host_stubs,
    )
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        print(
            json.dumps(
                {
                    "code": "OFFLINE_PROBE_INPUT",
                    "category": "validation",
                    "retryable": False,
                    "next_step": "Provide the local libraries and built probes",
                    "fix": f"Missing: {missing}",
                }
            ),
            file=sys.stderr,
        )
        return 2
    preload_paths = (str(args.shim.resolve()), str(args.host_stubs.resolve()))
    if any(":" in path or any(ch.isspace() for ch in path) for path in preload_paths):
        print(
            json.dumps(
                {
                    "code": "OFFLINE_PROBE_INPUT",
                    "category": "validation",
                    "retryable": False,
                    "next_step": "Use probe paths without whitespace or colons",
                    "fix": "LD_PRELOAD treats whitespace and colons as library separators",
                }
            ),
            file=sys.stderr,
        )
        return 2
    expected_preload = ":".join(preload_paths)
    if args.child:
        if (
            os.environ.get("LIVE_MAXLUA_DEEPBIND") != "1"
            or os.environ.get("LD_PRELOAD") != expected_preload
        ):
            print("OFFLINE_PROBE_CHILD_REQUIRES_PRELOADED_ISOLATION", file=sys.stderr)
            return 2
        return child(args.resources, args.foreign)
    env = {
        name: os.environ[name]
        for name in ("PATH", "HOME", "LANG", "LC_ALL", "TMPDIR")
        if name in os.environ
    }
    env["LD_PRELOAD"] = expected_preload
    env["LIVE_MAXLUA_DEEPBIND"] = "1"
    command = [
        sys.executable,
        str(Path(__file__).resolve()),
        "--child",
        "--resources",
        str(args.resources.resolve()),
        "--foreign",
        str(args.foreign.resolve()),
        "--shim",
        str(args.shim.resolve()),
        "--host-stubs",
        str(args.host_stubs.resolve()),
    ]
    result = subprocess.run(command, env=env, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(
            json.dumps(
                {
                    "code": "OFFLINE_PROBE_CHILD_FAILED",
                    "category": "execution",
                    "retryable": False,
                    "next_step": "Inspect child stages; do not rerun unchanged",
                    "fix": result.stderr[-1000:],
                    "exit_code": result.returncode,
                    "stages": result.stdout.splitlines(),
                }
            ),
            file=sys.stderr,
        )
        return 1
    print(
        json.dumps(
            {
                "schema": "maxlua-offline-probe/v1",
                "stages": result.stdout.splitlines(),
                "limitations": [
                    "No Live or Max process was used.",
                    "No Jitter API or device was exercised.",
                    "Linker-only Max stubs abort on use.",
                ],
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
