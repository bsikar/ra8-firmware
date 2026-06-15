# tz_nsc_cgc_usb (hw_validated/hil)

Single-core (CPU0) TrustZone demo that proves the three Non-Secure-Callable
CGC veneers -- `ra_nsc_cgc_pll2_enable`, `ra_nsc_cgc_usbfs_clock_enable`,
`ra_nsc_cgc_get_clock_hz` -- really trap from **genuine Non-Secure state**
into the Secure world via the Secure-Gateway and forward to the underlying
`ra_cgc_*` drivers.

## What it validates

The secure boot (`trustzone_init.c`) programs `SRAMSABAR` + the SAU, copies a
RAM-resident NS image into the SRAM Non-secure alias `0x32100000`, and
`BLXNS`-es into it. The NS image (`ns_main.c`) then calls the three NSC CGC
veneers and, on full success, advances `g_tz_nsc_cgc_usb_match` forever.

On-chip readout while it runs (J-Link halt):

| Signal | Value | Meaning |
|--------|-------|---------|
| `DSCSR.CDS` | `0` | CPU is in **Non-Secure** state |
| `g_tz_nsc_cgc_usb_init_step` | `4` | all three veneers returned `k_ra_ok` |
| `g_tz_nsc_cgc_usb_match` | climbing | success loop running |
| `g_tz_nsc_cgc_usb_mismatch` | `0` | no veneer returned non-OK |
| `g_tz_nsc_cgc_usb_sp_probe` | `0x3217FFD8` | MSP_NS on the NS stack |
| `g_tz_nsc_cgc_usb_clock_hz` | `0x3B9ACA00` | CPUCLK0 = 1 GHz, via the veneer |

## Two things that made it work (see #60)

1. **The RA8 IDAU is fixed by address bit[28]** (HUM s51.3.3.1): bit[28]=0 is
   Secure/NSC and the SAU cannot downgrade it, so an "NS" image at `0x02..` /
   `0x22..` always runs Secure. Real NS lives at the bit[28]=1 aliases. SRAM's
   secure/NS split is the **runtime** `SRAMSABAR` register, so the NS image is
   RAM-resident -- no persistent option bytes, no brick risk.
2. **GNU ld rewrites `cmse_nonsecure_entry` symbol references** (call and
   address-of) onto the secure body `__acle_se_*` inside one secure ELF,
   bypassing the SG veneer and faulting INVEP. `ns_main.c` therefore reaches
   each veneer **by address** (`g_ra_ls_sgstubs_start + slot offset`, via a
   `volatile` function pointer -- the one symbol ld does not rewrite). The slot
   offsets are pinned by `scripts/utils/check_sg_offsets.py`, run POST_BUILD.

## Build + validate

```sh
make tz_nsc_cgc_usb                       # builds; POST_BUILD checks SG slots
bash scripts/hil_run_local.sh tz_nsc_cgc_usb   # flash + HIL gate (local Mac)
```

The HIL gate (`hil.conf`) passes when `g_tz_nsc_cgc_usb_match` advances by at
least 100 across a 3 s window with `g_tz_nsc_cgc_usb_mismatch == 0`.

## Follow-up (optional)

Running ThreadX + USBX *inside* the NS image (issue #55 / #60's original Phase
C title) is a separate enhancement and is not required for this NSC-veneer-wall
validation.
