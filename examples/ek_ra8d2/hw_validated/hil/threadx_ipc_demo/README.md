# threadx_ipc_demo

Inter-Processor Communication (IPC) mailbox demo on the EK-RA8D2.

## What it does

A single ThreadX thread on the Cortex-M85 (CPU0) wakes once a second,
pushes the ASCII tag `"ping"` (a single 32-bit word) into the M85->M33
IPC FIFO, then drains every word currently queued in the M33->M85 FIFO
and prints what it saw over SCI8 at 115200 8N1.

Channel mapping (see `libs/ra8_hal/inc/ra8_ipc.h`):

| direction        | unit | FIFO   | channel id |
|:-----------------|:-----|:-------|-----------:|
| M85 -> M33 (TX)  | IPC1 | FIFO10 | 2          |
| M33 -> M85 (RX)  | IPC0 | FIFO00 | 0          |

The demo resolves both channel ids at runtime via
`ra8_ipc_channel_for_send` / `ra8_ipc_channel_for_recv` so it does not
hard-code the FIFO mapping.

## Mailbox + doorbell sequence

1. `ra8_ipc_init(send_cfg)` -- reset FIFO + clear status on channel 2.
2. `ra8_ipc_init(recv_cfg)` -- reset FIFO + clear status on channel 0
   with the `msg_ready` event unmasked.
3. Thread loop:
   - `ra8_ipc_send_message_retry(2, "ping", 16)` -- writes the word into
     the channel-2 TXD register; the IPC peripheral on the receiving
     core sees `STA.RDY = 1` automatically (the FIFO write is the
     doorbell).
   - `ra8_ipc_recv_message(0, &word)` x N -- pops up to 8 words from
     channel 0; each successful pop is logged as `"<- pong"`.
   - `tx_thread_sleep(1000)`.

## Peer-side firmware (out of scope)

The Cortex-M33 side is not part of this build. To close the loop the
M33 image must:

- Initialise channel 2 (its receive side) and channel 0 (its send side)
  with `ra8_ipc_init`.
- In an IPC ISR or polled loop, read every word arriving on channel 2
  via `ra8_ipc_recv_message` and reply on channel 0 with
  `ra8_ipc_send_message_retry`.

Without that peer the demo still runs cleanly: every iteration prints
`"<no reply>"` because the channel-0 FIFO stays empty.

## Build / flash

```sh
make build           # cross-compile to threadx_ipc_demo.elf / .hex / .bin
make flash           # JLink load via scripts/dev/flash.sh
make ozone           # SEGGER Ozone GUI debugger
make clean
```

A successful run prints, on the host's J-Link OB CDC port at 115200 8N1:

```
[ipc_demo] M85 starting
[ipc_demo] -> ping
[ipc_demo] <no reply>
[ipc_demo] -> ping
[ipc_demo] <- pong
...
```

## Layout

```
threadx_ipc_demo/
  main.c                 -- M85 thread + ipc_demo bring-up
  vector_table.c         -- per-app vector table
  system_init.c          -- per-app SystemInit
  secure_exception.c     -- per-app secure-fault handler
  trustzone_init.{c,h}   -- per-app SAU bring-up
  linker_script.ld       -- per-app memory map
  CMakeLists.txt         -- per-app cmake target
  Makefile               -- standalone make wrapper
  README.md              -- this file
```

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED init/toggle on the M85 side (per
UM Table 24 p 31). The IPC peripheral is on-chip and has no board-side
pin assignments; the M33 partner is out of scope for this build (no
second-core image).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 13 p 24 + Table 24 p 31, and HUM (R01UH1065EJ0130) Ch
"IPC" / "MHU".
