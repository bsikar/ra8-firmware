# tz_nsc_cgc_usb

Runs a full ThreadX + USBX CDC-ACM self-loop **entirely inside the Non-Secure
image** (#60), layered on the Non-Secure-Callable CGC veneer wall. Both USB
controllers are delegated to the NS world and the chip enumerates and echoes
against itself over a J7-to-J11 loop cable -- no PC, no manual cabling.

It is a two-project build (#96): a Secure ELF carrying the secure boot and the
NSC CGC veneers and emitting a CMSE import library, plus a separate Non-Secure
ELF, merged into one flashable hex. NS-to-Secure calls bind to the
Secure-Gateway stubs through that import library.

Two Non-Secure ThreadX threads share the core. USBFS on J11 is the CDC-ACM
**device**, driven by polling `ra8_usb_dispatch`, with Chapter-9 and bulk
auto-echo running inside the dispatch. USBHS on J7 is a polled **host** built on
the first-party `ra8_usb_host_*` primitives: bus reset, GET_DESCRIPTOR,
SET_ADDRESS, SET_CONFIGURATION, open the CDC bulk pipes, then bulk round-trip a
deterministic pattern and byte-check the echo forever. The host's polling
windows are far longer than the thread time-slice, so the device dispatch gets
serviced inside them without any USB interrupt at all.

`g_tz_usb_host_rounds_ok` advances only once the whole chain works -- veneers,
NS ThreadX, NS USBX device bring-up, NS host enumeration over the loop, and a
verified OUT/echo/IN round-trip -- so a climbing counter is end-to-end proof.
`DSCSR.CDS` reading 0 confirms the CPU really is Non-Secure while it happens.

## How both USB controllers reach Non-Secure

1. **The RA8 IDAU is fixed by address bit[28]** (HUM s51.3.3.1): real NS lives at
   the bit[28]=1 aliases, so the NS code reaches USBFS/USBHS at `0x5025_0000` /
   `0x5035_0000` under `-DRA8_PERIPH_NS_ALIAS`. SRAM's secure/NS split, by
   contrast, is the **runtime** `SRAMSABAR` register -- which is why the NS image
   is RAM-resident: no option bytes are touched, so there is no brick risk.
2. **The Secure side delegates both controllers before BLXNS.** It routes the FS
   device pins and the HS host pins (P4_08 USBHS_VBUS, PD07 high for J7 VBUS),
   enables the USBHS UTMI PLL -- CGC is Secure-only, and the 48 MHz USBFS clock
   reaches NS through the NSC CGC veneer -- sets the U15 I/O expander to host
   mode, and marks **PSARB bits 11 and 12** Non-secure under the PRC4 gate (HUM
   51.8.1; the PSARx registers share PRC4 with SRAMSABAR). The NS image then owns
   the USBFS and USBHS registers *and* their `MSTPCRB` module-stop bits.
3. **No USB IRQ.** Both stacks are polled (`-DRA8_USB_POLLED_ONLY`), so the
   Secure-attributed ICU and NVIC are never touched from NS. `ra8_time.c` is
   dropped from the NS link because it would fight ThreadX's SysTick; the NS side
   supplies ThreadX-backed `ra8_delay_ms` / `ra8_time_ms` instead.

## The U15 expander after a warm reset

The expander error is tracked separately from the pin-configuration error for a
reason. On a cold boot the single host-mode I2C write lands; after a
J-Link/SYSRESETREQ warm reset RIIC1's BBSY can still be set and the write
reports `k_ra8_err_busy`. The external PI4IOE latches its host-mode output
regardless, so the USBHS host role persists across the warm reset and the loop
still enumerates -- a busy expander write here is not a failure.
