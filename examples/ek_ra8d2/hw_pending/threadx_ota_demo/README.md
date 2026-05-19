# threadx_ota_demo

Eclipse ThreadX OTA-orchestration heartbeat demo for the EK-RA8D2.
A single ThreadX worker walks an idealised OTA pipeline -- check ->
download -> verify -> commit -> idle -- using LED1 as a fast
heartbeat and LED2 as the per-state pulse.

## What it does

| LED  | Cadence                | Meaning                       |
|------|------------------------|-------------------------------|
| LED1 | 250 ms (4 toggles/step)| Heartbeat -- worker is alive  |
| LED2 | once per state         | State transition has occurred |

The worker thread (`ota_worker`, prio 5, 2 KiB stack) loops:

1. Toggle LED1 four times at 250 ms each (one full step = 1 s).
2. Toggle LED2 once to mark the state transition.
3. Repeat.

`SysTick_Handler` is overridden to forward into `_tx_timer_interrupt`
for the kernel's 1 kHz tick.

The real OTA orchestration lives in `libs/ra_ota`; this example just
wires a watched thread + LED heartbeat around it so the next
integration step has somewhere to drop in the actual download /
verify / commit calls.

## Build + flash

From the repo root:

```sh
make threadx_ota_demo
bash scripts/flash.sh build/threadx_ota_demo/threadx_ota_demo.hex
```

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 / LED2 init + toggle (per
EK-RA8D2 v1 UM Table 24 "EK-RA8D2 Board LED Functions" p 31).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual
(R20UT5523EG0101 Rev 1.01) Table 24 p 31, and HUM (R01UH1065EJ0130)
Ch "MRAM" + Ch "SysTick".

## HIL plan

**HIL-able after firmware work -- network download path is a stub.**
The OTA pipeline (`check -> download -> verify -> commit`) currently
walks an idealised state machine with no real HTTP / TFTP client.
There is no real artifact being downloaded, so any "OTA OK" banner
would be vacuous.

Once a real download path lands (HTTP via lwIP or TFTP), the demo
could emit a per-state UART banner and a `uart_scrape` config could
walk the state sequence:

```
HIL_MODE=uart_scrape
HIL_EXPECT="ota: commit ok"
HIL_EXPECT_NEGATIVE="ota.*FAIL|HardFault"
HIL_TIMEOUT_S=30
```

Stays in `hw_pending/` until the download stub is replaced with a
real network client.
