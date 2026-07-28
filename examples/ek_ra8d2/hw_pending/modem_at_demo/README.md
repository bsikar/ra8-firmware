# modem_at_demo -- cellular AT modem bring-up over the MikroBUS UART (#259)

Drives the `libs/ra8_modem_at` cellular AT command/response driver against a
3GPP AT-command modem (SIM7600 / Quectel BG95 class) on the EK-RA8D2 MikroBUS
UART -- **RXD7 / TXD7 = SCI channel 7**
(`k_ra8_board_mikrobus_uart_sci_channel`). A MikroE cellular Click (LTE IoT /
4G LTE / NB-IoT) presents its modem UART on exactly these MikroBUS pads.

## What it does

Brings SCI7 up (8N1 at 115200), binds it to `ra8_modem_at`, registers a `+CREG`
URC handler, and walks a small state machine:

| Step | Command(s)                | Parsed result                         |
|------|---------------------------|---------------------------------------|
| sync | `AT` / `ATE0` / `AT+CMEE=1` | probe, disable echo, numeric CME errors |
| SIM  | `AT+CPIN?`                | `+CPIN: READY`                         |
| signal | `AT+CSQ`                | RSSI (0..31, 99 = unknown)             |
| registration | `AT+CREG=1`, poll URC, `AT+CREG?` | status (1 home / 5 roaming) |
| attach | `AT+CGATT?`             | PS-attach flag                         |
| error path | an unsupported command | modem replies `+CME ERROR: 4` -> the driver surfaces `k_ra8_err_hw_error`, which this step treats as the **expected** outcome |

On success it prints (SCI8 J-Link OB VCOM console):

```
modem: rssi=17 reg=1 attach=1 cme=ok PASS
```

On any unexpected step failure it prints `modem: <step> FAIL`, latches LED2 and
parks. It uses the central `ra8_board_*` console + MikroBUS pin helpers -- no
hand-encoded pins.

## Boot files

Like its sibling `epaper_refresh`, this app carries no per-app boot files or
linker script: `cmake/ra8_add_app.cmake` supplies the shared
`libs/ra8_board_ek_ra8d2/boot/*` startup + the canonical single-core linker
script. Only `main.c` is app-specific.

## Build

```
make            # -> build/modem_at_demo.elf / .hex / .bin
make flash      # JLinkExe load via scripts/dev/flash.sh
make clean
```

## Hardware status: hw_pending

A **live cellular modem on the wire is external hardware** (a populated
MikroBUS cellular Click with a provisioned SIM and antenna), so this app lives
under `hw_pending/`. The **AT protocol itself is faithfully modelled** in
ra8_emulator, so the example boots and runs its whole state machine with zero
skips in ra8_emulator:

```
tools/ra8_emulator/build/ra8_emulator build/modem_at_demo.elf --modem
```

`--modem` attaches the SCI7 AT-responder model
(`tools/ra8_emulator/src/periph/board_periph_modem.c`), which answers the exact AT script
above -- including a `+CREG` URC on `AT+CREG=1` and a `+CME ERROR: 4` for the
unsupported command -- so `ra8_modem_at -> ra8_sci` runs byte-for-byte as on
silicon (EIL == HIL for the AT protocol; only the RF link is unmodelled).
`scripts/emu/smoke.sh modem_at_demo` gates it to the PASS banner.

The compound decisions (registration OK, signal valid, per-step expected
outcome, overall verdict) are host-tested with MC/DC in
`tests/test_app_modem_at_demo.c`.

## TODO(real modem)

Validate on a bench EK-RA8D2 with a real cellular Click + SIM: confirm the RF
registration, signal strength and PS attach against a live network, and pin the
observed `+CSQ` / `+CREG` / `+CGATT` values. The firmware AT path is proven; the
antenna is the only unproven link.
