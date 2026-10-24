#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate psp-regs.c.inc from the register tables in psp/regs.

usage: gen_regs.py --output psp-regs.c.inc [--tsv psp-regs.tsv] FILE.json...

Every file named in REQUIRED_FILES must be given exactly once. Each file is
a JSON object {"spaces": {SPACE: [{"offset": HEX, "value": HEX,
"source": SOURCE, "note": S}]}}. A generation file may also name a "parent"
generation. A generation sees its own entries, then its parent chain, then
generic.json; tables are flattened here, so the runtime does one lookup. A
"value" of null in a generation file masks an inherited value.
HEX must be canonical (lowercase, no leading zeros). Lists must be sorted by
offset with no duplicates. "note" is optional but must not be empty. Values
are 32-bit. An entry must not repeat the value it would inherit, and a mask
must hide a value that is inherited.
"""

import argparse
import json
import os
import re
import sys

# File stem -> PspGeneration enumerator (include/psp/generations.h).
GENERATIONS = {
    "zen": "ZEN",
    "zen_plus": "ZEN_PLUS",
    "zen2": "ZEN2",
    "zen3": "ZEN3",
    "zen_tesla": "ZENTESLA",
}
GENERIC = "generic"
REQUIRED_FILES = {GENERIC, *GENERATIONS}

# JSON space key -> PspRegSpace enumerator (include/psp/psp-regs.h), and the
# exclusive upper bound for offsets in that space (None: 64-bit).
SPACES = {
    "smn_misc": ("PSP_REG_SPACE_SMN_MISC", (1 << 32) - 1),
    "psp_misc": ("PSP_REG_SPACE_PSP_MISC", None),
    "x86_misc": ("PSP_REG_SPACE_X86_MISC", None),
}
VALUE_LIMIT = 1 << 32
OFFSET_LIMIT = 1 << 64
# Where a value comes from. Nothing is "measured" unless it was read on real
# hardware; "unknown" marks entries migrated without provenance.
SOURCES = {"measured", "inferred", "code-reading", "unknown"}
ENTRY_KEYS = {"offset", "value", "source", "note"}
HEX_LOOSE = re.compile(r"0[xX][0-9a-fA-F]+")
HEX_CANON = re.compile(r"0x(?:0|[1-9a-f][0-9a-f]*)")


def fail(path, msg):
    sys.exit(f"gen_regs.py: {path}: {msg}")


def no_duplicate_keys(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError(f"duplicate key {key!r}")
        obj[key] = value
    return obj


def parse_hex(path, where, text):
    if not isinstance(text, str) or HEX_LOOSE.fullmatch(text) is None:
        fail(path, f'{where}: {text!r} is not a hex string such as "0x1f"')
    value = int(text, 16)
    if HEX_CANON.fullmatch(text) is None:
        fail(path, f'{where}: {text!r} is not canonical, write "0x{value:x}"')
    return value


def load(path, is_generation):
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f, object_pairs_hook=no_duplicate_keys)
    except ValueError as e:
        fail(path, f"invalid JSON: {e}")
    if not isinstance(doc, dict):
        fail(path, "top level must be an object")
    extra = sorted(set(doc) - {"spaces", "parent"})
    if extra or "spaces" not in doc:
        fail(path, f"top level must have the key 'spaces' and may have "
                   f"'parent' (found {sorted(doc)})")
    if not isinstance(doc["spaces"], dict):
        fail(path, "'spaces' must be an object")

    parent = doc.get("parent")
    if parent is not None:
        if not is_generation:
            fail(path, "generic.json cannot have a parent")
        if parent not in GENERATIONS:
            fail(path, f"unknown parent {parent!r}, expected one of "
                       f"{sorted(GENERATIONS)}")
    result = {space: [] for space in SPACES}
    for space, entries in doc["spaces"].items():
        if space not in SPACES:
            fail(path, f"unknown space {space!r}, expected one of "
                       f"{sorted(SPACES)}")
        if not isinstance(entries, list):
            fail(path, f"space {space!r} must be a list")
        max_offset = SPACES[space][1] or OFFSET_LIMIT
        prev = None
        for i, e in enumerate(entries):
            where = f"{space}[{i}]"
            if (not isinstance(e, dict) or
                    not {"offset", "value", "source"} <= set(e)):
                fail(path, f"{where}: needs 'offset', 'value' and 'source'")
            if not set(e) <= ENTRY_KEYS:
                fail(path, f"{where}: unknown key(s) "
                           f"{sorted(set(e) - ENTRY_KEYS)}")
            if e["source"] not in SOURCES:
                fail(path, f"{where}: source {e['source']!r} is not one of "
                           f"{sorted(SOURCES)}")
            note = e.get("note")
            if "note" in e and (not isinstance(note, str) or not note):
                fail(path, f"{where}: 'note' must be a non-empty string, "
                           f"or omit the key")
            offset = parse_hex(path, where + ".offset", e["offset"])
            if e["value"] is None:
                if not is_generation:
                    fail(path, f"{where}: only a generation file can mask "
                               f"a value with null")
                value = None
            else:
                value = parse_hex(path, where + ".value", e["value"])
            if offset >= max_offset:
                fail(path, f"{where}: offset 0x{offset:x} is outside the "
                           f"{space} space (must be < 0x{max_offset:x})")
            if value is not None and value >= VALUE_LIMIT:
                fail(path, f"{where}: value 0x{value:x} is wider than 32 "
                           f"bits")
            if prev is not None:
                if offset == prev:
                    fail(path, f"{where}: duplicate offset 0x{offset:x}; "
                               f"delete one of them")
                if offset < prev:
                    fail(path, f"{where}: offset 0x{offset:x} comes after "
                               f"0x{prev:x}; sort the {space} list by offset")
            prev = offset
            result[space].append((offset, value, e["source"], e.get("note", "")))
    return parent, result


def chain(stem, parents, paths):
    """Return stem's ancestors, nearest first, ending before generic."""
    seen = [stem]
    while parents[seen[-1]] is not None:
        nxt = parents[seen[-1]]
        if nxt in seen:
            fail(paths[stem], "parent cycle: " + " -> ".join(seen + [nxt]))
        seen.append(nxt)
    return seen[1:]


def flatten(data, parents, paths):
    """Resolve inheritance and masks into one sorted table per generation."""
    flat = {}
    for stem in GENERATIONS:
        flat[stem] = {}
        ancestors = chain(stem, parents, paths)
        for space in SPACES:
            inherited = {o: (v, src, note)
                         for o, v, src, note in data[GENERIC][space]}
            for anc in reversed(ancestors):
                for offset, value, src, note in data[anc][space]:
                    if value is None:
                        inherited.pop(offset, None)
                    else:
                        inherited[offset] = (value, src, note)
            merged = dict(inherited)
            for offset, value, src, note in data[stem][space]:
                if value is None:
                    if offset not in inherited:
                        fail(paths[stem], f"{space} offset 0x{offset:x} masks "
                                          f"a value that is not inherited")
                    del merged[offset]
                elif offset in inherited and inherited[offset][0] == value:
                    fail(paths[stem], f"{space} offset 0x{offset:x} repeats "
                                      f"the inherited value 0x{value:x}; "
                                      f"delete it")
                else:
                    merged[offset] = (value, src, note)
            flat[stem][space] = sorted(merged.items())
    return flat


def table_name(stem, space):
    return f"psp_regs_{stem}_{space}"


def render(flat):
    out = [
        "/* Generated by psp/regs/gen_regs.py from the JSON files in",
        " * psp/regs. Do not edit. Included by psp-regs.c. */",
        "",
    ]
    for stem in GENERATIONS:
        for space in SPACES:
            rows = flat[stem][space]
            if not rows:
                continue
            name = table_name(stem, space)
            out.append(f"static const PspRegEntry {name}[] = {{")
            for offset, (value, _, _) in rows:
                out.append(f"    {{UINT64_C(0x{offset:x}), "
                           f"UINT64_C(0x{value:x})}},")
            out.append("};")
            out.append("")

    def cells(stem):
        for space, (enum, _) in SPACES.items():
            rows = flat[stem][space]
            if rows:
                yield f"[{enum}] = {{{table_name(stem, space)}, {len(rows)}}},"
            else:
                yield f"[{enum}] = {{NULL, 0}},"

    out.append(f"QEMU_BUILD_BUG_ON(PSP_GENERATION_MAX != {len(GENERATIONS)});")
    out.append("")
    out.append("static const PspRegTable "
               "psp_regs_by_gen[PSP_GENERATION_MAX][PSP_REG_SPACE_MAX] = {")
    for stem, enum in GENERATIONS.items():
        out.append(f"    [{enum}] = {{")
        out.extend("        " + c for c in cells(stem))
        out.append("    },")
    out.append("};")
    out.append("")
    return "\n".join(out)


def render_tsv(flat):
    """One row per resolved entry; offsets and values as 0x hex strings."""
    out = ["space\tgeneration\toffset\tvalue\tsource\tnote"]
    for stem in GENERATIONS:
        for space in SPACES:
            for offset, (value, src, note) in flat[stem][space]:
                note = " ".join(note.split())
                out.append(f"{space}\t{stem}\t0x{offset:x}\t0x{value:x}"
                           f"\t{src}\t{note}")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", required=True)
    ap.add_argument("--tsv", help="also write the flattened tables as TSV")
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    paths = {}
    for path in args.inputs:
        stem = os.path.splitext(os.path.basename(path))[0]
        if stem in paths:
            sys.exit(f"gen_regs.py: {path}: given twice")
        if stem not in REQUIRED_FILES:
            sys.exit(f"gen_regs.py: {path}: unknown register file")
        paths[stem] = path
    missing = sorted(REQUIRED_FILES - set(paths))
    if missing:
        sys.exit("gen_regs.py: missing register file(s): "
                 + ", ".join(f"{m}.json" for m in missing))

    loaded = {stem: load(paths[stem], stem != GENERIC)
              for stem in REQUIRED_FILES}
    parents = {stem: loaded[stem][0] for stem in REQUIRED_FILES}
    data = {stem: loaded[stem][1] for stem in REQUIRED_FILES}
    flat = flatten(data, parents, paths)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(render(flat))
    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8") as f:
            f.write(render_tsv(flat))


if __name__ == "__main__":
    main()
