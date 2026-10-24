# PSP constant register tables

Constant register reads are data, not code. Each `*.json` file lists
`{"offset", "value", "note"}` entries per register space (`smn_misc`,
`psp_misc`, `x86_misc`). `generic.json` applies to every generation;
`zen.json`, `zen_plus.json`, `zen2.json`, `zen3.json` and `zen_tesla.json`
hold entries for one generation only.

To add a register, add an entry to the right file: canonical lowercase hex
strings, 32-bit values, offsets sorted and unique per space and file, no entry
that repeats a generic one with the same value, a `source` and a note with the
evidence. `source` is `measured` only for values read on real hardware, else
`inferred`, `code-reading` or `unknown`. `gen_regs.py` runs at build time and
emits `psp-regs.c.inc` into the build directory; do not edit it by hand.

Inheritance: a generation file may name a `"parent"` generation at the top
level. A generation sees its own entries, then its parent chain, then
`generic.json`, nearest first. No parent means generic only. A `"value": null`
entry in a generation file masks an inherited value for that generation; it
must hide something that is inherited, and an entry must not repeat the value
it would inherit. `gen_regs.py` flattens this at build time into one sorted
table per generation, so `psp_regs_lookup` does a single binary search. A
read only hits an entry when it starts at exactly the entry's offset; a
byte read of the middle of a registered word misses. A miss returns false
and the caller reports it as unimplemented. RAM windows: a file may list `"ram_windows"` (`offset`, `size`, `source`,
`note`), ranges of the SMN address space that behave as plain RAM. Windows of
`generic.json`, the parent chain and the file itself are added up and must not
overlap; the SMN device maps each as RAM, so DMA engines can use them too.

`gen_regs.py --tsv FILE` also writes the flattened tables for other tools: one
row per resolved entry with the columns space, generation, offset, value,
source, note (offsets and values as 0x hex strings); RAM windows appear as
`ram_window` rows with the size in the value column. Run `python3
psp/regs/test_gen_regs.py` after changing the generator.
