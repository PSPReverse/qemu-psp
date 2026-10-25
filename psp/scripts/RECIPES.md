# gdb recipes for the PSP

`psp_gdb.py` is loaded inside gdb (`trace.py` is the standalone tracer, see TRACE.md).
Each command takes a YAML/JSON spec (file path or inline JSON) and writes one JSON line per hit.

## Start

    nix-shell -p gdb 'python3.withPackages (p: [p.pyyaml])'     # yaml is optional, JSON works without
    # QEMU, stopped, gdbstub on PORT (machine/firmware options as in your usual invocation):
    qemu-system-arm <machine and firmware options> -gdb tcp::PORT -S
    # gdb:
    gdb -nx -ex 'set pagination off' -ex 'target remote :PORT' \
        -ex 'source psp/scripts/psp_gdb.py' -ex 'psp-out trace.jsonl' \
        -ex 'psp-names psp/scripts/psp_addrs.csv' -ex '<psp-... command>' -ex continue

`psp-names CSV` fills the `name` field (exact symbol, else `sym+0xoff`). `psp-clear` removes our breakpoints.
Line fields: `event, name, r0..r12, sp, lr, pc, cpsr, lr_name` plus per-command extras (hex strings).
gdb has no symbols here: append `thumb: true|false` to a spec to force the instruction set of its breakpoint.

## 1. BL status 0x01 spin / bl_start_mp5 call-return trace

    psp-func '[{"addr":"0x9508"},{"addr":"0x95bc"},{"addr":"0xd6f8"},{"addr":"0xd702"},{"addr":"0xd718"},{"addr":"0xd75c"},{"addr":"0xd79a"},{"addr":"0xd79c"},{"addr":"0xd668"}]'
    psp-caller '[{"addr":"0x9c60","sp_off":12}]'

`psp-func` logs `entry` (a0..a3 = r0..r3, `depth`) and `return` (`ret_r0`, first non-zero flagged
`first_nonzero`). Returns are matched by lr and sp, so a function that never returns leaves its
frame open. The addresses are a mix of function entries and mid-function points; for a mid-function
address `entry` just means "reached" and its return line is whatever follows lr at that moment.
`psp-caller` at the spin prints `lr` and the saved lr at `[sp+12]` (bytes; change `sp_off`).

## 2. SVC trace

    psp-svc '[{"addr":"0x8","name":"svc_vector"}]'            # breakpoint on the SVC vector, number from lr
    psp-svc '[{"addr":"0xADDR","source":"pc"}]'                # or on a specific SVC instruction

From lr: ARM reads the word at `lr-4` (mask 0xffffff), Thumb the halfword at `lr-2` (mask 0xff).
Exception entry clears cpsr.T, so the set is inferred from which encoding is an SVC; if both or
neither look like one the line has `svc: null` and a `note` (force it with `"thumb": true|false`).
Verify the vector address for the target before use; 0x8 is the architectural default only.

## 3. Who reads SMN 0x50df4 / 0x51050

These are SMN addresses, not PSP addresses. The PSP reaches SMN through an index/data window or
mapped slot, and a gdb watchpoint only sees the PSP-side address. So:

- if the PSP-side address that maps SMN 0x50df4 is known (a mapped slot), watch it:
  `psp-who-touches '[{"addr":"0xPSPADDR","kind":"r","name":"smn_50df4"}]'`
  (`lr` and `pc` in the line identify the reader; `value` is the memory content at the hit).
- if access goes through an index register, watch the index register with `kind:"w"` and filter
  the lines for value 0x50df4 / 0x51050, then the next data-register access is the reader
  (watch the data register with `kind:"a"` in the same call).
- otherwise gdb cannot do this. Use QEMU's own SMN device tracing instead (`-d unimp,guest_errors`
  or a trace event on the SMN device), which sees the SMN address directly.
`rwatch` needs the stub to support read watchpoints (QEMU's does, Z3/Z4).

## 4. Dumps

    psp-dump 0x3210000 0x100 out.bin
    psp-dump-struct '[{"addr":"0x32000d8","name":"err","fields":[{"name":"code","off":0,"size":4}]}]'

## Requesting a trace from another agent

Post a spec list, not prose: command name, the addresses, and what you want to learn, e.g.

    psp-func [{"addr":"0xd6f8","name":"a"},{"addr":"0xd702"}]
    psp-caller [{"addr":"0x9c60"}]

Whoever has a free QEMU runs it with `psp-out` and posts the JSONL back (or its path).
