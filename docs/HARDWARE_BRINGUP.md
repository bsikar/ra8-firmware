# Hardware Bring-up Report (EK-RA8D2 v1)

What bench work on the EK-RA8D2 established about this board and this
silicon. Every entry below is meant to still be true a year from now: a
wiring fact, a register contract, or a class of bug that will bite the
next person the same way. The per-session logs, halt-PC tables and
tallies that produced them live in git history and are not repeated
here.

**This document is not a status board.** The authoritative statement of
what runs on silicon is the HIL suite -- `scripts/hil/all.sh`, contract
in [`HIL_SUITE.md`](HIL_SUITE.md) -- which verifies every app under
`examples/ek_ra8d2/hw_validated/hil/` against its own `hil.conf`
manifest. Hand-flashing apps and eyeballing the halted PC does not
scale, and it produced at least one false bug report (see "What a
halt-PC sample proves", below).

**Probing convention.** Bench diagnostics are `volatile` globals read
over SWD after a halt. Always resolve the address with
`arm-none-eabi-nm` against the `.elf` you actually flashed; an address
written down anywhere moves with the next build.

---

## Octo-SPI flash (#44)

The single longest bring-up in the tree, and the most instructive: a
firmware bug that produced a perfect impersonation of a dead board, and
several confident hardware conclusions that had to be retracted.

### The chip-select is CS1, not CS0

The on-board ISSI IS25LX512M's chip-select net `OSPI_FLASH_S_L` (P104,
EK-RA8D2 v1 UM Table 29 p 35) is wired to the xSPI controller's **CS1**
line. A driver that programs the protocol into `LIOCFGCS[0]` and leaves
`CDCTL0.CSSEL = 0` strobes the unconnected CS0 pin: the flash never sees
chip-select, never drives the bus, and a 1S RDID floats to `0x00FFFFFF`
on the board pull-ups. `ra8_xspi_init` therefore writes both
`LIOCFGCS[k_ra8_xspi_onboard_cs]` and `CDCTL0.CSSEL`, and the FSP
EK-RA8D2 OSPI example agrees (`ospi_b.channel = channel.1`).

The manufacturer ID is **0x9D** (ISSI), not the Macronix `0xC2` that
some older comments assumed.

### Reset the part before talking to it

After the controller protocol config is in place, pulse `LIOCTL.RSTCS`
(bit 16) low->high, with `WPCS` held deasserted, to hardware-reset the
flash into its power-on 1S protocol. This is not optional housekeeping:
a previous J-Link OSPI-loader run can leave the part in OPI/DOPI, in
which state it will not answer a 1S RDID at all.

### The CDT command field is left-justified

The manual-command engine transmits `CMDSIZE` bytes **MSB-first from bit
31**. A 1-byte opcode therefore belongs at `CDT[31:24]`, not `[23:16]`.
Right-justifying it clocks out the zero byte above it, so the chip sees
command `0x00` for every RDID / RDSR / WREN / PP / SE. The rule matching
FSP's `r_ospi_b_direct_transfer` is `cmd_shift = 8 * (2 - cmd_bytes)`,
which also puts a 2-byte 8D complementary pair correctly at `[31:16]`.

### What a silent bus looks like from the controller side

Worth recognising, because it is what "the hardware is dead" looks like
when the hardware is fine:

- In 1S the DQ/SIO lines float **high** (`0xFF`, board pull-ups, nothing
  driving). In 8D they read **low**, or the transfer reports
  `INTS.DSTOCS0` -- a DQS timeout, i.e. the part never toggled DQS.
- `CDCTL0.TRREQ` self-clears and `CMDCMP` retires on every transfer. The
  command engine completing is evidence about the *controller*, and none
  at all about the *device*.
- A memory-mapped read of the XIP window returns `0xFFFFFFFF` with no
  bus fault.

### The U15 expander does not gate the OSPI bus

U15 (PI4IOE5V6408, I2C `0x43` on RIIC1) is a sense/override for SW4 and
nothing more. Its entire configuration space was swept on silicon -- all
256 output values, released-to-inputs, all-Hi-Z, and each line driven
individually -- with no effect on the bus. It cannot be the fix for an
OSPI problem.

Its input register `0x0F` is **not** a trustworthy read of the physical
DIP positions: the U15-to-SW4 bit mapping is not published in the UM
(it is in the Design Package schematic), and a root-cause claim built on
decoding that byte had to be retracted. Do not infer switch state from
it.

### SW4-3, and a documentation defect in the board UM

The EK-RA8D2 v1 UM contradicts itself on SW4-3:

- **Table 3 (p 16)**: SW4-3 OFF = Octo-SPI **Active**. The Table 4
  conflict matrix marks SW4-3 OFF + SW4-4 ON invalid, which is only
  consistent with OFF = Active.
- **Section 6.3 prose (p 35)**: says the flash is isolated by turning
  SW4-3 *off*.

Table 3, the conflict matrix and the FSP `ospi_b` example agree, so the
prose is the error (stale text from an older board manual). SW4-3 is an
analog **bus** mux, hardware-only: no firmware and no expander write can
move it.

### There is no software-reachable power gate

Audited against the UM so nobody re-derives it: the board carries two
fixed linear regulators in series (5 V -> 3.3 V, then 3.3 V -> 1.8 V for
the flash rail). The 1.8 V regulator's `EN` pin is a housekeeping net
tied on, not on the MCU GPIO map and not on the expander. The flash pin
assignment (UM Table 29) lists signal pins only -- no power-enable, no
load switch, no level-shifter OE, and no level shifter at all, because
the MCU's OM_0 pads are 1.8 V and drive the part directly. U15 is the
only addressable device on the system I2C bus. So "the flash is powered
down" is never the explanation.

### The lesson

Three separate sessions concluded "hardware fault" from firmware-side
evidence -- an isolation switch, a missing rail, an expander gate -- and
all three were wrong. The disproof was cheap and available the whole
time: J-Link's own OSPI flash loader erases, programs and verifies the
memory-mapped bank, so the part was present, powered and on the bus at
all times. **Before concluding that a peripheral is physically dead,
find an independent path that talks to it.**

---

## Console UART

The J-Link OB VCOM bridge is on **SCI8** (PD02/PD03 under the
`sci_async` PSEL), not SCI3. This was found by sweeping channels 0..9 on
silicon; a wrong channel is silent, not an error.

The host-side node is the `/dev/cu.usbmodem*` whose digits match the
probe's own serial -- the other node on the bus is a different
interface. `make hil-find-jlink` resolves it, and `JLINK_SN` /
`RA8_CONSOLE_TTY` in `.env` pin it. Bench-specific serials stay out of
the tree.

At 115200 8N1 the BRR divisor lands about 2.7% off the nominal baud,
which the J-Link OB CDC bridge accepts. That is at the edge of UART
tolerance: if a new console channel or clock tree ever produces garbage
rather than silence, this is the first thing to re-derive.

---

## Boot and vector tables

**Two weak definitions of the same handler is a silent bug.** Both the
per-app vector table and `ra8_time.c` once defined `SysTick_Handler`
weakly; the linker took the vector table's alias to `Default_Handler`
and quietly discarded the real implementation, so every delay spun
forever. Nothing in the build warns. If a handler appears not to fire,
check for a second definition before checking the hardware.

---

## No heap, and what pulls one in anyway

`rand()` / `srand()` from newlib allocate, so they trip the `sbrk` trap
that enforces NASA Power-of-10 Rule 3, from inside code that never
mentions memory. The tree ships a xorshift32 override for exactly this
reason. The same shape recurs with any library entry point that
allocates lazily on first use -- the failure surfaces as a fatal error
in an unrelated app, at boot, with no allocation in sight.

---

## MPU: MAIR is not optional

Leaving `MAIR0` at zero makes `attr_idx 0` resolve to device-nGnRnE
(strongly-ordered) for every region using it. Instruction fetch from
device-typed memory is UNPREDICTABLE per the Armv8-M ARM (D1.6.7), so
the M85 HardFaults on the first fetch after the MPU is enabled -- which
looks like an MPU permission bug and is not one.

Code and data regions need Normal memory (`attr 0 = 0xFF`: inner+outer
write-back, RW-allocate, non-transient); the peripheral region gets a
separate device attribute (`attr 1 = 0x04`).

---

## RSIP

`ra8_rsip_init` with BIST enabled cannot succeed on this silicon.
`STATUS.BIST_OK` is set by the access-management circuit inside the
sealed RSIP-E engine, not by the host write that the off-target test
path uses to terminate the wait, so the poll exhausts its budget and the
init returns a hardware-init failure.

The deeper problem is that the CTRL/STATUS layout in
`ra8_rsip_regs.h` is **not documented in the RA8D2 HUM**. It was
inferred for the host test path and has never been validated against
silicon. Anything built on it is a hypothesis, not a driver -- see
[`VENDOR_BLOBS.md`](VENDOR_BLOBS.md) for what a real bring-up would
require. Do not paper over it by stubbing the entropy source: weak
entropy that looks like success is worse than a failing init.

---

## USB device

Two findings that cost the most time here:

- **Nothing drives the dispatcher by default.** `ra8_usb_dispatch()` is
  what reads `INTSTS0` and forwards SETUP/BRDY/CTRT to the DCD bridge.
  If no ISR is registered and no loop polls it fast enough, the
  controller accumulates status bits nobody reads, SETUP packets time
  out at the host, and the device attaches electrically but never
  enumerates. A slow poll is the same as no poll: the SETUP window is
  the deadline.
- **Order matters at attach.** The dispatch path has to be running
  *before* `DPRPU` is asserted, not after. Reaching the bus late means
  the host has already given up and suspended the port.

Two related traps: clearing `INTSTS0` before the bridge callback runs
loses the event, and `DCPCTR.PID` must be restored to BUF after each
received SETUP token.

**Clock.** USB-FS needs 48 MHz. From the board's 24 MHz crystal the
in-spec plan is PLL2: `/2` to 12 MHz, `x80` to a 960 MHz VCO, `/4` for
240 MHz at PLL2P, then `/5` in `USBCKDIVCR` -- exactly 48.000 MHz.
(PLL1Q/8 gives 41.67 MHz, which is out of spec.) The `USBCKCR`
SREQ/SRDY handshake will not complete while the USBFS module is still
MSTP-gated: there is no clock for the gating logic to chase, so
`USBCKSRDY` stays 0 forever and the enable times out.

---

## Display and camera pins

The GLCDC pin table for this board is UM **Table 33 p 42** ("Parallel
Graphics Expansion Port"), and it is scattered across ports 2, 5, 6, 7,
8, 9 and B -- nothing like the tidy contiguous blocks the MCU's
pin-capability table suggests. Deriving board wiring from chip
capability is how the original stub tables were wrong (e.g. LCD_CLK is
P515, and TCON0/1/2 are P806/P805/P807).

Those stubs also collide with the LEDs: LED1 is P600, which the stub
claimed as LCDD0. The pin validator catches the conflict, so the
symptom is an init failure whose identity depends purely on which
`*_pins_init()` ran first -- the GLCDC call fails in one app and the
LED call fails in another, from the same underlying overlap. When a pin
claim fails, suspect the table before the driver.

The board has no on-chip SD slot; an SDHI pin table for it is fiction.

---

## Dual-core and the bootloader

An app that pings the M33 will report send timeouts and no reply when no
M33 image is loaded, and that is the designed steady state, not a fault:
the IPC FIFO fills, the retry helper returns a timeout, and the receive
side returns no-data. See [`DUAL_CORE.md`](DUAL_CORE.md) for the release
sequence and why the validated cross-core path is a shared-SRAM mailbox
rather than the IPC peripheral.

`ra8_bootloader` spins in `wfi` when neither bank holds a valid image.
That is its terminal state by design, and a halted PC inside it is not
evidence of a hang.

---

## BLE

The RA8D2 has **no on-chip BLE radio**. Apps that once drove one were
driving a controller that does not exist in this silicon, and the
"missing Renesas patch image" diagnosis was wrong -- there is no such
blob to obtain. BLE now runs on the ESP32-C6 companion across the HCI
transport seam.

---

## What a halt-PC sample proves

Very little on its own. Flashing an app, sleeping, halting and
classifying the PC will report a healthy app as broken if the sample
lands in a settle window -- one app was filed as a hardware-error bug
purely because the halt caught it inside clock init, and the retraction
cost more than the report saved. A PC inside a panic spin also tells you
nothing about which call panicked; the link register and `addr2line` do,
and a diagnostic global that a probe can read afterwards does better
still.

This is why the HIL suite asserts a *positive signal* -- a banner that
only the success path prints, a counter that only a completed loop
advances, a byte-exact echo off the wire -- and why plain
"the chip is alive" is not an accepted mode for a validated app.
