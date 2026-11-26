"""Synthetic AMXD resource indexing; no commercial device content is used."""

import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "amxd_resources", ROOT / "tools" / "amxd_resources.py"
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("AMXD resource tool could not be loaded")
tool = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = tool
SPEC.loader.exec_module(tool)


def chunk(tag: bytes, payload: bytes) -> bytes:
    return tag + struct.pack(">I", len(payload) + 8) + payload


def container(payload: bytes, *, name: bytes = b"test.lua\0", bad_offset: bool = False) -> bytes:
    header = b"ampf" + bytes(8) + b"ptch" + bytes(4) + b"mx@c"
    offset = len(header) - tool.RESOURCE_BASE
    if bad_offset:
        offset = 0xFFFFFFFF
    fields = b"".join(
        (
            chunk(b"type", b"Jlua"),
            chunk(b"fnam", name),
            chunk(b"sz32", struct.pack(">I", len(payload))),
            chunk(b"of32", struct.pack(">I", offset)),
        )
    )
    return header + payload + chunk(b"dire", fields)


class AmxdResourceTests(unittest.TestCase):
    def test_indexes_payload_without_printing_it(self) -> None:
        payload = b"return 42\n"
        data = container(payload)
        entries = tool.index(data)
        self.assertEqual(len(entries), 1)
        self.assertEqual(
            (entries[0].name, entries[0].kind, entries[0].size), ("test.lua", "Jlua", len(payload))
        )
        self.assertEqual(data[entries[0].offset : entries[0].offset + entries[0].size], payload)

    def test_rejects_unknown_layout(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported AMXD layout"):
            tool.index(b"not an amxd")

    def test_out_of_bounds_resource_is_not_reported(self) -> None:
        self.assertEqual(tool.index(container(b"safe", bad_offset=True)), [])

    def test_embedded_false_directory_marker_is_ignored(self) -> None:
        entries = tool.index(container(b"dire\x00\x00\x00\x08 not a directory"))
        self.assertEqual([entry.name for entry in entries], ["test.lua"])

    def test_cli_extracts_privately_and_never_overwrites(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            amxd = root / "fixture.amxd"
            output = root / "resource.lua"
            amxd.write_bytes(container(b"return 42\n"))
            command = [
                sys.executable,
                str(ROOT / "tools" / "amxd_resources.py"),
                str(amxd),
                "--resource",
                "test.lua",
                "--output",
                str(output),
            ]
            first = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(output.read_bytes(), b"return 42\n")
            self.assertEqual(os.stat(output).st_mode & 0o077, 0)
            second = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(second.returncode, 2)
            self.assertIn(b"AMXD_RESOURCE_ERROR", second.stderr)
            self.assertEqual(output.read_bytes(), b"return 42\n")


if __name__ == "__main__":
    unittest.main()
