# Firmware-free AMD PSP qtests

Fast libqtest tests for the PSP devices. They need no firmware, keys or
derived data: the machine boots from an empty flash image generated at test
time, nothing runs on the emulated CPU (the qtest accelerator executes no
code), and every register offset and descriptor field comes from the emulator
source (`include/psp/`, `psp/`, `psp/regs/`), never from a firmware image.

**What "correct" means here.** Every fidelity claim in these tests is against
PSPEmu behaviour as modelled in this emulator, not against hardware. A test
that passes says the emulator does what its source and comments say; it does
not say real silicon does. Behaviour marked "inferred" in the sources stays
inferred.

## Running

    scripts/psp-qtest.sh

builds the test binaries (`nice ninja -j4`, set `PSP_QTEST_NO_BUILD=1` to
skip) and runs, in order, the nine test binaries below and
`psp/regs/test_gen_regs.py`. On a quiet host it should finish in well under
60 s: each case starts one or two machines, about 0.1 to 0.3 s each.

The script runs the binaries directly with `QTEST_QEMU_BINARY` pointing at
`build-nix/qemu-system-arm`. `meson test` is not used: `build-nix` was
configured with meson 1.9.0 and `meson test` there fails on a version
mismatch with other meson versions. If your meson matches, `meson test -C
build-nix amd-psp-mailbox-test ...` works too (names below).

    PSP_QTEST_PENDING=1 scripts/psp-qtest.sh   # include the pending-fixes group
    build-nix/tests/qtest/amd-psp-ccp-test -p /amd-psp/ccp/sha256   # one case

## Coverage

| Binary | Device | Cases |
| --- | --- | --- |
| `amd-psp-mailbox-test` | mailbox, doorbell | external word-0 write rings IC source 0x3c only when 0x0301069c is armed (any SecOS bit, not the low bits); unarmed silent; words 1 and 2 never ring; byte and halfword writes keep neighbours and still ring on word 0; ring, ack, ring; `doorbell_irq_id` property (zen3 off by default, move, disable); a CCP copy onto the mailbox is not a ring |
| `amd-psp-ic-test` | interrupt controller | enable words independent and read back; idle reads; lowest source first across the four groups; ack scope (group, bits); raising twice is one event; re-raise after ack; `inject` range check; Zen/Zen+ block at 0x03010300 |
| `amd-psp-timer-test` | timers | register, misc and placeholder read-back and byte lanes; software counter (one per read when enabled); `counter_hz` counter follows the virtual clock and a write re-anchors it; expiry raises the source every period, stops on control or period 0, 1 us minimum, off without `expiry_irq_id`, timer 2 has no interrupt |
| `amd-psp-ccp-test` | CCP | queue registers per queue; passthrough, completion latch (write one to clear) and IC source 0x15; passthrough through the LSB incl. 256-bit swap; SHA-256 of "abcd" (INIT+EOM, status OK, digest stored reversed), split over two requests, INIT restarts; zlib valid stream and output counter; zlib failure leaves the device usable; TRNG register 0x0c: non-zero, never repeats, same default seed on every run, `trng_seed` changes the stream, narrow reads do not draw |
| `amd-psp-x86-test` | x86 mapper, port 0x61 | mapper register storage and byte lanes; port 0x61 bits 0-1 store, bit 4 toggles per read (reached through slot 0) |
| `amd-psp-migrate-test` | vmstate | save with `migrate` to a file, load with `-incoming defer` / `migrate-incoming` into a second machine, compare: mailbox, IC (enables and pending), timers (registers), CCP (queue regs, interrupt latch, LSB digest, zlib counter, TRNG position), x86 mapper |
| `amd-psp-reset-test` | `system_reset` | mailbox words and enable zero and disarmed; IC enables and pending cleared; timers cleared and expiry cancelled; CCP TRNG restarts, zlib counter clears; PC at the boot vector before and after |
| `amd-psp-todo-test` | PSP_TODO | one log line per hit, default policy is log; `ignore` is silent; same guest-visible state under both; policy change at runtime |
| `amd-psp-pending-fixes-test` | see below | skipped unless `PSP_QTEST_PENDING=1` |
| `psp/regs/test_gen_regs.py` | register table generator | layering: nearest ancestor wins over a three-level chain, parent masks reach children and children can restore, child masks stay local, spaces are independent, rw slots follow the merged order |

`amd-psp-test.h` holds the shared helpers (machine start, IC drain, CCP
descriptor builder, migration through a file).

### Pending-fixes group

`amd-psp-pending-fixes-test` is registered but each case is skipped unless
`PSP_QTEST_PENDING=1`, because the behaviour lives on a branch that is not in
the pin. Run it on a build that contains the branch; when the branch is
merged, move the case into its device's file.

| Case | Branch |
| --- | --- |
| SHA-256 contexts per LSB slot (interleaved hashes); continuation without context logged; zlib failure logged; CCP reset of queues, status and latch | `claude/psp-ccp-fixes` |
| timer expiry phase across a restore | `claude/psp-timer-fixes` |
| enable mask applies to `FIQ_NEXT` and `FIQ_ALL_PROCESSED` | `claude/psp-ic-mask` |
| port 0x61 cleared by reset and migrated | `claude/psp-reset-fixes` |
| doorbell on a debug write, `aspfuzz_mailbox_write` | `claude/psp-doorbell-debug`, `claude/psp-mailbox-write`: always skipped, see gaps |

## Coverage gaps

- The CPU reset fix (`claude/psp-reset-fixes`) cannot be seen from qtest: no
  code runs and no register can be written, so the reset CPU is
  indistinguishable from an untouched one. The reset test only guards the boot
  vector. Needs a TCG test with a tiny guest program.
- Debug-attribute writes (`claude/psp-doorbell-debug`) need the gdbstub or a
  vCPU access; `aspfuzz_mailbox_write`, `aspfuzz_timer_advance` (counter write
  consuming the advance offset) and the `aspfuzz_todo_*` counters are host
  exports of the QEMU process and not reachable from libqtest. PSP_TODO hits
  are therefore counted from the `-d unimp` log.
- Only one PSP_TODO site is reachable from a bare machine
  (`ccp.engine.unknown`); the `stop` policy needs the `AS_LIB` harness.
- IC level (line lowered again) semantics: no device that is reachable from
  qtest lowers its line, so only edge behaviour is covered. The core
  IRQ/FIQ output lines are not observable.
- Migration: SHA and zlib contexts are not migrated by design, so a snapshot
  taken mid-hash is not tested. `stop`/`savevm` needs a block device the
  machines lack; the migration stream is the vmstate path used here. The timer
  expiry deadline across a restore is in the pending group.
- AES, RSA, SHA-384, XTS, DES3, ECC, the SMN flash device, the SMN and PSP
  register tables (beyond the generator), the status port, fuse and the x86
  config space have no qtest coverage.
- Anything that needs a running guest: boot flow, interrupt delivery to the
  core, exceptions.

## Gate

A pin may move only if `scripts/psp-qtest.sh` passes on the new pin with
`PSP_QTEST_PENDING` unset, and, for every pending-fixes case whose branch the
new pin includes, with `PSP_QTEST_PENDING=1` as well. A case that fails after
its branch was merged is a regression, not a flaky test: fix the emulator or
change the test with a stated reason.
