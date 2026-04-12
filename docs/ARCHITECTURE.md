# ra8d2-firmware Architecture

This document is the source of truth for *where every file lives* in
this project. The older `docs/architecture.md` covers boot flow,
clock tree, and per-driver mechanics; this file covers the layered
ring model, the TrustZone world split, and the decision rule that
tells you where a brand-new file belongs.

If you are about to add a new file and you cannot answer the
"which ring + which world" question for it, stop and answer that
question first. Every new file in this project carries a
`[Ring N / NAME] {World: S | NS | NSC}` tag pair in its file
header (Ring 3 and above; rings 1 and 2 are Secure-only and use
just `[Ring N / NAME]`).

## The six concentric rings

The codebase is organised as six concentric rings. Outer rings call
inner rings. Inner rings know nothing about outer rings.

```
                              +---------------------+
                              |  Ring 6 Application |   src/main.c, demos
                              +---------------------+
                              |  Ring 5 Middleware  |   libs/third_party/lwip
                              |                     |   libs/third_party/cherryusb
                              +---------------------+
                              |  Ring 4 PAL         |   libs/ra_net_pal
                              |                     |   libs/ra_usb_pal
                              |                     |   libs/ra_nsc (veneers)
                              +---------------------+
                              |  Ring 3 HAL         |   libs/ra_hal
                              +---------------------+
                              |  Ring 2 Core        |   libs/ra_core
                              +---------------------+
                              |  Ring 1 BSP         |   src/boot, src/linker_script.ld
                              +---------------------+
                              |       silicon       |   Renesas R7KA8D2KFLCAC
                              +---------------------+
```

| Ring | Name        | What lives here                                        | Examples                                                              |
|-----:|:------------|:-------------------------------------------------------|:----------------------------------------------------------------------|
|    1 | BSP         | Board + chip bring-up. Linker script, vector table,    | `src/boot/system_init.c`, `src/boot/vector_table.c`,                  |
|      |             | reset handler, cache + MPU init, exception handlers,   | `src/linker_script.ld`, `cmake/toolchain-ra8d2.cmake`                 |
|      |             | clock tree bring-up.                                   |                                                                       |
|    2 | Core        | Project-wide vocabulary with no peripheral knowledge.  | `libs/ra_core/inc/ra_err.h`, `ra_log.h`, `ra_check.h`,                |
|      |             | Errors, logging, asserts, time, watchdog primitives,   | `ra_time.h`, `ra_register_protection.h`                               |
|      |             | pin validator, register guards.                        |                                                                       |
|    3 | HAL         | Per-peripheral drivers. Every register write lives     | `libs/ra_hal/src/ra_sci.c`, `ra_iic.c`, `ra_spi.c`, `ra_gpt.c`,       |
|      |             | here. The Wave 1 substrate (`ra_isr`, `ra_mstp`,       | `ra_xspi.c`, `ra_eth_*.c`, `ra_usb_fs.c`, `ra_glcdc.c`                |
|      |             | `ra_mpc`, `ra_pwr`, `ra_dma`, `ra_hw_err`) is also     |                                                                       |
|      |             | Ring 3.                                                |                                                                       |
|    4 | PAL         | Hand-written port glue that lets a Ring-5 library      | `libs/ra_usb_pal/src/usb_dc_ra8d2_fs.c`,                              |
|      |             | call down into our HAL without knowing the chip        | `libs/ra_net_pal/src/ra_net_ethernetif.c`,                            |
|      |             | exists. NSC veneers (Ring 4 / World NSC) live in       | `libs/ra_nsc/src/ra_nsc_xspi.c`                                       |
|      |             | `libs/ra_nsc/`.                                        |                                                                       |
|    5 | Middleware  | Vendored protocol stacks, file systems, chip-agnostic  | `libs/third_party/lwip/`,                                             |
|      |             | libraries. Strictly limited (see Vendoring policy).    | `libs/third_party/cherryusb/`                                         |
|    6 | Application | `src/main.c`, demos, control logic. The only ring      | `src/main.c`, future `src/secure_app/key_vault.c`                     |
|      |             | the user touches directly.                             |                                                                       |

The *legacy* `docs/architecture.md` ring diagram (drivers / register
headers / `ra_core` / boot) is the same model in coarser strokes.
This file is what `cite_check.py` and `check_world_tags.py` enforce.

## Dependency rule

> A file in Ring N may `#include` headers from Rings 1..N. It MUST
> NOT `#include` anything from Ring N+1 or higher.

Concretely:

- The HAL (`libs/ra_hal/`) does not know `main.c` exists.
- The Core (`libs/ra_core/`) does not know the HAL exists.
- The PAL (`libs/ra_*_pal/`) is the only ring that faces both
  directions: it `#include`s the HAL below and implements an
  interface the Middleware above calls into.
- The Middleware (`libs/third_party/`) talks only to its PAL, not
  to `ra_hal` or `ra_core` directly.
- The Application talks to `ra_core` + `ra_hal` directly and to
  Middleware through the PAL header.

The long-term stretch goal is to teach `scripts/utils/cite_check.py`
to walk the include graph and reject inward violations. That work
is earmarked for late Wave 0; until it lands, the rule is enforced
by code review.

### Security clause (TrustZone, applies from Wave 9 forward)

> Non-Secure code may only call Secure code through NSC veneers
> in `libs/ra_nsc/`. Direct NS -> S calls (without going through
> an SG instruction) are rejected at link time.

The build system enforces this via separate compilation units and
the `-mcmse` flag in `cmake/toolchain-ra8d2.cmake`. The
`scripts/utils/check_world_tags.py` hook rejects any
`__attribute__((cmse_nonsecure_entry))` outside `libs/ra_nsc/`.

## The world matrix (rings x worlds)

Every Ring 3+ file declares both a ring and a TrustZone world. The
two axes are *orthogonal*. The matrix below states the default
world for files in each ring; per-file overrides are documented in
the file header.

```
                  +-------------------+-------------------+-------------------+
                  |    Secure (S)     |  Non-Secure (NS)  |       NSC         |
+-----------------+-------------------+-------------------+-------------------+
| Ring 1 BSP      | DEFAULT (only)    |        --         |        --         |
|                 | system_init,      |                   |                   |
|                 | linker, cache,    |                   |                   |
|                 | MPU, vector table |                   |                   |
+-----------------+-------------------+-------------------+-------------------+
| Ring 2 Core     | DEFAULT (only)    |        --         |        --         |
|                 | err, log, check,  |                   |                   |
|                 | time, watchdog    |                   |                   |
|                 | primitives        |                   |                   |
+-----------------+-------------------+-------------------+-------------------+
| Ring 3 HAL      | substrate:        | per-peripheral    |        --         |
|                 | ra_isr, ra_mstp,  | drivers default:  |                   |
|                 | ra_mpc, ra_pwr,   | sci, iic, spi,    |                   |
|                 | ra_dma, ra_hw_err | gpt, adc, crc,    |                   |
|                 | xspi, sdramc,     | usb, eth, glcdc,  |                   |
|                 | rtc, wdt, iwdt    | dac_b, acmphs,    |                   |
|                 |                   | mtu, tpu, pdm,    |                   |
|                 |                   | ulpt, agt, cac,   |                   |
|                 |                   | canfd, sdhi, i3c  |                   |
+-----------------+-------------------+-------------------+-------------------+
| Ring 4 PAL      |        --         | DEFAULT           | NSC veneers ONLY  |
|                 |                   | ra_usb_pal,       | libs/ra_nsc/      |
|                 |                   | ra_net_pal        |                   |
+-----------------+-------------------+-------------------+-------------------+
| Ring 5 Middleware|       --         | DEFAULT (only)    |        --         |
|                 |                   | lwip, cherryusb   |                   |
+-----------------+-------------------+-------------------+-------------------+
| Ring 6 App      | secure-side app   | DEFAULT           |        --         |
|                 | (key handling,    | src/main.c,       |                   |
|                 | secure boot logic)| demos             |                   |
+-----------------+-------------------+-------------------+-------------------+
```

Default tagging rules in prose form:

- **Ring 1 (BSP)** -- Secure ONLY. SAU/IDAU/MPC setup, clock tree,
  vector table, exception handlers. Establishes the trust boundary.
- **Ring 2 (Core)** -- Secure ONLY. Errors, logging, time,
  watchdog primitives. Project-wide vocabulary stays in the
  trusted world.
- **Ring 3 (HAL)** -- MIXED. Substrate (`ra_isr`, `ra_mstp`,
  `ra_mpc`, `ra_pwr`, `ra_dma`, `ra_hw_err`) is Secure ONLY because
  it controls chip-wide configuration. Per-peripheral drivers
  default to Non-Secure but may be promoted to Secure if they
  control global state (`ra_xspi` for boot images, `ra_rtc` for
  battery-backed calendar + key provisioning, watchdogs).
- **Ring 4 (PAL)** -- Non-Secure preferred. CherryUSB and lwIP
  PALs run in NS so the middleware above them runs in NS. PALs
  reach Secure HAL functionality only through NSC veneers.
- **Ring 5 (Middleware)** -- Non-Secure ONLY. Vendored libraries
  run in NS. They never see Secure memory or Secure peripherals
  directly.
- **Ring 6 (Application)** -- Non-Secure preferred. `src/main.c`
  and demos run in NS. Secure-side application code (key handling,
  secure-boot logic) is the explicit exception, marked
  `{World: S}` in its file header.

Files written before Wave 9 are tentatively tagged with their
intended Wave-9 world in the file header. They actually compile
and run entirely in Secure world until Wave 9.1 flips the
partitioning on. Tags that turn out wrong by Wave 9 are corrected
in the same commit that retrofits the driver.

## Memory partitioning (Wave 9 onward)

```
        MRAM (1 MB code memory)
        +------------------------------+ 0x10000000  +------+
        |    Secure region             |             |  S   |
        |    BSP, Core, Secure HAL,    |             |      |
        |    NSC veneers (.gnu.sgstubs)|             |      |
        +------------------------------+             |      |
        |    Non-Secure region         |             |      |
        |    NS HAL, PALs, Middleware, |             |  NS  |
        |    Application               |             |      |
        +------------------------------+ 0x100FFFFF  +------+

        SRAM (2 MB with ECC)
        +------------------------------+ 0x22000000  +------+
        |    SRAM-0  Secure data       |             |  S   |
        +------------------------------+             +------+
        |    SRAM-1  Non-Secure data   |             |  NS  |
        +------------------------------+             +------+
```

Specific addresses come from HUM Ch 5 "Address Space" + Ch 16
"Memory Protection Unit (MPU)" + Ch 58 "SRAM" + Ch 59 "MRAM". See
`docs/reference/CHAPTER_MAP.md` for the full security-relevant
chapter index.

## Peripheral partitioning (Wave 9 onward)

The RA8D2 carries per-peripheral security attribution registers
(`xxxSAR`) that route bus accesses to either world. Default rule:

- **Secure** -- peripherals controlling chip-wide state: CGC,
  MSTP, ICU, MPC, DMAC, DTC, MPU, OSPI (boot image store), reset
  controller (RSTSAR), PVD (PVDSAR), IPC (IPCSAR / IPCPAR).
- **Non-Secure** -- peripherals with isolated effect: SCI, IIC,
  SPI, GPT, ADC, CRC, USB, ETH, GLCDC, MTU, TPU, PDM, AGT, ULPT,
  CAC, CANFD, SDHI, I3C.
- **Secure-callable from NS** -- where NS code legitimately needs
  a Secure-only resource (e.g. xSPI flash read for the MSC backend),
  it goes through an NSC veneer in `libs/ra_nsc/`.

## Where does this new file go? (decision flowchart)

```
                     +-------------------------+
                     |  I have a new file to   |
                     |  add. Where does it go? |
                     +-----------+-------------+
                                 |
                                 v
                  +---------------------------------+
                  | Does it write to MMIO registers?|
                  +-------+----------------+--------+
                          | yes            | no
                          v                v
                 +----------------+   +----------------------+
                 | Is it chip-wide|   | Is it project-wide   |
                 | substrate (CGC,|   | vocabulary that has  |
                 | MSTP, ICU, MPC,|   | NO peripheral        |
                 | DMA, MPU, pwr)?|   | knowledge (errors,   |
                 +----+----------++   | logging, time)?      |
                      |yes       |no  +------+--------+------+
                      v          v           |yes     |no
              +-------------+   +---------+  v        v
              | Ring 3 HAL  |   | Ring 3  | +------+ +-------+
              | substrate   |   | HAL     | | Ring | | not   |
              | {World: S}  |   | per-    | | 2    | | Core  |
              | libs/ra_hal/|   | periph  | | Core | | code  |
              +-------------+   | default:| +------+ +---+---+
                                | NS, but |               |
                                | promote |               v
                                | to S if |    +---------------------+
                                | global  |    | Is it a vendored    |
                                +---------+    | library (lwIP,      |
                                               | CherryUSB)?         |
                                               +-----+----------+----+
                                                     |yes       |no
                                                     v          v
                                              +-----------+   +---------+
                                              | Ring 5    |   | Is it   |
                                              | Middleware|   | port    |
                                              | libs/     |   | glue    |
                                              | third_    |   | between |
                                              | party/    |   | a Ring-5|
                                              | {World:NS}|   | lib and |
                                              +-----------+   | the HAL?|
                                                              +----+----+
                                                                   |yes,
                                                                   v
                                                          +-----------------+
                                                          | Ring 4 PAL      |
                                                          | libs/ra_*_pal/  |
                                                          | {World: NS}     |
                                                          | (NSC veneers go |
                                                          | in libs/ra_nsc/ |
                                                          | with            |
                                                          | {World: NSC})   |
                                                          +-----------------+
                                                                  ^ no
                                                                  |
                                                          +---------------+
                                                          | Is it boot,   |
                                                          | reset, vector |
                                                          | table, linker,|
                                                          | clock tree,   |
                                                          | cache, MPU?   |
                                                          +-+---------+---+
                                                            |yes      |no
                                                            v         v
                                                      +--------+   +--------+
                                                      | Ring 1 |   | Ring 6 |
                                                      | BSP    |   | App    |
                                                      | src/   |   | src/   |
                                                      | boot/  |   | main.c,|
                                                      |{World:S|   | demos  |
                                                      |}       |   |{NS by  |
                                                      +--------+   | default|
                                                                   | S only |
                                                                   | for    |
                                                                   | secure-|
                                                                   | side   |
                                                                   | app}   |
                                                                   +--------+
```

If the answer is "I'm not sure which ring", it almost always
means the file should not exist yet -- the right thing is to
extend an existing file in an existing ring instead of carving a
new ring slot.

If the answer is "I need a Ring 4 PAL but there's no Ring 5
consumer for it yet", stop. The vendoring policy forbids adding a
PAL without a sanctioned middleware that needs it. Take the work
back to the HAL or move it forward in the wave plan.

## Where the build splits Secure and Non-Secure (Wave 9+)

From Wave 9 forward, the build emits two ELF images:

```
        +-----------------+         +-----------------+
        | secure.elf      |         | nonsecure.elf   |
        |                 |         |                 |
        |  Ring 1 BSP     |         |  Ring 6 App     |
        |  Ring 2 Core    |         |  Ring 5 Mware   |
        |  Ring 3 substr  |         |  Ring 4 PAL     |
        |  Ring 3 secure  |         |  Ring 3 NS HAL  |
        |  drivers        |         |  drivers        |
        |  Ring 4 NSC     |         |                 |
        |  veneers        |         |                 |
        +-----------------+         +-----------------+
              | secure                     | non-secure
              | reset entry                | entry from secure
              v                            v
                 silicon (R7KA8D2KFLCAC)
```

Single-image-with-partitions vs. two-ELF is a Wave 9.1 decision.
Whichever option is taken is recorded as a paragraph in this file
at that time. Until then, the project ships a single Secure-only
ELF and the world tags are advisory.

## Useful pointers

- `docs/architecture.md` -- legacy boot / clock / driver-mechanic walkthrough
- `docs/ROADMAP.md` -- per-driver progress, single source of truth
- `docs/reference/CHAPTER_MAP.md` -- HUM page-range index, includes a Security/TrustZone section
- `scripts/utils/cite_check.py` -- validates HUM citations against `CHAPTER_MAP.md`
- `scripts/utils/check_world_tags.py` -- validates `{World: ...}` tags on Ring 3+ files
- `scripts/utils/roadmap_stats.py` -- rewrites the ROADMAP summary block deterministically
