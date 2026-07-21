# threadx_canfd_demo

Eclipse ThreadX heartbeat / RX-blink demo wired around the RA8D2's
CANFD0 channel. Two cooperating threads exercise the `ra8_canfd` HAL
under the kernel: a TX thread blinks LED1 at the heartbeat cadence and
an RX poller thread blinks LED2 at the receive-poll cadence.

## What it does

| Thread     | Priority | Period            | Visible effect      |
|------------|----------|-------------------|---------------------|
| `canfd_tx` | 4        | 500 ms            | LED1 toggles per heartbeat |
| `canfd_rx` | 4        | 50 ms (poll loop) | LED2 toggles per poll       |

The TX thread targets standard 11-bit CAN ID `0x123` with DLC 8; the
RX thread polls the channel and toggles LED2 each iteration. Both
threads use static 1 KiB stacks (NASA Power of 10 Rule 3 -- no
dynamic allocation). `SysTick_Handler` is overridden to forward into
`_tx_timer_interrupt` so the kernel's 1 kHz tick is sourced from
SysTick.

The example exists to prove the `ra8_canfd` HAL driver wires up
cleanly under ThreadX -- it is intentionally minimal and is not a
full CAN stack.

## Build + flash

From the repo root:

```sh
make threadx_canfd_demo
bash scripts/dev/flash.sh build/threadx_canfd_demo/threadx_canfd_demo.hex
```

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 / LED2 init + toggle (per
EK-RA8D2 v1 UM Table 24 "EK-RA8D2 Board LED Functions" p 31).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual
(R20UT5523EG0101 Rev 1.01) Table 24 p 31, and HUM (R01UH1065EJ0130)
Ch "CAN with Flexible Data Rate (CANFD)" + Ch "SysTick".
