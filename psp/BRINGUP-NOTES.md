# PSP model: interrupts, debug knobs and Zen3 bring-up notes

Facts only; items marked (guess) are inferred from firmware disassembly, not measured on hardware.

## Interrupt controller (psp/psp-irq.c)
- Window at PSP address 0x03010200 (Zen2/Zen3+) or 0x03010300 (Zen1, Zen+). Registers inside the window:
  `+0x0..0xc` per-group enable mask (4 groups x 32 sources, flat id = 32*group + bit),
  `+0xb0..0xbc` ACK (write 1<<bit clears pending), `+0xc0` PEN (reads 1 when nothing is pending),
  `+0xc4` ID (lowest pending flat id). Absolute: ACK 0x030102b0, PEN 0x030102c0, ID 0x030102c4 on Zen2+.
- Core lines: both ARM IRQ and FIQ follow (pending & enable). The off-chip BL services IRQ, SecOS services FIQ.
- Sources: 128 GPIO inputs. Wired: CCP completion = id 0x15; mailbox doorbell = id 0x3c (default);
  timer expiry = opt-in (see below).
- State is in the vmstate so fast snapshots restore it. CCP int_status and mailbox int_enable are NOT yet.

## Debug knobs
- `-global driver=amd_psp.irq,property=force_group0,value=<mask>`: group-0 sources pending from reset.
- `-global driver=amd_psp.mailbox,property=doorbell_irq_id,value=<N>`: IC source raised on a write to mailbox
  word 0 (0x03010570) made while the vCPU is not executing guest code (harness/x86-side injection). Default 0x3c,
  -1 disables. Armed only once the device-side enable at 0x0301069c (SecOS writes 0xff000000) is non-zero.
- `-global driver=amd_psp.timer,property=expiry_irq_id,value=<N>`: periodic expiry on the 0x03010400 timer
  (period at +0x10, 100 MHz); 0x60 is the tick id SecOS expects (guess). Off by default so snapshots hold no host timers.
- Traces: `psp_irq_enable_write` (enable-mask writes), `psp_irq_set_bit`/`psp_irq_clear_bit` (real raise/ACK),
  `psp_mailbox_write_attrs`, `psp_mailbox_ctrl_read/write`.

## CCP
- Each completed queue sets bit 17 of 0x03006008 (write-1-to-clear) and raises IC id 0x15.
- A zero RSA modulus returns zeros instead of letting GMP raise SIGFPE (guest-controlled input).

## Zen3 EPYC (amd-psp-zen3) bring-up status
Boots on-chip BL -> off-chip BL (0.13.0.7A from ASUS KRPA-U16 BIOS 1002) up to PSP status 0x107.
Needed (all in psp/smn/psp-smn-misc.c, psp/x86/psp-x86-misc.c, psp/smn/psp-smn.c):
- SMN misc ready bit 8 at 0x6c14c/0x6c94c/0x6d14c; fuse word 0x5da20 = 1 (guess: code needs SoC class 2 so the
  FET walk at fn 0x134c8 does not return 0; value inferred from code).
- Flash image also visible at SMN 0x47000000 (the off-chip BL reads it there; the on-chip BL uses 0x44000000).
- 16550 LSR at legacy COM2/3/4 (0x2f8/0x3e8/0x2e8) answers THRE|TEMT like COM1.
Open: the 0x107 stop (analysis paused by the project lead), XTS-AES/DES3/ECC engines are no-ops, queue status is
always success, SecOS idle timer write 0x0301070c and tick block 0x03010a44 are not modelled.
The Zen3 on-chip BL, flash images and BLs are proprietary and are NOT in this repository.
