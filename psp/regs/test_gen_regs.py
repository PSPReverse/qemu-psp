#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for gen_regs.py: run with python3 psp/regs/test_gen_regs.py"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_regs.py")
STEMS = ["generic", "zen", "zen_plus", "zen2", "zen3", "zen_tesla"]


def entry(offset, value, source="unknown"):
    return {"offset": offset, "value": value, "source": source}


class GenRegsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.docs = {s: {"spaces": {}} for s in STEMS}

    def run_gen(self):
        paths = []
        for stem, doc in self.docs.items():
            path = os.path.join(self.tmp.name, stem + ".json")
            with open(path, "w") as f:
                json.dump(doc, f)
            paths.append(path)
        out = os.path.join(self.tmp.name, "out.inc")
        p = subprocess.run([sys.executable, GEN, "--output", out, *paths],
                           capture_output=True, text=True)
        text = open(out).read() if p.returncode == 0 else ""
        return p, text

    @staticmethod
    def offsets(text, gen, space="smn_misc"):
        m = re.search(r"psp_regs_%s_%s\[\] = \{(.*?)\};" % (gen, space),
                      text, re.S)
        return re.findall(r"UINT64_C\((0x[0-9a-f]+)\), UINT64_C", m.group(1)) \
            if m else []

    def test_generic_is_flattened_into_every_generation(self):
        self.docs["generic"]["spaces"]["smn_misc"] = [entry("0x10", "0x1")]
        p, text = self.run_gen()
        self.assertEqual(p.returncode, 0, p.stderr)
        for gen in STEMS[1:]:
            self.assertEqual(self.offsets(text, gen), ["0x10"])

    def test_child_overrides_and_parent_chain(self):
        self.docs["generic"]["spaces"]["smn_misc"] = [entry("0x10", "0x1")]
        self.docs["zen2"]["spaces"]["smn_misc"] = [entry("0x10", "0x2"),
                                                   entry("0x20", "0x3")]
        self.docs["zen3"]["parent"] = "zen2"
        p, text = self.run_gen()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.offsets(text, "zen3"), ["0x10", "0x20"])
        self.assertEqual(self.offsets(text, "zen"), ["0x10"])

    def test_mask_hides_only_in_that_generation(self):
        self.docs["generic"]["spaces"]["smn_misc"] = [entry("0x10", "0x1")]
        self.docs["zen2"]["spaces"]["smn_misc"] = [entry("0x10", None)]
        p, text = self.run_gen()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.offsets(text, "zen2"), [])
        self.assertEqual(self.offsets(text, "zen"), ["0x10"])

    def test_mask_without_inherited_value_fails(self):
        self.docs["zen2"]["spaces"]["smn_misc"] = [entry("0x10", None)]
        p, _ = self.run_gen()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("masks a value that is not inherited", p.stderr)

    def test_repeating_inherited_value_fails(self):
        self.docs["generic"]["spaces"]["smn_misc"] = [entry("0x10", "0x1")]
        self.docs["zen2"]["spaces"]["smn_misc"] = [entry("0x10", "0x1")]
        p, _ = self.run_gen()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("repeats the inherited value", p.stderr)

    def test_repeating_value_inherited_from_parent_fails(self):
        self.docs["zen2"]["spaces"]["smn_misc"] = [entry("0x10", "0x2")]
        self.docs["zen3"]["parent"] = "zen2"
        self.docs["zen3"]["spaces"]["smn_misc"] = [entry("0x10", "0x2")]
        p, _ = self.run_gen()
        self.assertNotEqual(p.returncode, 0)

    def test_cycle_and_unknown_parent_fail(self):
        self.docs["zen2"]["parent"] = "zen3"
        self.docs["zen3"]["parent"] = "zen2"
        p, _ = self.run_gen()
        self.assertIn("parent cycle", p.stderr)
        self.docs["zen2"]["parent"] = "nope"
        p, _ = self.run_gen()
        self.assertIn("unknown parent", p.stderr)

    def test_generic_cannot_have_parent_or_mask(self):
        self.docs["generic"]["parent"] = "zen"
        p, _ = self.run_gen()
        self.assertIn("generic.json cannot have a parent", p.stderr)
        del self.docs["generic"]["parent"]
        self.docs["generic"]["spaces"]["smn_misc"] = [entry("0x10", None)]
        p, _ = self.run_gen()
        self.assertIn("only a generation file can mask", p.stderr)

    def test_source_is_required_and_checked(self):
        self.docs["generic"]["spaces"]["smn_misc"] = [
            {"offset": "0x10", "value": "0x1"}]
        p, _ = self.run_gen()
        self.assertIn("needs 'offset', 'value' and 'source'", p.stderr)
        self.docs["generic"]["spaces"]["smn_misc"] = [
            entry("0x10", "0x1", "guessed")]
        p, _ = self.run_gen()
        self.assertIn("is not one of", p.stderr)


if __name__ == "__main__":
    unittest.main()
