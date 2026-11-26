"""The file inventory never claims that an unobserved Max object is missing."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "jitter_inventory", Path(__file__).parents[1] / "tools/jitter_inventory.py"
)
if spec is None or spec.loader is None:
    raise RuntimeError("Could not load jitter_inventory.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
scan = module.scan


class InventoryTest(unittest.TestCase):
    def test_exact_mapped_and_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for path in ("init", "externals", "extensions", "packages"):
                (root / path).mkdir()
            (root / "init/jitter-objectlist.txt").write_text(
                'max oblist "Jitter Test" jit.present;\n'
                'max oblist "Jitter Test" jit.alias;\n'
                'max oblist "Jitter Test" jit.unknown;\n'
            )
            (root / "init/jitter-objectmappings.txt").write_text(
                "max objectfile jit.alias jit.present;\n"
            )
            (root / "externals/jit.present.mxl_x86_64").touch()
            result = scan(root)
            objects = {item["name"]: item for item in result["objects"]}
            self.assertEqual(objects["jit.alias"]["mapped_to"], "jit.present")
            self.assertEqual(
                objects["jit.alias"]["named_files"],
                ["externals/jit.present.mxl_x86_64"],
            )
            self.assertEqual(objects["jit.unknown"]["named_files"], [])
            self.assertEqual(objects["jit.unknown"]["runtime_status"], "unverified")
            self.assertEqual(result["summary"]["no_named_file"], 1)

    def test_empty_catalog_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "init").mkdir()
            (root / "init/jitter-objectlist.txt").write_text("")
            with self.assertRaisesRegex(ValueError, "No catalog entries"):
                scan(root)


if __name__ == "__main__":
    unittest.main()
