# c6_wifi_link -- station bring-up through the `ra8_c6link` facade

Takes the ESP32-C6's Wi-Fi station all the way up using
[`libs/ra8_c6link`](../../../../../libs/ra8_c6link/), the single integration
boundary application code is meant to use, and reads the station's MAC address
back. It is the first application on this board that speaks esp-hosted without
building the protocol itself.

```
make c6_wifi_link
make hil-c6 APP=c6_wifi_link
```

## What it proves that the other c6 apps do not

| App | Proves |
|---|---|
| `c6_spi_probe` | the wire: which J26 hole is which net, at mode 3 / 1 MHz |
| `c6_hosted_init` | the port: a framed transaction through the OS-abstraction vtable |
| `c6_fw_version` | the protocol: one RPC round-trip, hand-built inside the app |
| **`c6_wifi_link`** | **the facade, and the co-processor's acceptance of a real
  `Req_WifiInit` configuration** |

The last of those is the reason this application exists on a bench rather than
only in host tests. `tests/test_ra8_c6link.c` drives every layer of the facade
against a co-processor model that decodes what the host transmits with the same
generated codec the C6 runs -- so it proves this firmware *encodes*
`Req_WifiInit` correctly. It cannot prove the co-processor *accepts* it: the
twenty scalars that request carries (a magic word, buffer counts, aggregation
flags, an HE queue count) are validated by the far side's own
`esp_wifi_init()`, against limits that belong to the co-processor's build.

So a FAIL here prints the co-processor's `esp_err_t` verbatim next to the RPC id
that produced it. `ESP_ERR_INVALID_ARG` (`-1` in the generic case, `0x102` as an
`esp_err_t`) against `RPC_ID__Req_WifiInit` means the transmitted configuration,
not the link; the values and the reasoning behind each are in
`ra8_c6link_wifi_init_t` in `libs/ra8_c6link/src/ra8_c6link_wifi.c`.

## The run

1. bring the port up under ThreadX and bind its three-row transport seam;
2. announce this host on `ESP_PRIV_IF`, then establish readiness with one
   `Req_GetCoprocessorFwVersion` exchange. Readiness is deliberately not the
   `Event_ESPInit` boot event: that fires once, when the *co-processor* boots,
   and the C6 has its own supply, so resetting this board does not reboot it.
   An earlier revision waited for the event and therefore passed exactly once,
   on a freshly-flashed co-processor;
3. the identity from step 2 is checked against the vendored host driver's own
   version (`esp_hosted_host_fw_ver.h`) and against the ESP32-C6 chip id, so the
   check is the host/co-processor version lock rather than a literal written
   twice;
4. `Req_WifiInit` -> `Req_SetWifiMode(STA)` -> `Req_WifiStart`, then a short
   drain so any Wi-Fi event the co-processor raises on its own is reported;
5. `Req_GetMACAddress` for the station address;
6. `Req_WifiStop` -> `Req_WifiDeinit`, then one `PASS` or `FAIL` line.

**No network is joined.** Association needs an AP in range and is
[#492](https://github.com/bsikar/ra8-firmware/issues/492); this stops at "the
radio is up and has an address", which is exactly the state an IP driver starts
from.

## Bench requirements

- The C6 harness on **J26** (Pmod1), wired per `coprocessor/esp32c6/pins.env`.
- **SW4 1=OFF 2=OFF 3=ON 4=OFF.** SW4-3 ON is what connects J26-1..J26-4 to the
  MCU at all; with it wrong the board and the co-processor both look healthy and
  the link simply does not exist.
- The C6 powered from its own USB, running the pinned `network_adapter` image
  (esp-idf v5.5.4, esp-hosted-mcu `949bb30`, FW 2.12.11).

`c6_spi_probe` tells the two failure modes apart without guesswork. If it
reports every wire as `hi->1 lo->1 high-side(pull-up or driven high)`, the MCU
cannot pull those lines low at all and the fault is SW4-3 or the harness -- not
the co-processor, which will still boot happily on its own USB and answer its
own console. In that state every app in this tier reads `0xff` off CIPO and
fails identically, so a red here means "look at the bench", not "look at the
firmware".

## Console

```
c6_wifi: EK-RA8D2 <-> ESP32-C6 station bring-up through ra8_c6link
c6_wifi: cpuclk0_hz=1000000000 pclka_hz=100000000
c6_wifi: spi sci=2 sck_hz=5000000 frame_bytes=1600 max_payload=1588
c6_wifi: port_init=k_ra8_ok
c6_wifi: link_open=k_ra8_ok
c6_wifi: await_ready=ok
c6_wifi: coprocessor fw=2.12.11 chip_id=0x0d idf_target=esp32c6 expected=2.12.11
c6_wifi: station started (WifiInit + SetWifiMode + WifiStart accepted)
c6_wifi: poll drain xfers=... rpc_in=... events=...
c6_wifi: station mac=...
c6_wifi: station stopped (WifiStop + WifiDeinit accepted)
c6_wifi: PASS ra8_c6link drove the coprocessor station up and read its address
```
