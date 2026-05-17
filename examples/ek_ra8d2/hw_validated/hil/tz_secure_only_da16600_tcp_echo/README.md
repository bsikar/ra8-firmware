# tz_secure_only_da16600_tcp_echo

Wi-Fi TCP-echo demo for the Renesas DA16600 module on a
US159-DA16600EVZ Pmod daughter card plugged into Pmod1 (J26) of the
EK-RA8D2 evaluation kit.

## What it does

1. Bring up CGC + SysTick + J-Link OB VCOM console + SCI2 to the
   DA16600 (Pmod1 UART, P801/P802).
2. Initialize the `ra_da16600` driver.
3. Associate with the AP defined at compile time
   (`DA16600_SSID` / `DA16600_PASS`, defaulting to `hil_lab` /
   `test1234`) using `AT+WFJAP` (UM-WI-046 section 4.5).
4. Emit `wifi: ip=A.B.C.D` on the VCOM channel so the harness knows
   where to send echo probes.
5. Open a TCP listening socket on port 7 (RFC 862 echo) via
   `AT+TRTS` (UM-WI-046 section 5.2.3).
6. Block on inbound payloads (`+TRDTC` URC, UM-WI-046 5.2.6) and
   echo them back using `AT+TRDTS` (UM-WI-046 5.2.5).

## Overrides

To target a different AP:

```sh
make tz_secure_only_da16600_tcp_echo \
  EXTRA_CFLAGS='-DDA16600_SSID="\"mywifi\"" -DDA16600_PASS="\"secret\""'
```

## HIL contract

`hil.conf` declares `HIL_MODE=alive` while the Pi-as-AP lab fixture
is being built out -- the firmware just needs to boot and stay
alive long enough for the DA16600 init / Wi-Fi association attempt
to run. Once the Pi-AP exists, the conf will be flipped to
`uart_scrape` matching `wifi: ip=` with a 20 s timeout.

Hardware wiring: see `scripts/hil_da16600_setup.md`.
