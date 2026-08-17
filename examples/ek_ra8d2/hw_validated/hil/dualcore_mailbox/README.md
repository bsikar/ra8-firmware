# dualcore_mailbox -- two images, two cores, one mailbox

A minimal, teaching-focused example that does two things this repo's other
examples do not do together:

1. **Two images.** The firmware is built as **two independently compiled
   images** -- one for each CPU core -- and stitched into a single `.hex`:
   - `main.c`      -> the **Cortex-M85** primary-core image (`-mcpu=cortex-m85`)
   - `cpu1_main.c` -> the **Cortex-M33** secondary-core image (`-mcpu=cortex-m33`)

   The M33 image is objcopy'd into a `.cpu1_image` blob and linked into the M85
   ELF (see `CMakeLists.txt`), pinned at `MRAM_CPU1` (`0x020C0000`) by
   `linker_script.ld`. One SWD flash of the M85 `.hex` therefore drops *both*
   cores' code into MRAM.

2. **Both cores running at once.** At runtime the M85 **releases** the M33 and
   the two cores exchange messages through a shared-SRAM mailbox.

> If you want the *Secure + Non-Secure* flavour of "two images" (TrustZone),
> see `apps/stand_alone/ereader` -- that splits one core into a Secure and a
> Non-Secure image. This example is about the *other* axis: two **cores**.

## How it works

```
   Cortex-M85 (primary, 1 GHz)              Cortex-M33 (secondary, 250 MHz)
   --------------------------               -------------------------------
   ra8_cpu1_release(entry, sp)  ----------->  boots from .cpu1_vectors
        |                                         |
        |  writes request, request_seq++          |  stamps m33_signature = 33
        |          (shared SRAM @ 0x22100000)      |
        |  ------------------------------------->  |  reads request
        |                                         |  reply = request*3 + 1
        |  <-------------------------------------  |  reply_seq = request_seq
        |  reads + verifies reply                  |
        v                                         v
```

- **Release.** `ra8_cpu1_release()` writes the M33's vector-table base to
  `CPU1INITVTOR` and asserts `CPU1ACTCSR.ACTREQ` to bring the second core out
  of power-gating (HUM Ch 2.9.1 "CPU control registers", p 128-130). The M33
  then fetches its reset vector and runs `cpu1_reset_handler`.
- **Mailbox.** The two cores have no shared cache, but they do share on-chip
  SRAM. `dualcore_mailbox.h` pins a small `volatile` struct at `0x22100000`
  (the start of SRAM2), which both linker scripts leave unclaimed, so the same
  physical bytes back the struct on both sides. The M85 data cache is left off
  in `system_init.c`, so a `dsb` after each write is all the coherency needed.
- **Proof of life.** Each round the M85 sends an operand `n` and the M33 sends
  back `n*3 + 1`. That tiny computation matters: a correct reply can only have
  been produced by the M33 actually executing code, so it is honest evidence
  the second core is alive -- not the M85 reading back its own write.

## Running it (no hardware needed)

```sh
make emu-dualcore_mailbox
```

This cross-builds the app **Debug** (so the log lines are compiled in), builds
the `ra8_emulator` emulator, and boots the M85 ELF. ra8_emulator sees the embedded
`.cpu1_image` and automatically spins up a **second Unicorn engine for the
M33**, sharing the SRAM buffer with the M85 engine -- so both cores really run.

### What `[itm]` means

`ra8_log_info(...)` writes bytes to the Arm CoreSight **ITM** (Instrumented
Trace Macrocell) stimulus port. On real hardware those bytes leave through the
SWO pin to the J-Link SWO trace console. In the emulator, ra8_emulator echoes them
to your terminal prefixed with `[itm]`. So **`[itm]` == "what you'd see on the
SWO trace console on the bench."**

### Expected output

```
  cpu1 engine   : Cortex-M33, shared SRAM (dual-core)
[itm] [M85] INFO: ==== RA8D2 dual-core mailbox demo ====
[itm] [M85] INFO: releasing Cortex-M33 secondary core ...
[itm] [M85] INFO: ra8_cpu1_release rc (0 = ok)=0
[itm] [M85] INFO: M33 is up; boot signature=33
[itm] [M85] INFO: -- exchanging messages with the M33 --
[itm] [M85] INFO:   -> sent operand to M33=1
[itm] [M85] INFO:   <- M33 replied (3n+1)=4
...
[itm] [M85] INFO:   -> sent operand to M33=6
[itm] [M85] INFO:   <- M33 replied (3n+1)=19
[itm] [M85] INFO: demo done; total M33 replies=6
[itm] [M85] INFO: both cores confirmed alive -- idle heartbeat follows
[itm] [M85] INFO: heartbeat: both cores alive, M33 replies=7
[itm] [M85] INFO: heartbeat: both cores alive, M33 replies=8
...
```

### Why only the M85 prints

ra8_emulator echoes the **primary** core's ITM only. Each core has its own ITM on
hardware, but the emulator does not wire the M33 engine's ITM to the console,
so an `ra8_log` call on the M33 would be invisible here. Rather than print lines
you cannot see, the M33 stays silent and the **M85 narrates the M33's mailbox
replies on its behalf** -- and since those values (`signature=33`, `3n+1`, the
climbing reply count) can only come from the M33, the log is an honest account
of both cores in both the emulator and on silicon.

## Building / flashing

```sh
make dualcore_mailbox          # cross-build (RelWithDebInfo; logs compiled out)
make flash-dualcore_mailbox    # flash the combined .hex (both cores) via J-Link
```

A default (RelWithDebInfo) build still runs the full dual-core exchange; it is
just silent because `ra8_log_info` is gated out below INFO level. Use the
`emu-` target (or a Debug build) to see the `[itm]` lines.

## Status

- **Emulator: validated.** Both cores run and the mailbox handshake completes
  under `ra8_emulator` (see the output above).
- **Hardware: pending.** This example has **not** yet been bench-validated on a
  physical EK-RA8D2, which is why it lives under `hw_pending/`. The dual-core
  release sequence it uses is the same one as the bench-validated
  `cpu1_pingpong` HIL test, but treat on-board bring-up as unproven until run.

## Files

| File                    | Role                                              |
|-------------------------|---------------------------------------------------|
| `main.c`                | M85 primary-core driver (release + mailbox + log) |
| `cpu1_main.c`           | M33 secondary-core responder image                |
| `dualcore_mailbox.h`    | Shared-SRAM mailbox layout + protocol constants   |
| `linker_script.ld`      | M85 memory map; pins `.cpu1_image` at MRAM_CPU1   |
| `linker_script_cpu1.ld` | M33 memory map (MRAM_CPU1 + SRAM_CPU1)             |
| `system_init.c`         | M85 core bring-up (D-cache off; no TrustZone)     |
| `vector_table.c`        | M85 vector table + `Reset_Handler`                |
| `trustzone_init.c`      | SAU bring-up (present but not invoked here)        |
| `CMakeLists.txt`        | Builds both images; embeds the M33 into the M85   |
| `Makefile`              | Per-app build / flash / debug wrapper             |
