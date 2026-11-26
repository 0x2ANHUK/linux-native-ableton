#!/usr/bin/env python3
"""Compare MaxLua lifecycle behaviour with and without local symbol binding.

Runs two disposable, non-Max child processes against locally installed libraries.
Inputs must be supplied by the user; no vendor binary is copied or distributed.
The children preload inert Max stubs, call MaxLua's own max_lua_init(), then
luaL_newstate and luaL_openlibs. One child also preloads an opt-in deepbind shim
that scopes RTLD_DEEPBIND to an explicit MaxLua.so dlopen.

This shows whether foreign Lua interposition changes the outcome. It does not
prove the Max editor's load order, and it is not a container or sandbox.
Run it detached (agent-job); a child may crash and core dumps are disabled.
"""

from __future__ import annotations

import argparse
import json
import os
import resource
import subprocess
import sys
import tempfile
from pathlib import Path

CHILD = r"""
import ctypes, os, sys
root = sys.argv[1] + "/"
ctypes.CDLL(sys.argv[2], mode=os.RTLD_NOW | os.RTLD_GLOBAL)
lua = ctypes.CDLL(root + "MaxLua.so", mode=os.RTLD_LAZY | os.RTLD_LOCAL)
print("stage=loaded", flush=True)
lua.max_lua_init.restype = None
lua.max_lua_init()
print("stage=max_lua_init_ok", flush=True)
new = lua.luaL_newstate
new.restype = ctypes.c_void_p
openlibs = lua.luaL_openlibs
openlibs.argtypes = [ctypes.c_void_p]
close = lua.lua_close
close.argtypes = [ctypes.c_void_p]
state = new()
if not state:
    raise SystemExit("MaxLua returned no state")
print("stage=newstate_ok", flush=True)
openlibs(state)
print("stage=openlibs_ok", flush=True)
close(state)
print("stage=close_ok", flush=True)
"""


def run_child(resources: Path, foreign: Path, preload: list[Path], isolated: bool) -> dict:
    env = {
        name: os.environ[name] for name in ("PATH", "HOME", "LANG", "TMPDIR") if name in os.environ
    }
    env["LD_PRELOAD"] = ":".join(str(path) for path in preload)
    env["LIVE_MAXLUA_DEEPBIND"] = "1" if isolated else "0"
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False, encoding="utf-8") as script:
        script.write(CHILD)
        path = script.name
    try:
        result = subprocess.run(
            [sys.executable, path, str(resources), str(foreign)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )
        stages = result.stdout.split()
        return {
            "isolated": isolated,
            "exit_code": result.returncode,
            "stages": stages,
            "stderr_tail": result.stderr[-400:],
            "lifecycle_complete": "stage=close_ok" in stages,
        }
    finally:
        Path(path).unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", required=True, type=Path, help="local Max resources/support")
    parser.add_argument(
        "--foreign", required=True, type=Path, help="local Lua library loaded globally first"
    )
    parser.add_argument(
        "--shim", required=True, type=Path, help="locally built opt-in deepbind shim"
    )
    parser.add_argument(
        "--init-stubs", required=True, type=Path, help="locally built inert Max stubs"
    )
    args = parser.parse_args()
    required = (args.resources / "MaxLua.so", args.foreign, args.shim, args.init_stubs)
    missing = [str(path) for path in required if not path.is_file()]
    separators = [
        str(path) for path in (args.shim, args.init_stubs) if ":" in str(path) or " " in str(path)
    ]
    if missing or separators:
        print(
            json.dumps(
                {
                    "code": "ISOLATION_CHECK_INPUT",
                    "category": "validation",
                    "retryable": False,
                    "next_step": "Supply local libraries and paths without spaces or colons",
                    "fix": f"missing={missing} unusable={separators}",
                }
            ),
            file=sys.stderr,
        )
        return 2
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    base = [args.init_stubs.resolve()]
    results = [
        run_child(
            args.resources.resolve(), args.foreign.resolve(), [args.shim.resolve(), *base], True
        ),
        run_child(args.resources.resolve(), args.foreign.resolve(), base, False),
    ]
    failed = [item for item in results if item["lifecycle_complete"] is False]
    print(
        json.dumps(
            {
                "schema": "maxlua-isolation-check/v1",
                "runs": results,
                "isolation_changes_outcome": len(failed) == 1,
                "limitations": [
                    "Disposable process, not the Max editor.",
                    "Inert Max stubs, not a Max implementation.",
                    "Shows a symbol-binding mechanism, not a proven editor root cause.",
                ],
            },
            sort_keys=True,
        )
    )
    return 0 if len(failed) <= 1 else 1


if __name__ == "__main__":
    raise SystemExit(main())
