#!/usr/bin/env python3
"""Index or extract embedded resources from the observed Max AMXD container.

Only the observed ``ampf/ptch/mx@c`` layout is accepted. Directory records are
validated structurally, but this is not a complete published AMXD specification.
No device content is printed; extraction requires a new explicit output file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

MAX_CONTAINER_BYTES = 256 * 1024 * 1024
MAX_DIRECTORY_MARKERS = 65536
MAX_DIRECTORY_ENTRY_BYTES = 4096
RESOURCE_BASE = 20


@dataclass(frozen=True)
class Resource:
    name: str
    kind: str
    offset: int
    size: int
    sha256: str


def _u32(data: bytes) -> int:
    if len(data) != 4:
        raise ValueError("resource integer is not four bytes")
    return struct.unpack(">I", data)[0]


def _fields(data: bytes, start: int, end: int) -> dict[bytes, bytes]:
    fields: dict[bytes, bytes] = {}
    position = start
    while position < end:
        if position + 8 > end:
            raise ValueError("truncated directory field")
        tag = data[position : position + 4]
        size = _u32(data[position + 4 : position + 8])
        if size < 8 or position + size > end or tag in fields:
            raise ValueError("invalid or duplicate directory field")
        fields[tag] = data[position + 8 : position + size]
        position += size
    return fields


def index(data: bytes) -> list[Resource]:
    if len(data) > MAX_CONTAINER_BYTES:
        raise ValueError("container exceeds the supported size limit")
    if (
        data[:4] != b"ampf"
        or data[12:16] != b"ptch"
        or data[RESOURCE_BASE : RESOURCE_BASE + 4] != b"mx@c"
    ):
        raise ValueError("unsupported AMXD layout; expected ampf/ptch/mx@c")
    records: list[Resource] = []
    position = RESOURCE_BASE + 4
    markers = 0
    while True:
        start = data.find(b"dire", position)
        if start < 0:
            break
        markers += 1
        if markers > MAX_DIRECTORY_MARKERS:
            raise ValueError("too many potential resource directory markers")
        position = start + 4
        if start + 8 > len(data):
            continue
        size = _u32(data[start + 4 : start + 8])
        if size < 8 or size > MAX_DIRECTORY_ENTRY_BYTES or start + size > len(data):
            continue
        try:
            fields = _fields(data, start + 8, start + size)
            name = fields[b"fnam"].rstrip(b"\0").decode("utf-8")
            kind = fields[b"type"].rstrip(b"\0").decode("ascii")
            offset = RESOURCE_BASE + _u32(fields[b"of32"])
            length = _u32(fields[b"sz32"])
        except (KeyError, UnicodeError, ValueError):
            continue
        if not name or not kind or offset < RESOURCE_BASE or offset + length > start:
            continue
        payload = data[offset : offset + length]
        records.append(Resource(name, kind, offset, length, hashlib.sha256(payload).hexdigest()))
        position = start + size
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("amxd", type=Path)
    parser.add_argument("--resource", help="exact resource name to extract")
    parser.add_argument("--output", type=Path, help="new output file; never overwritten")
    args = parser.parse_args()
    try:
        with args.amxd.open("rb") as stream:
            data = stream.read(MAX_CONTAINER_BYTES + 1)
        if len(data) > MAX_CONTAINER_BYTES:
            raise ValueError("container exceeds the supported size limit")
        records = index(data)
        if bool(args.resource) != bool(args.output):
            raise ValueError("--resource and --output must be provided together")
        if args.resource:
            matches = [entry for entry in records if entry.name == args.resource]
            if len(matches) != 1:
                raise ValueError(f"resource name must match exactly once; found {len(matches)}")
            entry = matches[0]
            assert args.output is not None
            fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                with os.fdopen(fd, "wb") as stream:
                    stream.write(data[entry.offset : entry.offset + entry.size])
            except OSError:
                args.output.unlink(missing_ok=True)
                raise
            print(json.dumps({"resource": entry.name, "size": entry.size, "sha256": entry.sha256}))
        else:
            print(json.dumps({"resources": [asdict(entry) for entry in records]}, sort_keys=True))
        return 0
    except (OSError, ValueError) as exc:
        print(
            json.dumps(
                {
                    "code": "AMXD_RESOURCE_ERROR",
                    "category": "validation",
                    "retryable": False,
                    "next_step": "Check the local device, layout and exact resource name.",
                    "fix": str(exc),
                }
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
