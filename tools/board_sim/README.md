<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# board_sim -- RA8D2 board emulator (companion dev tool)

Boots the **real, unmodified firmware `.elf`** -- the same image that flashes to
the EK-RA8D2 -- on an emulated Cortex-M and shows it in a graphical **board
view**: the GLCDC panel framebuffer on the left, plus a status sidebar on the
right with three LED indicators (lit in the real GPIO LED colour) and live
USB / UART / timer-IRQ / touch state. So a NON-display example (blink, USB,
UART, timers) is observable graphically, and a display example still shows its
screen beside the status panel. board_sim runs the actual cross-compiled ARM
binary, so it exercises the genuine bring-up and peripheral-driver code path --
the panel/UI you see is exactly what the flashed firmware draws.

Standalone tool under `tools/`. Needs Unicorn + Capstone: `brew install unicorn
capstone` on macOS. On Linux, Capstone is the distro `libcapstone-dev` package
but **Unicorn is version-pinned** -- its decode of Armv8.1-M (Helium/MVE)
differs across releases, so an unpinned emulator makes the same commit pass on
one box and fault on another (#354). Provision the pin (currently 2.1.4) with
`bash scripts/ci/install_unicorn.sh` (see `docs/TOOLCHAIN.md`); the board-sim
gates fail loudly if the runtime Unicorn is not the pin. Both are discovered via
CMake's `find_library`/`find_path`. The live
board view (`--view`) is a macOS Cocoa window; every other path -- headless
boot, the MMIO report, `--ppm`, the console capture -- builds and runs headless
on Linux as well (the CMake links a no-op window shim off the APPLE path and
references zero AppKit symbols), which is what lets a board-sim smoke gate run
on the Linux CI runner.

## Build & run

```sh
cd tools/board_sim && cmake -B build -S . && cmake --build build -j
./build/board_sim <firmware.elf>                      # headless: boot + MMIO report
./build/board_sim <firmware.elf> --view               # live board view (macOS window)
./build/board_sim <firmware.elf> --ppm out.ppm        # write the full composite frame
./build/board_sim <firmware.elf> --panel <file.toml>  # size the window to a display
./build/board_sim <firmware.elf> --size 480x272       # explicit size (overrides --panel)
./build/board_sim <firmware.elf> --input "ping\r\n"   # feed bytes to the console UART RX
./build/board_sim <firmware.elf> --usb-in "ping"      # feed bytes to the USB CDC bulk OUT pipe
./build/board_sim <firmware.elf> --sd <image>         # attach a pre-built FAT image as the microSD
./build/board_sim <firmware.elf> --sd-new 64:fat32    # create + attach a blank 64 MiB FAT32 card
./build/board_sim <firmware.elf> --sd-new 16 --save-sd out.img  # blank card, then dump it after
./build/board_sim <firmware.elf> --trace-sym ra8_usb_device_attach   # log each entry to a function
./build/board_sim <firmware.elf> --device ra8p1       # emulate the RA8P1 (adds the Ethos-U55 NPU window)
```

### Emulated part: RA8D2 (default) or RA8P1

`--device ra8d2|ra8p1` picks which RA8 part the model emulates (default
`ra8d2`). The RA8P1 (`R7KA8P1KFLCAC`) shares the RA8D2's entire register map and
memory map -- 1 MB MRAM at `0x02000000`, 1664 KB SRAM at `0x22000000`, and all
155 peripheral bases are byte-identical -- so the same peripheral models serve
both parts, and an RA8P1-linked ELF (e.g.
`examples/ra8p1_foundation/blink_ra8p1`) boots and runs exactly as the RA8D2
blink does. The RA8P1 is "RA8D2 + an Arm Ethos-U55 NPU"; see
`docs/reference/ra8p1_vs_ra8d2.md`.

`--device ra8p1` additionally exposes the one RA8P1-only register window -- the
**Ethos-U55 NPU** at `0x40140000` (4 KiB; ELC event `0x067`).

It is modelled **honestly but not implemented**: the window is mapped (so
NPU-touching firmware does not read a spinning ready-bit toggle), every read
returns a stable `0` (no fabricated `NPU_ID`, no faked ready/done bit -- an
inference is never faked), writes are recorded, and the end-of-run report prints
a `MAPPED BUT UNMODELLED` line with the access tally whenever the window was
touched. A real Ethos-U55 command-stream model is a follow-up (issue #222). On
the default RA8D2 profile this block is gated off entirely, so the RA8D2 run is
byte-for-behaviour unchanged. (Note the RA8P1 has **no OFS3** option register; no
in-tree app depends on it, so the shared OFS window is left mapped and harmless.)

### Configuring the simulated devices

The microSD card is set up at launch with no pre-built image required:

- `--sd-new <N>[k|m|g][:fat16|fat32]` formats a **blank FAT volume of any size**
  and attaches it. A bare number is MiB; a `k`/`m`/`g` suffix sets the unit, so
  multi-GB cards are easy: `--sd-new 64:fat32`, `--sd-new 8:fat16`, `--sd-new 10g`,
  `--sd-new 30g`. The format defaults by size (FAT32 at >= 512 MiB) like a real
  card -- FAT16/FAT12 cannot exceed their cluster ceilings, so big cards are
  FAT32; cluster size grows with capacity (8 KiB at ~2 GiB, 32 KiB at ~8 GiB+).
  The backing store is a **sparse mmap**, so a 30 GiB card costs ~kilobytes of
  host RAM (only the sectors the formatter + firmware actually touch are
  materialised -- a full 30 GiB run measured ~18 MiB RSS). The BPB is complete
  enough for a host `fsck_msdos` and the firmware's `ra8_fs` mount alike, and the
  modelled card's CSD reports the chosen capacity (so the firmware sees the real
  size -- `sd: card=30720 MB` for `--sd-new 30g`). `--sd <image>` still attaches a
  pre-built image when you need specific contents.
- `--save-sd <out>` writes the card image (with whatever the firmware wrote) out
  after the run -- inspect it with `fsck_msdos out` / `hdiutil attach out`, or
  feed it back in with `--sd out` next time.
- Both work **headless and in `--view`**: the end-of-run report prints an `SD
  card : <N> MB FAT<bits> '<label>'` line, and the live sidebar shows the same
  `SD:` row alongside the USB / UART / IRQ state, so the configured devices are
  always visible. (`fsck_msdos`-validated FAT16 and FAT32; `tz_secure_only_sd`
  mounts a freshly-created card and round-trips a file in-sim, no hardware.)

`--trace-sym <name>` is a debugging instrument: it arms a code hook on a symbol's
entry address and logs every time control reaches it (with the calling LR), so a
multi-stage bring-up that stalls is visible without a full instruction trace --
e.g. tracing the USBX device worker (`ux_dcd_ra8_usb_initialize`,
`ra8_usb_device_attach`, the USBX `_ux_*` calls) pinpoints exactly which init step
a stuck enumeration never reaches. Pair it with `--dump-sym <global>` (print a
32-bit global after the run) and `--stop-sym <global> <N>` (end the run the
instant a 32-bit global reaches `N` -- the counter analog of
`BOARD_SIM_STOP_ON`; the sim resets every counter to 0 on boot, so this is how
`scripts/sim/sil_all.sh` verifies a `jlink_memprobe` progress counter without a
debugger) and the headless run-bounding env vars for post-mortems:
`BOARD_SIM_MAX_CHUNKS` (chunk budget), `BOARD_SIM_WALL_S`
(wall-clock floor), `BOARD_SIM_IDLE_STOP=N` (stop once observable state is
unchanged for N chunks -- an RTOS idle spin), `BOARD_SIM_USB_STOP=N` (stop N
chunks after the virtual host reports the device CONFIGURED -- the USB device
apps never idle, so this is what makes the enumeration gate fast and
deterministic), `BOARD_SIM_USBH_STOP=N` (the host-mode counterpart: stop N
chunks after the virtual boot keyboard has streamed its reports / the MSC host
reaches its read-only write test), and `BOARD_SIM_STOP_ON="<substr>"` (a generic
banner stop: end the run as soon as the console UART's last line contains
`<substr>` -- used to stop an app that loops forever after a success line).

The console UART is captured: every byte the firmware writes to an SCI TDR is
echoed to stdout (`[uart] SCIn: ...`), so a console app's output is greppable
(`board_sim uart_hello.elf | grep 'hello'`). `--input <str>` queues bytes into
the EK-RA8D2 console channel's (SCI8) receive path, with `\n` / `\r` / `\t`
escapes decoded -- so an echo example like `uart_irq_echo` can be driven
headlessly.

The display is configurable: `--panel` takes a flat `key = value` descriptor
(`name`, `width`, `height`, ... -- the files in `tools/board_sim/panels/`),
so the emulator presents any screen, not just the EK-RA8D2 1024x600.

Or from the repo root: `make sim-<app> [PANEL=<name>]` (e.g.
`make sim-ereader_ui`) cross-builds the app and opens its live window
sized by `tools/board_sim/panels/<PANEL>.toml` (default `ek_ra8d2`). Close to
exit. board_sim is the single simulator: it boots the *real cross-compiled
`.elf`*, so a chrome app like `ereader_ui` doubles as the UI preview --
there is no separate native UI tool.

## How it works

- **CPU**: Unicorn (QEMU's core as a library) tops out at Cortex-M33 (Armv8-M),
  but the M85 (Armv8.1-M) firmware executes on it -- the boot path emits no
  v8.1-M-only opcode. An invalid-instruction trap reports any that ever appears.
- **Memory map**: ITCM / MRAM (code+vectors) / DTCM / SRAM / DATA_FLASH / SDRAM /
  PPB mapped as RAM; the Renesas peripheral space (`0x40000000`+) is callback MMIO.
- **Peripheral model** (sparse): control writes read back as written, so
  "configure then verify" works; once the firmware spins reading one address (a
  "wait for ready/idle" poll) past a threshold, reads alternate `0` / all-ones so
  a single-bit poll for either edge completes instead of timing out. This generic
  rule satisfies almost every stabilization poll on the boot path.
- **Register-accurate blocks** (`board_periph.{c,h}` core + one
  `board_periph_<blk>.c` per block): the core is a *decentralized block
  registry* + MMIO dispatch + the ICU/NVIC interrupt path; each peripheral block
  lives in its own file and self-registers with the core, superseding the sparse
  fallback for its register range. The blocks today are GPIO/PORT
  (`board_periph_gpio.c`), the GPT + AGT timers (`board_periph_timer.c`), the
  **SCI_B UART** (`board_periph_sci.c`), and I3C/I2C + the GT911 touch device
  (`board_periph_i2c.c`). The SCI model captures each TDR write to the console
  sink, serves a host RX byte queue through RDR (CSR.RDRF tracks availability,
  CSR.TDRE/TEND stay asserted so the driver's transmit empty/end polls fall
  through), and raises TXI/TEI/RXI through the core's ICU event path so
  interrupt-driven serial works as well as polled. The core models the NVIC
  ISER/ICER set-enable / clear-enable semantics so a firmware that enables
  several IRQ lines (e.g. SCI RXI+TXI+TEI) keeps them all enabled. See
  [Adding a peripheral block](#adding-a-peripheral-block) for how a new block
  joins with no central-list edit.
- **USBFS controller + a virtual USB host** (`board_usb.{c,h}`): models the
  USB-FS device-mode register block (SYSCFG pull-up, INTSTS0 CTSQ/DVSQ/VALID +
  the CTRT/DVST/BRDY event bits, the CFIFO data port with its CFIFOSEL/CFIFOCTR
  FRDY/BVAL/BCLR/DTLN handshake, USBREQ..USBLENG SETUP latches, DCPCTR PID/CCPL,
  the bulk pipe FIFOs and BRDYSTS), and drives a virtual host that runs the
  standard chapter-9 enumeration against the *real* ThreadX + Eclipse USBX
  CDC-ACM firmware. The host watches SYSCFG.DPRPU, issues a bus reset, then walks
  GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION + the CDC line requests --
  raising the USBFS interrupt through the same ICU->NVIC path so the genuine ISR
  (`ra8_usb_dispatch` -> `ux_dcd_ra8_usb_irq`) answers each SETUP, clocking the
  device's descriptor responses out of the CFIFO until USBX's CDC-ACM activate
  callback fires. `--usb-in <str>` then drives the bulk OUT pipe and reads the
  device's echoed bulk IN, so the CDC data path is exercised end-to-end.
- **Targeted quirk**: the MRMS frequency latches (MRCFREQ/MREFREQ) strip a write
  key byte on readback -- modelled directly, since it is an exact-value (not
  edge) poll. This is currently the *only* register needing bespoke behaviour.
- **Time**: bare-metal delays are SysTick-driven. Between emulation chunks the
  installed `SysTick_Handler` is cooperatively invoked as a function so the tick
  counter advances and `ra8_delay_ms` returns.
- **Display**: the current frame is read from emulated GLCDC state each present
  -- the BG_BGC background colour, with the GR1 graphics-layer framebuffer
  (decoded from FLM2/FLM3/FLM5/FLM6) blitted over it out of emulated RAM.
- **Board view** (`board_overlay.{c,h}`): each present, the panel frame is
  composited with a status sidebar into one RGB565 buffer -- three LED dots that
  light in the real GPIO colour (LED1 blue / LED2 green / LED3 red, read from the
  GPIO/PORT model) when their pin is driven high, and text lines for the live
  `USB:` state, the last captured `UART:` line, the `IRQ:` taken counts, and the
  last `touch` point. The sidebar text is drawn with an embedded 5x7 ASCII font
  *into the same pixel buffer* the window shows, so a non-display example carries
  its whole "show" in the sidebar and **`--ppm` captures the full composite**
  (panel + sidebar) -- the overlay is therefore verifiable headlessly with a
  region/pixel check, not just visible on screen. main.c reads the state through
  read-only `board_periph` / `board_usb` getters; the Cocoa view only blits the
  result, so the renderer itself is plain, portable, AppKit-free C.

## Adding a peripheral block

The peripheral model is **decentralized**: the `board_periph.c` core owns only
the block registry, the MMIO dispatch, and the ICU/NVIC routing. It keeps **no
hand-maintained list of blocks**, so a new block (and several in parallel) can
be added without touching the core. Adding a block is exactly two steps:

1. **Add `src/board_periph_<blk>.c`.** Include `board_periph_block.h`, implement the
   block's `read` / `write` (required) and `tick` / `reset` / `report` (each
   optional -- use `nullptr` if the block has none), describe the block with a
   static `board_periph_block_t` (its absolute register `base` / `span`, an
   `order` from `board_periph_block_order_t`, and the handler pointers), and
   self-register it from a file-scope constructor:

   ```c
   static const board_periph_block_t k_my_block = {
       .base = 0x40xxxxxxUL, .span = 0xNNN, .order = k_block_order_xxx,
       .read = my_read, .write = my_write,
       .tick = my_tick, .reset = my_reset, .report = my_report,
       .name = "MYBLK",
   };
   [[gnu::constructor]] static void my_block_register(void) {
       board_periph_register_block(&k_my_block);
   }
   ```

   `board_sim` is a host program, so the constructor runs before `main` and the
   block is registered by the time `board_periph_init` resets it. To pend an
   interrupt, call `board_periph_icu_raise_event(uc, <ELC event>)` -- the core
   owns the IELSR table, the NVIC enable shadow and the IRQ ring; check
   `board_periph_trace()` before logging.

2. **Add the file to the source list** in `CMakeLists.txt`.

That is all -- no edit to a central block list. MMIO is dispatched by disjoint
address range (registration order is irrelevant), and `tick` / `reset` /
`report` run in ascending descriptor `order`, so two blocks added in parallel
never conflict. If a block needs a board-view getter, declare it in
`board_periph.h` and implement it in the block file (as the GPIO LED, UART
last-line and GT911 touch getters already are). Bump `k_block_max` in the core
only if you exceed the registry capacity.

## Status / limits

- Proven on `lcd_color_cycle`: boots clocks -> SDRAM -> GLCDC and cycles the
  background colour (red/green/blue/white), shown live.
- Proven on `display_pal_animation`: its RGB565 GR1 framebuffer in on-chip SRAM
  is read back and rendered (real drawn pixels, not just a background colour).
- Proven on `uart_hello`: the real polled SCI8 TX path is captured -- the
  `hello, ra8d2!` banner appears on stdout once per loop iteration.
- Proven on `uart_irq_echo`: the interrupt-driven SCI8 path -- `--input` bytes
  raise RXI, the ISR echoes them back over TXI, and the echo is captured
  (IRQ0/RXI + IRQ1/TXI both fire, RX/TX byte counts match).
- Proven on `threadx_usbx_cdc_demo` / `usb_cdc_echo`: the real ThreadX + USBX
  CDC-ACM device fully enumerates against the virtual host -- the step log shows
  each SETUP (GET_DESCRIPTOR device/config/string -> SET_ADDRESS ->
  SET_CONFIGURATION), the device reaches CONFIGURED, USBX's CDC-ACM activate
  callback fires (`USB: device CONFIGURED (CDC-ACM active)`), and with
  `--usb-in <str>` the bulk bytes round-trip back through the device's echo
  (`sent N OUT, read N IN`). Final PC sits in the ThreadX run loop, not a panic.
  This is now a **regression gate** across all three device classes:
  `scripts/sim/smoke.sh usb_cdc_echo threadx_usbx_cdc_demo usb_hid_device
  usb_msc_device` asserts the `device CONFIGURED` milestone for CDC-ACM, HID
  (boot mouse), and MSC (BOT/SCSI + a sector read), so a change that breaks USB
  enumeration fails the smoke suite -- no hardware needed. The gate earned its
  keep immediately: `--trace-sym` traced the USBX device worker and showed the
  CDC and HID demos were silently stalling in `_ux_device_class_*_initialize`
  with `UX_MEMORY_INSUFFICIENT` -- their 16 KiB USBX pool could not satisfy the
  class's cache-safe IN buffer, so the device never asserted its pull-up. Raising
  those pools to 32 KiB (matching the HIL-validated `usb_selftest_cdc` and the
  already-correct `usb_msc_device`) restored full enumeration; the gate keeps it
  from regressing again.
- Proven on `usb_host_keyboard`: the inverse path -- the firmware as USB **host**.
  The USBHS host controller (`0x40351000`) is unmodelled, so board_sim seams the
  first-party `ra8_usb_host_*` primitives to a **virtual HID boot keyboard** (the
  same function-seam it uses for `ra8_eth_*`, since the register model "cannot
  satisfy" that sequence either). The firmware's real host stack enumerates the
  virtual device (device + config + HID descriptors -> SET_ADDRESS ->
  SET_CONFIGURATION), opens the interrupt-IN pipe, reads the boot-keyboard input
  reports and decodes the keycodes -- `ra8d2 hid: host decoded keys "RA8D2" ...
  USB HOST KEYBOARD PASS`.
- Proven on `usb_host_msc_browse`: the host as USB **mass-storage host**. board_sim
  seams the higher-level `ra8_usb_hmsc_*` class API (one level above BOT/SCSI) to a
  **virtual read-only FAT16 disk** whose single file `MRAM.BIN` is the live 1 MiB
  MRAM window -- the boot/FAT/root sectors are a byte-exact replica of the device's
  synthesis, and the data region is read straight out of emulated MRAM, so it
  matches the host's own MRAM compare byte-for-byte. The firmware's real host stack
  enumerates it, `READ_CAPACITY`s, mounts the FAT16 over `ra8_fs`, browses the root
  directory (finds `MRAM.BIN`, 1 MiB), content-verifies all 1 048 576 bytes against
  MRAM, and confirms a `WRITE(10)` is rejected (read-only) -- `ra8d2 host: USB HOST
  MSC BROWSE PASS`.
- Proven on `usb_host_file_ops`: the same MSC disk made **read-write**. When the
  firmware links `fileops_backend_write` the virtual disk accepts `WRITE(10)` into
  a small sector overlay (a host file touches only a handful of FAT / root / data
  sectors), so the host's `ra8_fs` can create, read back, rename, and unlink a
  file: the app runs its full nine-step ladder (write `USBTEST.TXT`, verify the
  payload, rename to `USBDONE.TXT`, unlink) and prints `ra8d2 fileops: ALL FILE
  OPS PASSED`. All three host apps are gated: `scripts/sim/smoke.sh
  usb_host_keyboard usb_host_msc_browse usb_host_file_ops` asserts each PASS
  banner. The seam picks the virtual device's class + writability from the
  firmware's linked host stack (`ra8_usb_hmsc_*` -> disk, `fileops_backend_write`
  -> writable, else `ra8_usb_host_*` -> keyboard) and is symbol-gated, so
  device-mode apps are entirely unaffected.
- ThreadX apps run on the real PendSV/SysTick-scheduled exception path now; the
  USBX/CDC stacks all execute as the actual cross-compiled `.elf`.
- The graphical board view is proven via `--ppm` region checks: `blink` lights
  the LED1 indicator pure blue (RGB565 0x001F) while P600 is high; the
  `threadx_usbx_cdc_demo` sidebar shows `USB: CONFIGURED (CDC-ACM active)` once
  enumeration completes; `uart_hello` shows `UART: hello, ra8d2!`;
  `gpt_irq_demo` shows `IRQ: 9999 total IRQ0 x9999`; and a display app
  (`ereader_ui`) renders its chrome pixel-correct in the panel region with the
  sidebar beside it.
- The peripheral model fakes hardware *handshakes*; it validates "does the
  firmware drive the controller correctly," not real silicon timing. It
  complements HIL, it does not replace it.

## Example coverage

Every EK-RA8D2 example was booted on the emulator (`scripts/sim/smoke.sh`
gates a bare-metal subset in CI). **72 of 75 run to their main loop and produce
their expected output.** The three that do not are one honest category:

- **A real firmware bug the emulator found (3):** `agt_periodic`,
  `agt_cascade_demo`, `agt_pulse_demo` fault after exactly 255 timer periods.
  Each re-arm leaks one `ra8_mstp` reference and `ra8_mstp_enable` saturates a
  `uint8_t` refcount at 255; the short HIL run never reaches it, the emulator's
  longer run does. The emulator is faithful here -- it ran the real firmware long
  enough to expose the leak. Tracked in issue #68.

Two earlier gaps are now closed: `cpu1_pingpong` runs on the **second emulated
core** (CPU1's real code in a second Unicorn engine over shared SRAM ->
`g_cpu1_pingpong_mismatch == 0`), and `mpu_partition_simple` faults + recovers
under the now-**enforced Armv8-M MPU** (`mpu: fault handled, recovered`).

Host-Unicorn note: Unicorn is version-pinned (2.1.4) across every board_sim
environment -- dev box, devcontainer and CI runner (`docs/TOOLCHAIN.md`, #354) --
so the Armv8.1-M decode is identical everywhere. An earlier version of this note
claimed the runner ran 2.0.1; it did not. The runner carried a source-built
2.1.4 while the dev box and devcontainer ran 2.0.1, and 2.0.1 raises a spurious
`EXCP_NOCP` on the Helium/MVE store family -- which is exactly what made the same
commit fault locally and pass in CI. On the pinned build those stores decode
natively; any residual ThreadX first-context-switch PendSV misbehaviour is a
board_sim defect around the ICSR `PENDSVSET` write, not a Unicorn one, and is
tracked separately.
