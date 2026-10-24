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

    def run_gen(self, tsv=False):
        paths = []
        for stem, doc in self.docs.items():
            path = os.path.join(self.tmp.name, stem + ".json")
            with open(path, "w") as f:
                json.dump(doc, f)
            paths.append(path)
        out = os.path.join(self.tmp.name, "out.inc")
        self.tsv_path = os.path.join(self.tmp.name, "out.tsv")
        extra = ["--tsv", self.tsv_path] if tsv else []
        p = subprocess.run([sys.executable, GEN, "--output", out, *extra,
                            *paths], capture_output=True, text=True)
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

    def test_tsv_has_one_resolved_row_per_entry(self):
        e = entry("0x10", "0x1", "inferred")
        e["note"] = "multi\tword\nnote"
        self.docs["generic"]["spaces"]["smn_misc"] = [e]
        self.docs["zen2"]["spaces"]["smn_misc"] = [entry("0x20", "0x3")]
        self.docs["zen3"]["parent"] = "zen2"
        p, _ = self.run_gen(tsv=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = [r.split("\t") for r in
                open(self.tsv_path).read().splitlines()]
        self.assertEqual(rows[0], ["space", "generation", "offset", "value",
                                   "source", "note"])
        self.assertTrue(all(len(r) == 6 for r in rows))
        zen3 = [r for r in rows if r[1] == "zen3"]
        self.assertEqual([r[2] for r in zen3], ["0x10", "0x20"])
        self.assertEqual(zen3[0][4:], ["inferred", "multi word note"])
        self.assertEqual(len([r for r in rows if r[1] == "zen"]), 1)

    @staticmethod
    def window(offset, size, source="unknown"):
        return {"offset": offset, "size": size, "source": source}

    def test_ram_windows_are_collected_per_generation(self):
        self.docs["generic"]["ram_windows"] = [self.window("0x1000", "0x100")]
        self.docs["zen2"]["ram_windows"] = [self.window("0x2000", "0x10")]
        self.docs["zen3"]["parent"] = "zen2"
        self.docs["zen3"]["ram_windows"] = [self.window("0x3000", "0x20")]
        p, text = self.run_gen(tsv=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        def offs(gen):
            m = re.search(r"psp_regs_ram_%s\[\] = \{(.*?)\};" % gen, text,
                          re.S)
            return re.findall(r"UINT64_C\((0x[0-9a-f]+)\), UINT64_C\(0x",
                              m.group(1))
        self.assertEqual(offs("zen"), ["0x1000"])
        self.assertEqual(offs("zen2"), ["0x1000", "0x2000"])
        self.assertEqual(offs("zen3"), ["0x1000", "0x2000", "0x3000"])
        rows = [r for r in open(self.tsv_path).read().splitlines()
                if r.startswith("ram_window\tzen3")]
        self.assertEqual(len(rows), 3)

    def test_ram_window_errors(self):
        self.docs["generic"]["ram_windows"] = [self.window("0x1000", "0x0")]
        p, _ = self.run_gen()
        self.assertIn("needs 0 < size", p.stderr)
        self.docs["generic"]["ram_windows"] = [self.window("0xfffffff0",
                                                           "0x100")]
        p, _ = self.run_gen()
        self.assertIn("needs 0 < size", p.stderr)
        self.docs["generic"]["ram_windows"] = [self.window("0x2000", "0x10"),
                                               self.window("0x1000", "0x10")]
        p, _ = self.run_gen()
        self.assertIn("sorted by offset", p.stderr)
        self.docs["generic"]["ram_windows"] = [self.window("0x1000", "0x100")]
        self.docs["zen2"]["ram_windows"] = [self.window("0x1080", "0x10")]
        p, _ = self.run_gen()
        self.assertIn("overlaps", p.stderr)

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
