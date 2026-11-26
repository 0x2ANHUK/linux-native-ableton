#!/usr/bin/env python3
"""Read Lua relocation targets in an already-running local Max process.

This is read-only: it reads process maps and only the Lua relocation slots and
lua_class pointer of MaxLua.so. It does not attach a debugger, invoke functions,
or pause the process. A slot pointing inside MaxLua may still be a lazy PLT stub;
results do not prove a crash cause or that a Lua function has run.
"""

import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

MODULE_NAME = "MaxLua.so"
RELOCATION = re.compile(
    r"^([0-9a-fA-F]+)\s+\S+\s+R_X86_64_(?:GLOB_DAT|JUMP_SLOT)\s+\S+\s+(lua\w+)\s"
)
DYNAMIC_SYMBOL = re.compile(r"^([0-9a-fA-F]+)\s+\w\s+(luaO_nilobject_|lua_class)$", re.M)


def parse_maps(text: str) -> list[tuple[int, int, int, str]]:
    result = []
    for line in text.splitlines():
        fields = line.split(None, 5)
        if len(fields) < 6:
            continue
        low, high = fields[0].split("-", 1)
        result.append((int(low, 16), int(high, 16), int(fields[2], 16), fields[5]))
    return result


def parse_relocations(text: str) -> list[tuple[int, str]]:
    result = []
    for line in text.splitlines():
        match = RELOCATION.match(line)
        if match:
            result.append((int(match.group(1), 16), match.group(2)))
    return result


def provider(rows: list[tuple[int, int, int, str]], pointer: int) -> str:
    return next((path for low, high, _, path in rows if low <= pointer < high), "[unmapped]")


def inspect(pid: int) -> dict:
    if pid <= 0:
        raise ValueError("pid must be a positive process ID")
    root = Path("/proc") / str(pid)
    rows = parse_maps((root / "maps").read_text())
    candidates = {path for _, _, _, path in rows if path.endswith("/" + MODULE_NAME)}
    if len(candidates) != 1:
        raise ValueError(f"expected one mapped {MODULE_NAME}, found {len(candidates)}")
    module = candidates.pop()
    base = min(low - offset for low, _, offset, path in rows if path == module)
    reloc_text = subprocess.run(
        ["readelf", "-rW", module], check=True, capture_output=True, text=True
    ).stdout
    symbols_text = subprocess.run(
        ["nm", "-D", "--defined-only", module], check=True, capture_output=True, text=True
    ).stdout
    relocations = parse_relocations(reloc_text)
    symbols = {
        match.group(2): int(match.group(1), 16) for match in DYNAMIC_SYMBOL.finditer(symbols_text)
    }
    if not relocations or "lua_class" not in symbols or "luaO_nilobject_" not in symbols:
        raise ValueError(
            "expected Lua relocations and exported lua_class/luaO_nilobject_ were not found"
        )
    with (root / "mem").open("rb", buffering=0) as memory:

        def pointer_at(offset: int) -> int:
            data = os.pread(memory.fileno(), 8, base + offset)
            if len(data) != 8:
                raise OSError("short process-memory read")
            return struct.unpack("<Q", data)[0]

        bindings = [(name, provider(rows, pointer_at(offset))) for offset, name in relocations]
        class_initialized = pointer_at(symbols["lua_class"]) != 0
        nil_offsets = [offset for offset, name in relocations if name == "luaO_nilobject_"]
        nil_is_local = (
            pointer_at(nil_offsets[0]) == base + symbols["luaO_nilobject_"]
            if len(nil_offsets) == 1
            else None
        )
    counts = collections.Counter(path for _, path in bindings)
    return {
        "schema": "maxlua-bindings/v1",
        "pid": pid,
        "module": module,
        "lua_relocation_slots": len(bindings),
        "providers": dict(sorted(counts.items())),
        "non_module_symbols": [name for name, path in bindings if path != module],
        "lua_class_initialized": class_initialized,
        "nilobject_bound_to_module": nil_is_local,
        "limitations": [
            "Lazy PLT entries may not yet be resolved.",
            "A separate Max editor is a different process and may have different bindings.",
            "Symbol locations alone do not establish a crash cause or working Lua execution.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pid", type=int, required=True, help="running local Live or Max process ID"
    )
    parser.add_argument("--json", action="store_true", help="print the complete structured result")
    args = parser.parse_args()
    try:
        result = inspect(args.pid)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(
            json.dumps(
                {
                    "code": "MAXLUA_INSPECT_FAILED",
                    "category": "unavailable",
                    "retryable": False,
                    "next_step": "Check PID, MaxLua mapping, readelf/nm and process permissions",
                    "fix": str(exc),
                }
            ),
            file=sys.stderr,
        )
        return 2
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            f"Lua relocation slots: {result['lua_relocation_slots']}; "
            f"non-module providers: {len(result['non_module_symbols'])}"
        )
        print(f"Host lua_class initialized: {result['lua_class_initialized']}")
        print("Lazy relocations and separate Max processes are not verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
