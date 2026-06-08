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
screen beside the status panel. Unlike `tools/simulator` (which recompiles the
GUIX UI natively), board_sim runs the actual ARM binary, so it exercises the
genuine bring-up and peripheral-driver code path.

Standalone tool under `tools/`, outside the firmware CI gates. Needs Unicorn +
Capstone (`brew install unicorn capstone`).

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
```

The console UART is captured: every byte the firmware writes to an SCI TDR is
echoed to stdout (`[uart] SCIn: ...`), so a console app's output is greppable
(`board_sim uart_hello.elf | grep 'hello'`). `--input <str>` queues bytes into
the EK-RA8D2 console channel's (SCI8) receive path, with `\n` / `\r` / `\t`
escapes decoded -- so an echo example like `uart_irq_echo` can be driven
headlessly.

The display is configurable: `--panel` takes a flat `key = value` descriptor
(`name`, `width`, `height`, ... -- the same files as `tools/simulator/panels/`),
so the emulator presents any screen, not just the EK-RA8D2 1024x600.

Or from the repo root: `make emulate-<app> [PANEL=<name>]` (e.g.
`make emulate-bedroom_ui_panel`) cross-builds the app and opens its live window
sized by `tools/simulator/panels/<PANEL>.toml` (default `ek_ra8d2`). Close to exit.
(`make simulate-<app>` is kept as a backward-compatible alias.)

Don't confuse this with `make sim` (`tools/simulator`): that recompiles the
shared GUIX UI *natively* on macOS for fast, clickable UI design; `emulate-<app>`
boots the *real cross-compiled `.elf`* on the CPU emulator for high-fidelity
bring-up validation.

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
- **Register-accurate blocks** (`board_periph.{c,h}`): a per-block model table
  supersedes the sparse fallback for GPIO/PORT, the AGT/GPT timers, the ICU/NVIC
  interrupt path, and the **SCI_B UART**. The SCI model captures each TDR write
  to the console sink, serves a host RX byte queue through RDR (CSR.RDRF tracks
  availability, CSR.TDRE/TEND stay asserted so the driver's transmit empty/end
  polls fall through), and raises TXI/TEI/RXI through the same ICU event path so
  interrupt-driven serial works as well as polled. It also models the NVIC
  ISER/ICER set-enable / clear-enable semantics so a firmware that enables
  several IRQ lines (e.g. SCI RXI+TXI+TEI) keeps them all enabled.
- **USBFS controller + a virtual USB host** (`board_usb.{c,h}`): models the
  USB-FS device-mode register block (SYSCFG pull-up, INTSTS0 CTSQ/DVSQ/VALID +
  the CTRT/DVST/BRDY event bits, the CFIFO data port with its CFIFOSEL/CFIFOCTR
  FRDY/BVAL/BCLR/DTLN handshake, USBREQ..USBLENG SETUP latches, DCPCTR PID/CCPL,
  the bulk pipe FIFOs and BRDYSTS), and drives a virtual host that runs the
  standard chapter-9 enumeration against the *real* ThreadX + Eclipse USBX
  CDC-ACM firmware. The host watches SYSCFG.DPRPU, issues a bus reset, then walks
  GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION + the CDC line requests --
  raising the USBFS interrupt through the same ICU->NVIC path so the genuine ISR
  (`ra_usb_dispatch` -> `ux_dcd_ra_usb_irq`) answers each SETUP, clocking the
  device's descriptor responses out of the CFIFO until USBX's CDC-ACM activate
  callback fires. `--usb-in <str>` then drives the bulk OUT pipe and reads the
  device's echoed bulk IN, so the CDC data path is exercised end-to-end.
- **Targeted quirk**: the MRMS frequency latches (MRCFREQ/MREFREQ) strip a write
  key byte on readback -- modelled directly, since it is an exact-value (not
  edge) poll. This is currently the *only* register needing bespoke behaviour.
- **Time**: bare-metal delays are SysTick-driven. Between emulation chunks the
  installed `SysTick_Handler` is cooperatively invoked as a function so the tick
  counter advances and `ra_delay_ms` returns.
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
- ThreadX apps run on the real PendSV/SysTick-scheduled exception path now; the
  GUIX UI (`threadx_guix_demo`) renders its real framebuffer (77 distinct
  colours) and the GUIX/USBX/CDC stacks all execute as the actual cross-compiled
  `.elf`.
- The graphical board view is proven via `--ppm` region checks: `blink` lights
  the LED1 indicator pure blue (RGB565 0x001F) while P600 is high; the
  `threadx_usbx_cdc_demo` sidebar shows `USB: CONFIGURED (CDC-ACM active)` once
  enumeration completes; `uart_hello` shows `UART: hello, ra8d2!`;
  `gpt_irq_demo` shows `IRQ: 9999 total IRQ0 x9999`; and a display app
  (`bedroom_ui_panel`) renders its dashboard pixel-correct in the panel region
  with the sidebar beside it (a `--click` tab switch still diffs ~222k panel
  pixels and the sidebar reports the `touch` point).
- The peripheral model fakes hardware *handshakes*; it validates "does the
  firmware drive the controller correctly," not real silicon timing. It
  complements HIL, it does not replace it.
