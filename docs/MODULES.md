# Module tour (`libs/`)

A paragraph each on the libraries a newcomer meets first. This is a reading
order, not an index: [`libs/README.md`](../libs/README.md) lists every library
in the tree, and the public header named under "main entry points" carries the
full API with the project's mandatory Doxygen tag set.

Vendored third-party libraries live under `libs/third_party/` and are
documented as SOUP under [`docs/SOUP/`](SOUP/); they are not covered here.

## ra8_core

Foundational primitives that every other library depends on: error
codes (`ra8_err_t`), structured logging (`ra8_log_*`), runtime asserts,
the bit-position constants used throughout the HAL, and the stack
canary supervisor. Nothing in `ra8_core` touches a hardware register --
it is the project's freestanding-C runtime.

* Headers: `libs/ra8_core/inc/ra8_*.h`
* Main entry points: `ra8_infrastructure_init()`, `ra8_log_init()`,
  `ra8_stack_canary_check()`.

## ra8_hal

The hand-written hardware abstraction layer for the RA8D2 on-chip
peripherals: the serial buses, the timers and PWM, analog in and out,
the interrupt and event fabric, DMA, external memory, networking,
display and camera, USB, crypto and the watchdogs -- one driver per
peripheral, over a bank of `ra8_*_regs.h` register-map headers written
from the HUM. Every driver returns `ra8_err_t` and is unit-tested against
`tests/mocks/ra8_fake_mmap.c`, which presents the MCU peripheral
address space as host-side RAM.

* Headers: `libs/ra8_hal/inc/ra8_*.h` and `libs/ra8_hal/inc/ra8_*_regs.h`
* Main entry points: per-peripheral `ra8_<periph>_init()` /
  `_open()` / `_deinit()`.

## ra8_board_ek_ra8d2

Board-support pinning and resource-id table for the EK-RA8D2 v1
evaluation kit: LED handles, push-button handles, on-board PMOD
mappings, and the `ra8_board_get_info()` discovery call apps use to
key off the exact board variant.

* Header: `libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2.h`
* Main entry points: `ra8_board_get_info()`, `ra8_board_led_init()`.

## ra8_mpu

Cortex-M85 Memory Protection Unit configuration helper. Lets apps
declare an MPU layout as a static `ra8_mpu_cfg_t` table at
init-time and have the helper program the regions in the right
order, then enable the MPU.

* Header: `libs/ra8_mpu/inc/ra8_mpu.h`
* Main entry points: `ra8_mpu_configure()`, `ra8_mpu_enable()`.

## ra8_nsc

TrustZone Non-Secure Callable veneers. Lives in the secure world and
exposes a curated subset of secure-side services (key vault, OTA
commit, register windows, etc.) to the non-secure firmware via the
`__attribute__((cmse_nonsecure_entry))` ABI. There is no public
header per se -- the veneer set is split across
`ra8_nsc.h`, `ra8_nsc_cgc.h`, `ra8_nsc_comms.h`, `ra8_nsc_io.h` and
`ra8_nsc_veneer.h`.

* Headers: `libs/ra8_nsc/inc/ra8_nsc_*.h`
* Main entry points: NSC veneers prefixed `ra8_nsc_*`.

## ra8_net_pal

Network platform-abstraction layer over the RA8D2 ESWM block. Owns
the DMA descriptor rings, MAC bring-up, and the byte pump up to
whatever IP stack sits above it. Stack-agnostic by design: the NetX
Duo port in `port/netxduo/` builds on it.

* Header: `libs/ra8_net_pal/inc/ra8_net_pal.h`
* Main entry points: `ra8_net_pal_init()`, `ra8_net_pal_set_mac_addr()`.

## ra8_usb_pal

USB device-mode platform-abstraction layer. Hides the FS-vs-HS
controller choice, the MSTP / clock-gate dance, and per-endpoint
software ring buffers behind a small stack-agnostic API. CherryUSB
binds against this layer; TinyUSB or a hand-rolled stack could
substitute without changing this header.

* Header: `libs/ra8_usb_pal/inc/ra8_usb_pal.h`
* Main entry points: `ra8_usb_pal_init()` / `_attach()`, then the
  per-endpoint `_ep_*` calls.

## ra8_tls

Thin facade over the vendored Mbed TLS + TF-PSA-Crypto stack. Hands out
TLS sessions from a fixed-size static pool (NASA Power-of-10 Rule 3) and
translates Mbed TLS error codes into `ra8_err_t`. Higher-level apps
(HTTPS client, MQTT/TLS, OTA fetch) call into this facade instead of
pulling Mbed TLS directly.

* Header: `libs/ra8_tls/inc/ra8_tls.h`
* Main entry points: `ra8_tls_global_init()`, then the per-session
  open / handshake / send / recv calls.

## ra8_psa_crypto

Application-level facade over `tf-psa-crypto`. Wraps PSA Crypto API
key-import / sign / verify / AEAD encrypt+decrypt / random calls and
returns `ra8_err_t`. Pairs with the secure-side key vault exposed
through `ra8_nsc_key_vault`.

* Header: `libs/ra8_psa_crypto/inc/ra8_psa_crypto.h`
* Main entry points: `ra8_psa_crypto_init()`, `ra8_psa_key_import()`,
  and the `ra8_psa_aead_*` pair.

## ra8_ota

Over-the-air firmware update orchestrator: manifest fetch,
slot management against the dual-bank Octo-SPI flash layout, signed
image verification (delegated to `ra8_psa_crypto`), and the secure-
side commit veneer in `ra8_nsc_ota`.

* Header: `libs/ra8_ota/inc/ra8_ota.h`
* Main entry points: `ra8_ota_init()`,
  `ra8_ota_check_for_update()`.

## ra8_modem_at

Cellular modem AT command/response driver layered on top of an
`ra8_uart` instance. Owns the line buffer, the response parser, and
the URC dispatcher. Used by the ereader and the OTA-over-cellular
example paths.

* Header: `libs/ra8_modem_at/inc/ra8_modem_at.h`
* Main entry points: `ra8_modem_at_init()`, `ra8_modem_at_send_cmd()`,
  `ra8_modem_at_poll()`.

## ra8_c6link

The single integration boundary between this firmware and the ESP32-C6
companion radio. Owns the esp-hosted payload header and its checksum,
the serial endpoint's TLV envelope, the protobuf `Rpc` control plane
(encode, decode, UID correlation, event decode), the polled transaction
pump, the 802.3 data plane, and Wi-Fi station control. Everything
hardware-shaped sits behind a three-function transport seam, which
`port/esp-hosted/` binds to the OS-abstraction vtable on the board and
`tests/mocks/ra8_c6_model.c` binds to a co-processor model on the host.

The generated protobuf codec allocates, and this firmware has no heap,
so the codec is handed a bump allocator over a caller-supplied array
that is emptied after every message -- which keeps the whole control
plane inside NASA Power of 10 Rule 3.

* Headers: `libs/ra8_c6link/inc/ra8_c6link.h`,
  `libs/ra8_c6link/inc/ra8_c6link_wifi.h`,
  `libs/ra8_c6link/inc/ra8_c6link_transport.h`
* Main entry points: `ra8_c6link_open()` and `ra8_c6link_poll()` for the
  link itself, `ra8_c6link_eth_send()` for the data plane, and the
  `ra8_c6link_wifi_*` calls for station control.

## ra8_fs

First-party FAT12/FAT16/FAT32 + exFAT filesystem (read + write) backed
by a swappable block-device interface. The platform's only filesystem
since the vendored FileX was retired (#611); used by the ereader app to
walk EPUBs off an SD card, and by the ThreadX demos over LevelX.

* Header: `libs/ra8_fs/inc/ra8_fs.h`
* Main entry points: `ra8_fs_mount()`, `ra8_fs_open()`,
  `ra8_fs_read()`, `ra8_fs_write()`.

## ra8_epub

EPUB (.epub) reader and chapter iterator. Walks the ZIP container
through miniz, parses the OPF manifest + NCX spine, and hands
chapter XHTML to `ra8_reflow` for layout.

* Header: `libs/ra8_epub/inc/ra8_epub.h`
* Main entry points: `ra8_epub_open()`, `ra8_epub_get_chapter_count()`,
  `ra8_epub_load_chapter()`.

## ra8_reflow

HTML reflow + paginate engine for the ereader: it pages chapter content
into the viewport so the GLCDC layer can blit one page at a time. The
default engine is first-party, zero-allocation and MC/DC-testable; a
litehtml-backed variant exists behind a build option that is off, and
`docs/EPUB_CONFORMANCE.md` is the contract for what either one renders.

* Header: `libs/ra8_reflow/inc/ra8_reflow.h`
* Main entry points: `ra8_reflow_init()`,
  `ra8_reflow_layout_chapter()`.

## ra8_gfx

Software 2D graphics primitives for the parallel TFT: framebuffer
clear, pixel/line/rect/circle, bitmap blits, and bundled bitmap
fonts. Sits on top of the GLCDC driver in `ra8_hal`.

* Headers: `libs/ra8_gfx/inc/ra8_gfx{,_font,_text}.h`
* Main entry points: `ra8_gfx_clear()`, `ra8_gfx_line()`,
  `ra8_gfx_text_out()`.

## ra8_touch_cal

Resistive/capacitive touch-screen calibration utility. Runs the
classic 3- or 5-point capture sequence, computes the affine
transform, and applies it to live raw samples.

* Header: `libs/ra8_touch_cal/inc/ra8_touch_cal.h`
* Main entry points: `ra8_touch_cal_run()`, `ra8_touch_cal_apply()`.

## ra8_power_profile

Power-profiling helper that inserts named region markers into the
firmware so power-rail captures (Joulescope or J-Link energy probe)
can be correlated to firmware activity. Trivially small, but used
across most of the example apps.

* Header: `libs/ra8_power_profile/inc/ra8_power_profile.h`
* Main entry points: `ra8_power_profile_init()`,
  `ra8_power_profile_mark_enter()`, `ra8_power_profile_mark_exit()`.

## ra8_wdt_supervisor

ThreadX-aware watchdog supervisor. Provides a per-thread check-in
registry so a hung thread starves the watchdog refresh and triggers
a reset, instead of one healthy thread keeping the watchdog happy
indefinitely. Falls back to a plain refresh path in non-RTOS apps.

* Header: `libs/ra8_wdt_supervisor/inc/ra8_wdt_supervisor.h`
* Main entry points: `ra8_wdt_supervisor_init()`,
  `ra8_wdt_supervisor_register_thread()`, `ra8_wdt_supervisor_checkin()`.
