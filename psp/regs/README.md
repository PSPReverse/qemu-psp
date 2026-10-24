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

Lookup (`psp_regs_lookup`): the device's generation table first, then
`generic.json`. There is no inheritance between generations (e.g. Zen3 does not
fall back to Zen2). A read only hits an entry when it starts at exactly the entry's offset; a
byte read of the middle of a registered word misses. A miss returns false and the caller reports it as
unimplemented.
