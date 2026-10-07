"""The seed loader retains runtime observation metadata."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.__main__ import _load_seed_functions  # noqa: E402


class ObservedSeedTest(unittest.TestCase):
    def test_loader_marks_observed_seeds(self):
        seeds = [
            {"start": "0x001CF6AC", "observed": True, "note": "x"},
            # Written by seed_from_log before the field existed.
            {"start": "0x00015C5A", "note": "PsCreateSystemThreadEx "
             "StartContext1 observed at runtime; decodes as a function body."},
            {"start": "0x00202C2E", "note": "RTTI vtable slot"},
            0x00010000,
        ]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "seeds.json")
            with open(path, "w") as f:
                json.dump(seeds, f)
            observed = set()
            addrs = _load_seed_functions(path, observed)
        self.assertEqual(sorted(addrs),
                         [0x00010000, 0x00015C5A, 0x001CF6AC, 0x00202C2E])
        self.assertEqual(observed, {0x001CF6AC, 0x00015C5A})


if __name__ == "__main__":
    unittest.main()
