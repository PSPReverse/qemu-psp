# PSP model: debug knobs and exported hooks

Reference for the QOM properties and `aspfuzz_*` symbols that exist only for debugging,
testing and fuzzing. Source: `psp/` and `include/psp/` at e9d4673a.

Status legend: **read** = verified by reading the code at the cited line; **unverified** = not
executed (nothing was built or run for this document).

## 1. Command-line knobs (`-global`, applied before realize)

QEMU's `-global driver=X` matches X and its subclasses (`qom/object.c:456`), so `amd-psp`
also covers `amd-psp-zen*`.

| Property | Device type | Default | Effect | Example | Code |
|---|---|---|---|---|---|
| `force_group0`, `force_group1`, `force_group3` | `amd_psp.irq` | 0 | Bits marked pending on every reset. Bit n of group N is source `32*N+n`. | `-global driver=amd_psp.irq,property=force_group0,value=536870912` (0x20000000, source 0x1d) | `psp-irq.c:294-296` props, `:279-286` reset hold |
| `doorbell_irq_id` | `amd_psp.mailbox` | 0x3c (60) | IC source raised on a host-side write to mailbox word 0 (0x03010570) while the vCPU is not running. `-1` disables. Wired at realize only. | `-global driver=amd_psp.mailbox,property=doorbell_irq_id,value=60` | `mailbox.c:162`, `:84-86`; wiring `psp.c:221-228` |
| `expiry_irq_id` | `amd_psp.timer` | -1 (off) | Periodic expiry raised on IC source N. Period at `+0x10` (100 MHz, 10 ns per tick). Only timer1 is wired. | `-global driver=amd_psp.timer,property=expiry_irq_id,value=96` (0x60, unverified as the tick id) | `psp-timer.c:238` prop, `:42-49` arm, `:52-57` expire; wiring `psp.c:187-192`; timer2 is not connected (`psp.c:193-196`) |
| `flash_img` | `amd_psp.smnflash` | unset | Host path of the flash image loaded into the SMN flash region. Unset or unloadable means `exit(1)`. | `-global driver=amd_psp.smnflash,property=flash_img,value=/path/to/image.bin` | `psp-smn-flash.c:79` prop, `:61-68` realize |
| `dbg_mode` | `amd-psp` (parent of `amd-psp-zen*`) | false | Forwarded to the fuse device; sets bit 10 of fuse register 0x0 (base 0x1a060900). | `-global driver=amd-psp,property=dbg_mode,value=on` | `psp.c:257` prop, `:231-234` forward; `psp-fuse.c:25` |
| `dbg_mode` | `amd_psp.fuse` | false | Same bit, set directly on the fuse device (no forwarding). | `-global driver=amd_psp.fuse,property=dbg_mode,value=on` | `psp-fuse.c:54`, `:25` |
| `use_access_observer` | `amd_psp.x86.mapper` | false | Routes the x86 slot memory regions through the access observer instead of the x86 memory. Read at realize. | `-global driver=amd_psp.x86.mapper,property=use_access_observer,value=on` | `psp-x86-mapper.c:316-317`, `:235-236` |

Notes:
- `psp.c:256` comments the `dbg_mode` syntax as `-global amd-psp.<property>=<value>`. That is
  not the QEMU syntax; use `driver=amd-psp,property=dbg_mode,value=...` as above. (read)
- The `expiry_irq_id` value 0x60 is a guess taken from the code comments (`psp-irq.c:275`
  says "suspected tick"; `psp-timer.c` comment says "SecOS expects (guess)"). Unverified.

### Internal wiring properties (set by the parent device, not knobs)

`psp_irq_ident`, `psp_irq_generation`, `psp_misc_msize`, `psp_misc_ident`,
`psp_misc_generation`, `psp_generation`, `psp_smn_misc_generation`, `smn-container-base`
(`psp.c:120-138`, `psp-smn.c:262,316`), `in_x86_space` (`psp-x86.c:110`), and the
`x86_memory` link (`access-observer.c:134`, `psp-x86-mapper.c:276`). Do not set these from
the command line.

## 2. Runtime knobs (`qom-set`, device must be realized)

Only one property has a runtime setter. All `DEFINE_PROP_*` properties are refused after
realize (`hw/core/qdev-properties.c:35-36`), so the table in section 1 is command-line only.

| Property | Object path | Type | Effect | Code |
|---|---|---|---|---|
| `inject` | `/machine/soc/irq` | uint32, set only | Raises IC source `value` (the GPIO input). The firmware clears it with the ACK write. Error if not realized or `value >= 128`. No getter, so `qom-get` fails. | `psp-irq.c:178-196` setter, `:218-224` registration |

Path derivation (read, not checked against a live `info qom-tree`): the machine creates the
SoC as child `soc` (`amd-psp_zen.c:64`), and the PSP device creates child `irq`
(`psp.c:85`).

QMP:
```
{"execute": "qom-set", "arguments": {"path": "/machine/soc/irq", "property": "inject", "value": 29}}
```
HMP (unverified syntax): `qom-set /machine/soc/irq inject 29`

Value 29 raises source 0x1d. Sources are `group*32 + bit`, 128 total (`psp-irq.h:28`).

## 3. Exported `aspfuzz_*` symbols

All are non-static and get no header declaration, except where noted. "BQL" means the
caller must hold the big QEMU lock (not checked for all of them).

### Functions

| Symbol | Signature | Location | Preconditions | Return |
|---|---|---|---|---|
| `aspfuzz_irq_inject` | `int32_t (uint32_t id)` | `psp-irq.c:207` | Takes BQL itself (`:209`). Global set at realize (`:266`), cleared at unrealize (`:228`). | `-1` if no IC realized or `id >= 128`; else `0`. |
| `aspfuzz_smn_update_slot` | `void (uint32_t idx)` | `psp-smn.c:39` | Needs `aspfuzz_smn_state` set (`:132`, by a guest slot write). No bounds check on `idx` (array of 32, `:35`). Otherwise NULL dereference at `:42`. BQL: unverified. | none |
| `aspfuzz_smn_write` | `int (hwaddr addr, void* buf, int size)` | `psp-smn.c:49` | `global_smn_as` set at realize (`:273`), never cleared. Uses `address_space_write_rom`. BQL: unverified. | `-1` if unset; else the `address_space_write_rom` result (unverified convention). |
| `aspfuzz_smn_read` | `int (hwaddr addr, void* buf, int size)` | `psp-smn.c:58` | Same as above, `address_space_read`. | `-1` if unset; else the `address_space_read` result (unverified convention). |
| `aspfuzz_write_smn_flash` | `void (hwaddr addr, hwaddr len, const void* ptr)` | `psp-smn-flash.c:40` | `asp_smn_mr` set at flash realize (`:73`), cleared at unrealize (`:76`). Otherwise logs and returns. No bounds check of `addr+len` against the flash size: `memcpy` into the RAM block (`:48-49`). BQL: not taken. | none |
| `aspfuzz_x86_write` | `int32_t (hwaddr addr, void* buf, hwaddr size)` | `psp-x86.c:40` | `global_x86_state` set at PSP x86 realize (`:87`), never cleared. Writes via the x86 address space. Contains a leftover debug trace call at the top of the function. | `-1` if unset; else the `address_space_write` result (unverified convention). |
| `aspfuzz_x86_read` | `int32_t (hwaddr addr, void* buf, hwaddr size)` | `psp-x86.c:52` | As above, `address_space_read`. | `-1` if unset; else the `address_space_read` result. |
| `aspfuzz_access_observer_activate` | `int32_t (hwaddr addr, hwaddr size)` | `access-observer.c:21` | Global set at realize (`:123`), never cleared (no unrealize). Sets the observed window. | `-1` if unset; else `0`. |
| `aspfuzz_access_observer_reset` | `int32_t (void)` | `access-observer.c:32` | As above. Clears the window and the recorded read/write address and PC. | `-1` if unset; else `0`. |
| `aspfuzz_access_observer_status` | `int32_t (bool get_read, hwaddr* addr, vaddr* pc)` | `access-observer.c:47` | As above. Copies the last recorded address and PC. The PC is 0 outside vCPU context (`get_pc`, `:63-76`). The name "illegal" is historical: nothing checks legality. | `-1` if unset; else `0`. |
| `aspfuzz_write_catcher_activate` | `int32_t (hwaddr addr, hwaddr size)` | `write-catcher.c:19` | Global set at realize (`:128`), never cleared. The device is not instantiated by any code in this tree (grep), so the global stays NULL. | `-1` if unset; else `0`. |
| `aspfuzz_write_catcher_reset` | `int32_t (void)` | `write-catcher.c:30` | As above. | `-1` if unset; else `0`. |
| `aspfuzz_write_catcher_status` | `int32_t (bool get_read, hwaddr* addr, vaddr* pc)` | `write-catcher.c:45` | As above. | `-1` if unset; else `0`. |

### Data symbols

| Symbol | Type | Location | Semantics |
|---|---|---|---|
| `aspfuzz_timer_count_0`, `aspfuzz_timer_control_0` | `uint64_t` | `psp-timer.c:33-34` | Shadow of timer1 (MMIO 0x03010400). On each MMIO access the device adopts a differing harness value (`:68-79` read path, `:149-160` write path) and publishes its own value at the end of the access (`:129-133`, `:206-210`). |
| `aspfuzz_timer_count_1`, `aspfuzz_timer_control_1` | `uint64_t` | `psp-timer.c:35-36` | Same, for every other timer instance (timer2 at 0x03010424). |
| `aspfuzz_smn_slots[32]` | `uint32_t[32]` | `psp-smn.c:35` | SMN slot address table. Written by guest slot programming (`:133`), read by `aspfuzz_smn_update_slot`. |
| `aspfuzz_smn_state` | `PSPSmnState*` | `psp-smn.c:36` | Set on every slot update (`:132`). |
| `global_smn_as` | `AddressSpace*` | `psp-smn.c:47` | Not prefixed, but used only by the `aspfuzz_smn_*` functions. |
| `asp_smn_mr` | `MemoryRegion*` | `psp-smn-flash.c:38` | Not prefixed, but used only by `aspfuzz_write_smn_flash`. |

Concurrent writes to the data symbols from a host thread are not synchronised anywhere in
the code (unverified whether the harness serialises them).

## 4. Unverified or inconsistent items

- Nothing above was executed. All `-global` and `qom-set` examples are from the code, not
  from a run. Hex values for `-global` (`0x...`) are accepted by QEMU's numeric parser, but
  this was not tested; decimal equivalents are used where it matters.
- The `-global` examples assume the default machine path and driver names as listed. The
  live `/machine/soc/irq` path was not checked with `info qom-tree`.
- `psp-irq.c:200-201` says the BQL is "taken if the caller does not hold it". The code
  (`:209`, `BQL_LOCK_GUARD()`) takes it unconditionally (`include/qemu/main-loop.h:410`).
  Calling `aspfuzz_irq_inject` while holding the BQL is therefore a deadlock or assertion
  risk. Whether QEMU asserts or deadlocks was not verified.
- `psp-irq.c:270-277` comment: "group3 bits 16..20 (ids 0x70..0x73)". Bits 16..20 of group 3
  are ids 0x70..0x74, so the range in the comment is one short. Not changed here.
- Timer tick id 0x60 (source 96) is a guess (see section 1).
- The write-catcher device has no instantiation in this tree. Whether `-device
  amd_psp.x86.write_catcher` would realize cleanly was not tested.
- The CCP completion (id 0x15) and the mailbox doorbell default (0x3c) are wiring, not knobs,
  and are listed only where they are overridable.
