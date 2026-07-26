# tz_nsc_cgc_usb (hil_needs_revalidation)

Single-core (CPU0) TrustZone demo that runs a **full ThreadX + USBX CDC-ACM
self-loop entirely inside the Non-Secure image** (issue #60's literal title),
layered on the Non-Secure-Callable CGC veneer wall. Both USB controllers are
delegated to the NS world and the chip enumerates + echoes against itself over
the J7<->J11 loop cable -- no PC, no manual cabling.

It is a **two-project build** (#96): a Secure ELF (secure boot + the NSC CGC
veneers, emitting a CMSE import library) and a separate Non-Secure ELF
(`ns_main.c` + `ns_usb.c` + ThreadX + USBX + `ra8_usb`), merged into one
flashable hex. NS->Secure calls bind to the Secure-Gateway stubs through the
import library; the NS image reaches the peripherals through the IDAU bit[28]=1
Non-secure alias.

## What runs inside the NS image

Two time-sliced ThreadX worker threads, both Non-Secure:

- **USBFS (J11) = CDC-ACM DEVICE** -- USBX device stack + the `ux_dcd_ra8_usb`
  bridge, driven by polling `ra8_usb_dispatch` (compiled `RA8_USB_POLLED_ONLY`, so
  it never touches the Secure-attributed USB NVIC line). Chapter-9 + bulk
  auto-echo (OUT pipe 2 -> IN pipe 1) run inside the dispatch.
- **USBHS (J7) = polled HOST** -- the first-party `ra8_usb_host_*` primitives.
  It enumerates the looped device (bus reset -> GET_DESCRIPTOR -> SET_ADDRESS ->
  SET_CONFIGURATION), opens the CDC bulk pipes, then bulk round-trips a
  deterministic pattern and byte-checks the echo, forever.

The host's polling windows (~10 ms) are far longer than the 1 ms time-slice, so
the device dispatch is serviced inside them without any USB interrupt.

## What it validates (HIL gate)

`g_tz_usb_host_rounds_ok` advances (~550/s) ONLY after the whole chain works:
veneers -> NS ThreadX -> NS USBX device bring-up -> NS host enumeration over the
loop -> verified OUT/echo/IN round-trip. A climbing counter is end-to-end proof.

On-chip readout while it runs (J-Link halt):

| Signal | Value | Meaning |
|--------|-------|---------|
| `DSCSR.CDS` | `0` | CPU is in **Non-Secure** state |
| `g_tz_nsc_cgc_usb_init_step` | `4` | all three NSC CGC veneers returned `k_ra8_ok` |
| `g_tz_nsc_cgc_usb_mismatch` | `0` | no veneer returned non-OK |
| `g_tz_usb_pins_err` | `0` | FS+HS pins, J7 VBUS GPIO, USBHS PLL all up |
| `g_tz_usb_psarb_readback` | `0x1800` | USBFS0 (PSARB11) + USBHS (PSARB12) Non-secure |
| `g_tz_usb_state` | `5` | FS device dispatch loop running |
| `g_tz_usb_host_phase` | `4` | HS host enumerated; running echo rounds |
| `g_tz_usb_host_pid` | `0x000A` | idProduct the host read from the looped device |
| `g_tz_usb_host_rounds_ok` | climbing | bulk echo round-trips verified byte-equal |
| `g_tz_usb_host_err` | `0` | no host-ladder error |

## How both USB controllers reach Non-Secure

1. **The RA8 IDAU is fixed by address bit[28]** (HUM s51.3.3.1): real NS lives
   at the bit[28]=1 aliases. SRAM's secure/NS split is the **runtime**
   `SRAMSABAR` register, so the NS image is RAM-resident -- no option bytes, no
   brick risk. `ns_usb.c`/`ra8_usb`/`ra8_mstp` reach USBFS/USBHS through the NS
   aliases `0x5025_0000` / `0x5035_0000` (`-DRA8_PERIPH_NS_ALIAS`).
2. **The Secure side delegates both controllers before BLXNS**
   (`tz_usb_handoff_prepare`): routes the FS device pins + the HS host pins
   (P4_08 USBHS_VBUS, PD07 HIGH for J7 VBUS), enables the USBHS UTMI PLL
   (`ra8_cgc_usbhs_pll_enable` -- CGC is Secure-only; the 48 MHz USBFS clock comes
   from the NS image via the NSC CGC veneer), sets the U15 I/O-expander to host
   mode, and marks **PSARB bits 11 + 12** Non-secure under the PRC4 gate (HUM
   51.8.1; PSARx share PRC4 with SRAMSABAR). The NS image then owns the USBFS /
   USBHS registers *and* their `MSTPCRB.MSTPB11/12` module-stop bits.
3. **No USB IRQ** (`-DRA8_USB_POLLED_ONLY`) -- both stacks are polled, so the
   Secure-attributed ICU/NVIC is never touched from NS. `ra8_time.c` is dropped
   from the NS link (it would fight ThreadX's SysTick); `ns_usb.c` supplies a
   ThreadX-backed `ra8_delay_ms` / `ra8_time_ms`.

## Build + validate

```sh
make tz_nsc_cgc_usb                            # two-project build -> merged hex
bash scripts/hil/run_local.sh tz_nsc_cgc_usb   # flash + HIL gate (local Mac)
```

The HIL gate (`hil.conf`) flashes, dwells while the host enumerates, then passes
when `g_tz_usb_host_rounds_ok` advances by at least 50 across a 3 s window with
`g_tz_nsc_cgc_usb_mismatch == 0`.

## Note: the U15 expander after a warm reset

`g_tz_usb_expander_err` is tracked separately from `g_tz_usb_pins_err`. On a cold
boot the single host-mode I2C write lands; after a J-Link/SYSRESETREQ warm reset
RIIC1's BBSY can still be set and the write reports `k_ra8_err_busy`. The external
PI4IOE latches its host-mode output, so the USBHS host role persists across the
warm reset and the loop still enumerates -- which the gate confirms.
