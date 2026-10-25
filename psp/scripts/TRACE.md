# trace.py

Stdlib-only call/return tracer for a QEMU gdbstub (GDB Remote Serial Protocol).

    python3 psp/scripts/trace.py config.json
    python3 psp/scripts/trace.py --selftest

Config keys:

- `gdb`: `"host:port"` of a running stub, or `qemu: {"argv": [...]}` to spawn QEMU
  (`-gdb tcp:127.0.0.1:PORT -S` is appended).
- `breakpoints`: `[{addr, name, kind, regs, mem}]`. `kind` is `hit` (log), `call` (log, then
  trace the matching return via a temporary breakpoint at lr) or `return-of` (log only the
  return). `mem`: `[{reg|addr, off, len}]`, read at the hit.
- `stop`: `first_hit` (address), `first_return_nonzero`, `max_hits`, `wall_s`.
- `svc` / `svc_handler`: breakpoint on the SVC vector handler; the SVC number is decoded from
  the instruction before lr (ARM `lr-4`, Thumb `lr-2`).
- `names_csv`: `address,name` lines (e.g. a Ghidra export) used to name pcs and callees.
- `out`: output file (default stdout). One JSON object per line: hits
  (`n, pc, lr, name, cpsr, r0..r12, sp, depth, mem`), `{"event":"return", callee, ret_pc, r0..r3}`
  (the first nonzero r0 is flagged `first_nonzero`), and a final `{"event":"done"}`.

Breakpoint kind: an odd address, or cpsr.T set at setup, gives Thumb (`Z0,addr,2`); otherwise
ARM (`Z0,addr,4`). Return breakpoints follow the Thumb bit of lr. All breakpoints are removed
and the stub detached on exit.

Example, Milan off-chip BL (`config.json`):

    {
      "qemu": {"argv": ["qemu-system-arm", "<machine and firmware options>"]},
      "breakpoints": [
        {"addr": "0x9508", "kind": "call", "name": "bl_start_mp5", "regs": ["r0", "r1"]},
        {"addr": "0x95bc", "kind": "call", "name": "bl_start_mp5_die", "regs": ["r0", "r1"]}
      ],
      "stop": {"first_hit": "0x9c60", "wall_s": 300},
      "out": "trace.jsonl"
    }
