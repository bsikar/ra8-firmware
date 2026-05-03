# Module tour (`libs/`)

A one-paragraph guided tour of every first-party library under
`libs/`. For the full API surface read the public header listed under
"main entry points" -- every public function carries the project's
mandatory Doxygen tag set (see `CLAUDE.md`).

Vendored third-party libraries (mbedTLS, ThreadX, NetX Duo, FileX,
GuiX, USBX, NimBLE, lwIP, miniz, stb, litehtml, gumbo) live under
`libs/third_party/` and are documented as SOUP under
[`docs/SOUP/`](SOUP/) -- they are not enumerated here.

## ra_core

Foundational primitives that every other library depends on: error
codes (`ra_err_t`), structured logging (`ra_log_*`), runtime asserts,
the bit-position constants used throughout the HAL, and the stack
canary supervisor. Nothing in `ra_core` touches a hardware register --
it is the project's freestanding-C runtime.

* Headers: `libs/ra_core/inc/ra_*.h`
* Main entry points: `ra_infrastructure_init()`, `ra_log_init()`,
  `ra_stack_canary_check()`.

## ra_hal

The hand-written hardware abstraction layer for every RA8D2 on-chip
peripheral the project drives: GPIO, SCI/UART, IIC, SPI, ADC, DAC,
PWM/GPT, AGT, CANFD, ETHA + ESWM, USB FS/HS, GLCDC, RSIP TRNG, MSTP,
clocks, ICU, watchdog, and a bank of `ra8d2_*_regs.h` register-map
headers. Every driver returns `ra_err_t` and is unit-tested against
`tests/mocks/ra_sim_mmap.c`, which presents the MCU peripheral
address space as host-side RAM.

* Headers: `libs/ra_hal/inc/ra_*.h` and `libs/ra_hal/inc/ra8d2_*_regs.h`
* Main entry points: per-peripheral `ra_<periph>_init()` /
  `_open()` / `_deinit()`.

## ra_board_ek_ra8d2

Board-support pinning and resource-id table for the EK-RA8D2 v1
evaluation kit: LED handles, push-button handles, on-board PMOD
mappings, and the `ra_board_get_info()` discovery call apps use to
key off the exact board variant.

* Header: `libs/ra_board_ek_ra8d2/inc/ra_board_ek_ra8d2.h`
* Main entry points: `ra_board_get_info()`, `ra_board_led_init()`.

## ra_mpu

Cortex-M85 Memory Protection Unit configuration helper. Lets apps
declare an MPU layout as a static `ra_mpu_cfg_t` table at
init-time and have the helper program the regions in the right
order, then enable the MPU.

* Header: `libs/ra_mpu/inc/ra_mpu.h`
* Main entry points: `ra_mpu_configure()`, `ra_mpu_enable()`.

## ra_nsc

TrustZone Non-Secure Callable veneers. Lives in the secure world and
exposes a curated subset of secure-side services (key vault, OTA
commit, register windows, etc.) to the non-secure firmware via the
`__attribute__((cmse_nonsecure_entry))` ABI. There is no public
header per se -- the veneer set is split across
`ra_nsc_{comms,io,key_vault,ota,eth,xspi,log,periph_init}.h`.

* Headers: `libs/ra_nsc/inc/ra_nsc_*.h`
* Main entry points: NSC veneers prefixed `ra_nsc_*`.

## ra_net_pal

Network platform-abstraction layer over the RA8D2 ESWM block. Owns
the DMA descriptor rings, MAC bring-up, and the byte pump up to
the IP-layer handlers in `ra_net`. Stack-agnostic: it is what both
the in-house `ra_net` adapter and the lwIP/NetX-Duo ports build on.

* Header: `libs/ra_net_pal/inc/ra_net_pal.h`
* Main entry points: `ra_net_pal_init()`, `ra_net_pal_set_mac_addr()`.

## ra_net

Tiny lwIP-style TCP/IP adapter on top of `ra_net_pal`. Implements
the ARP cache, IPv4 dispatch (ICMP/UDP/TCP), and a minimal DHCP
client. Designed for the example apps that need basic networking
without the full lwIP/NetX-Duo footprint.

* Header: `libs/ra_net/inc/ra_net.h`
* Main entry points: `ra_net_open()`, `ra_net_poll()`,
  `ra_net_test_inject_frame()` (UNIT_TEST only).

## ra_usb_pal

USB device-mode platform-abstraction layer. Hides the FS-vs-HS
controller choice, the MSTP / clock-gate dance, and per-endpoint
software ring buffers behind a small stack-agnostic API. CherryUSB
binds against this layer; TinyUSB or a hand-rolled stack could
substitute without changing this header.

* Header: `libs/ra_usb_pal/inc/ra_usb_pal.h`
* Main entry points: `ra_usb_pal_init()`, `ra_usb_pal_attach()`,
  `ra_usb_pal_ep_open()`, `ra_usb_pal_ep_send()`,
  `ra_usb_pal_ep_recv()`.

## ra_ble_host

Starter Bluetooth Low Energy host stack -- L2CAP fixed channels, ATT
attribute protocol, GATT services + characteristics -- layered on the
HCI driver in `ra_hal/ra_ble`. Aggressively capped attribute table
(8 services, 32 characteristics) so the entire host fits alongside
the rest of the firmware on the EK-RA8D2.

* Headers: `libs/ra_ble_host/inc/ra_ble_{host,gatt_client,security,mesh}.h`
* Main entry points: `ra_ble_host_init()`,
  `ra_ble_host_gatt_register_service()`, `ra_ble_host_advertise_start()`.

## ra_tls

Thin facade over the vendored Mbed TLS 4.x + TF-PSA-Crypto 1.x stack.
Hands out TLS sessions from a fixed-size static pool (NASA Power-of-10
Rule 3) and translates Mbed TLS error codes into `ra_err_t`. Higher-
level apps (HTTPS client, MQTT/TLS, OTA fetch) call into this facade
instead of pulling Mbed TLS directly.

* Header: `libs/ra_tls/inc/ra_tls.h`
* Main entry points: `ra_tls_global_init()`, `ra_tls_session_open()`,
  `ra_tls_handshake()`, `ra_tls_send()`, `ra_tls_recv()`.

## ra_psa_crypto

Application-level facade over `tf-psa-crypto`. Wraps PSA Crypto API
key-import / sign / verify / AEAD encrypt+decrypt / random calls and
returns `ra_err_t`. Pairs with the secure-side key vault exposed
through `ra_nsc_key_vault`.

* Header: `libs/ra_psa_crypto/inc/ra_psa_crypto.h`
* Main entry points: `ra_psa_crypto_init()`, `ra_psa_key_import()`,
  `ra_psa_aead_encrypt()`, `ra_psa_aead_decrypt()`.

## ra_ota

Phase-5 over-the-air firmware update orchestrator: manifest fetch,
slot management against the dual-bank Octo-SPI flash layout, signed
image verification (delegated to `ra_psa_crypto`), and the secure-
side commit veneer in `ra_nsc_ota`.

* Header: `libs/ra_ota/inc/ra_ota.h`
* Main entry points: `ra_ota_init()`,
  `ra_ota_check_for_update()`.

## ra_modem_at

Cellular modem AT command/response driver layered on top of an
`ra_uart` instance. Owns the line buffer, the response parser, and
the URC dispatcher. Used by the ereader and the OTA-over-cellular
example paths.

* Header: `libs/ra_modem_at/inc/ra_modem_at.h`
* Main entry points: `ra_modem_at_init()`, `ra_modem_at_send_cmd()`,
  `ra_modem_at_poll()`.

## ra_fs

Minimal FAT12/FAT16/FAT32 filesystem adapter (read + write) backed
by a swappable block-device interface. Used by the ereader app to
walk EPUBs off an SD card without pulling in FileX.

* Header: `libs/ra_fs/inc/ra_fs.h`
* Main entry points: `ra_fs_mount()`, `ra_fs_open()`,
  `ra_fs_read()`, `ra_fs_write()`.

## ra_epub

EPUB (.epub) reader and chapter iterator. Walks the ZIP container
through miniz, parses the OPF manifest + NCX spine, and hands
chapter XHTML to `ra_reflow` for layout.

* Header: `libs/ra_epub/inc/ra_epub.h`
* Main entry points: `ra_epub_open()`, `ra_epub_get_chapter_count()`,
  `ra_epub_load_chapter()`.

## ra_reflow

HTML/CSS reflow + paginate engine for the ra8d2 ereader. Wraps
litehtml + gumbo and pages chapter content into the ereader's
viewport so the GLCDC layer can blit one page at a time.

* Header: `libs/ra_reflow/inc/ra_reflow.h`
* Main entry points: `ra_reflow_init()`,
  `ra_reflow_layout_chapter()`.

## ra_gfx

Software 2D graphics primitives for the parallel TFT: framebuffer
clear, pixel/line/rect/circle, bitmap blits, and bundled bitmap
fonts. Sits on top of the GLCDC driver in `ra_hal`.

* Headers: `libs/ra_gfx/inc/ra_gfx{,_font,_text}.h`
* Main entry points: `ra_gfx_clear()`, `ra_gfx_line()`,
  `ra_gfx_text_draw()`.

## ra_touch_cal

Resistive/capacitive touch-screen calibration utility. Runs the
classic 3- or 5-point capture sequence, computes the affine
transform, and applies it to live raw samples.

* Header: `libs/ra_touch_cal/inc/ra_touch_cal.h`
* Main entry points: `ra_touch_cal_run()`, `ra_touch_cal_apply()`.

## ra_power_profile

Power-profiling helper that inserts named region markers into the
firmware so power-rail captures (Joulescope or J-Link energy probe)
can be correlated to firmware activity. Trivially small, but used
across most of the example apps.

* Header: `libs/ra_power_profile/inc/ra_power_profile.h`
* Main entry points: `ra_power_profile_init()`,
  `ra_power_profile_mark_enter()`, `ra_power_profile_mark_exit()`.

## ra_wdt_supervisor

ThreadX-aware watchdog supervisor. Provides a per-thread check-in
registry so a hung thread starves the watchdog refresh and triggers
a reset, instead of one healthy thread keeping the watchdog happy
indefinitely. Falls back to a plain refresh path in non-RTOS apps.

* Header: `libs/ra_wdt_supervisor/inc/ra_wdt_supervisor.h`
* Main entry points: `ra_wdt_supervisor_init()`,
  `ra_wdt_supervisor_register()`, `ra_wdt_supervisor_checkin()`.
