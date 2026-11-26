#!/usr/bin/env python3
"""Inventory Max's Jitter catalog against named local files, without loading Max.

Input: an installed Max resources directory. Output: candidate file matches;
no file match is NOT proof an object is unavailable (it may be built in, mapped,
or registered dynamically). Excludes node_modules and does not follow symlinks.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

CATALOG = re.compile(r'^max oblist\s+"([^\"]+)"\s+([^;\s]+);')
MAPPING = re.compile(r"^max objectfile\s+(\S+)\s+(\S+);")
SUFFIXES = (".mxl_x86_64", ".mxl_aarch64", ".maxpat", ".mxf")
SEARCH_ROOTS = ("externals", "extensions", "packages")


def scan(resources: Path) -> dict:
    catalog_path = resources / "init" / "jitter-objectlist.txt"
    mapping_path = resources / "init" / "jitter-objectmappings.txt"
    entries = [
        match.groups()
        for line in catalog_path.read_text().splitlines()
        if (match := CATALOG.match(line))
    ]
    if not entries:
        raise ValueError(
            f"No catalog entries in {catalog_path}; verify this is a Max resources directory"
        )
    mappings: dict[str, str] = {}
    for line in mapping_path.read_text().splitlines():
        match = MAPPING.match(line)
        if match:
            mappings[match.group(1)] = match.group(2)
    files: dict[str, list[str]] = {}
    for root in SEARCH_ROOTS:
        directory = resources / root
        if not directory.is_dir():
            raise ValueError(f"Missing search directory {directory}; verify the Max installation")
        for base, dirs, names in os.walk(directory, followlinks=False):
            dirs[:] = sorted(
                d for d in dirs if d != "node_modules" and not (Path(base) / d).is_symlink()
            )
            for name in sorted(names):
                for suffix in SUFFIXES:
                    if name.endswith(suffix):
                        key = name[: -len(suffix)]
                        files.setdefault(key, []).append(
                            str((Path(base) / name).relative_to(resources))
                        )
                        break
    objects = []
    for category, name in sorted(set(entries), key=lambda item: (item[1], item[0])):
        target = mappings.get(name, name)
        objects.append(
            {
                "name": name,
                "category": category,
                "mapped_to": target if target != name else None,
                "named_files": files.get(target, []),
                "runtime_status": "unverified",
            }
        )
    return {
        "schema": "jit-inventory/v1",
        "resources": str(resources),
        "search_roots": list(SEARCH_ROOTS),
        "exclusions": ["node_modules", "symlink directories"],
        "objects": objects,
        "summary": {
            "catalog_rows": len(objects),
            "no_named_file": sum(not item["named_files"] for item in objects),
        },
        "limitation": "No named file does not establish that a Max object is missing or broken.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", required=True, type=Path, help="Max resources directory")
    parser.add_argument(
        "--json", action="store_true", help="Print the complete structured inventory"
    )
    args = parser.parse_args()
    try:
        result = scan(args.resources.resolve())
    except (OSError, ValueError) as exc:
        print(
            json.dumps(
                {
                    "code": "JIT_INVENTORY_INPUT",
                    "category": "validation",
                    "retryable": False,
                    "next_step": "Supply the Max resources directory",
                    "fix": str(exc),
                }
            ),
            file=sys.stderr,
        )
        return 2
    if args.json:
        print(json.dumps(result, sort_keys=True, ensure_ascii=False))
    else:
        print(
            f"Catalog entries: {result['summary']['catalog_rows']}; "
            f"no named file: {result['summary']['no_named_file']}"
        )
        print(result["limitation"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
