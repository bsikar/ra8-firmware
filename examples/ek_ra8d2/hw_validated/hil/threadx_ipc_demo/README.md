# threadx_ipc_demo

Inter-processor mailbox demo, M85 side. A ThreadX thread wakes once a second,
pushes a single 32-bit word into the M85 -> M33 IPC FIFO, drains whatever is
queued in the M33 -> M85 direction, and prints what it saw.

| direction       | unit | FIFO   | channel |
|:----------------|:-----|:-------|--------:|
| M85 -> M33 (TX) | IPC1 | FIFO10 |       2 |
| M33 -> M85 (RX) | IPC0 | FIFO00 |       0 |

The app resolves both channel ids at runtime via `ra8_ipc_channel_for_send` /
`ra8_ipc_channel_for_recv` rather than hard-coding that mapping.

There is no separate doorbell to ring: the FIFO write *is* the doorbell, and
the receiving core's IPC peripheral raises `STA.RDY` on its own.

**The Cortex-M33 half is not part of this build.** Without a peer image every
iteration reports no reply, which is a clean run and not a failure. To close
the loop, an M33 image has to initialise channel 2 as its receive side and
channel 0 as its send side, then echo each arriving word back.

The IPC peripheral is on-chip and has no board pin assignments (HUM
R01UH1065EJ0130 Ch "IPC" / "MHU"). LEDs per EK-RA8D2 v1 UM Table 24 p 31.
