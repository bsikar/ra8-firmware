# c6_wifi_join -- join the bench Wi-Fi and get an IP by DHCP over the ESP32-C6

This is [#492](https://github.com/bsikar/ra8-firmware/issues/492): the first
application on this board that takes a real network all the way up over the
ESP32-C6. It associates the co-processor's Wi-Fi station with a bench access
point through [`libs/ra8_c6link`](../../../../../libs/ra8_c6link/), then runs a
NetX Duo DHCP client over a new link driver
([`port/netxduo/src/nx_ether_driver_c6.c`](../../../../../port/netxduo/src/nx_ether_driver_c6.c))
to obtain a lease, and finally pings the leased gateway to show the path carries
traffic.

```
# build WITH credentials (see below), then:
make c6_wifi_join
make hil-c6 APP=c6_wifi_join
```

## What it proves that `c6_wifi_link` did not

| App | Proves |
|---|---|
| `c6_wifi_link` | the facade: the station radio comes up and reports its MAC |
| **`c6_wifi_join`** | **association to an AP, a DHCP lease, and IP traffic** |

`c6_wifi_link` stopped at "the radio is up and has an address", which is exactly
the state an IP driver starts from. This app is that IP driver.

## Architecture

The ESP32-C6 running esp-hosted is a pure layer-2 bridge: it forwards 802.3
frames between the Wi-Fi netif and the host, and nothing else. So the whole IP
stack -- ARP, DHCP, ICMP -- runs on the **RA8**, over NetX Duo, exactly as the
on-chip Ethernet apps do. The only new part is the link driver:

- `nx_ether_driver_c6` transmits by handing NetX's framed packet to
  `ra8_c6link_eth_send`, and receives through the facade's 802.3 callback, which
  fires inside a dedicated ThreadX RX poll worker;
- the C6 link is a single, polled SPI transport, so transmit and the RX poll are
  serialised behind one mutex inside the driver.

The run, on the application worker thread:

1. bring the port up under ThreadX, open the link (with the driver's RX callback
   wired in) and establish readiness with one `Req_GetCoprocessorFwVersion`;
2. `ra8_c6link_wifi_start`, read the station MAC, `ra8_c6link_wifi_join` with the
   compiled-in SSID/PSK, then pump the link until
   `k_ra8_c6link_event_sta_connected` arrives (or a disconnect / timeout);
3. hand the link and MAC to `nx_ether_driver_c6`, create the NetX packet pool and
   IP instance, enable ARP / UDP / ICMP, and run the DHCP client to a bound
   lease;
4. print the leased address, mask, gateway and DHCP server, ping the gateway,
   and print one `PASS` or `FAIL` line.

`PASS` is gated on the **DHCP lease**, not the ping: a lease is a full
DISCOVER/OFFER/REQUEST/ACK round trip, which already proves the link passes
traffic in both directions. The gateway ping is reported as an extra
`reachability` line so a gateway that filters ICMP does not turn a real success
into a failure.

## Credentials -- read from the environment, never committed

The SSID and passphrase are **not in this tree**. They are compiled in at build
time from two environment variables, and the passphrase in the real bench setup
comes from OpenBao (`secret/ra8d2/bench-network`, key `bench_psk`):

```sh
export RA8_C6_WIFI_SSID=ra8-bench
export RA8_C6_WIFI_PSK="$(python3 ../../../../../scripts/secrets/openbao_client.py \
    kv-get secret/ra8d2/bench-network --field bench_psk)"
make
```

or drop the same two `KEY=value` lines into the gitignored
`coprocessor/esp32c6/wifi.env` (copy `wifi.env.example`), which the Makefile
sources automatically. Built with neither set the image still compiles -- it
prints `c6_join: FAIL no Wi-Fi credentials compiled in` at runtime rather than
baking a blank credential -- so no secret is ever committed and the aggregate
cross-build stays green. The passphrase does end up in the ELF (as it must for
any supplicant), so treat a credentialed image as a secret.

## Bench requirements

- The C6 harness on **J26** (Pmod1), wired per `coprocessor/esp32c6/pins.env`,
  with **SW4 1=OFF 2=OFF 3=ON 4=OFF** (SW4-3 ON connects J26-1..J26-4 to the MCU).
- The C6 powered from its own USB, running the pinned `network_adapter` image
  (esp-idf v5.5.4, esp-hosted-mcu `949bb30`, FW 2.12.11).
- The **bench access point in RF range of the C6**, handing out DHCP leases.

If `c6_spi_probe` reports every wire as `hi->1 lo->1`, the fault is SW4-3 or the
harness, not the firmware -- see the tier README.

## Console

```
c6_join: EK-RA8D2 <-> ESP32-C6 Wi-Fi join + DHCP + reachability
c6_join: cpuclk0_hz=1000000000 pclka_hz=100000000
c6_join: spi sci=2 sck_hz=5000000 frame_bytes=1600 max_payload=1588
c6_join: port_init=k_ra8_ok
c6_join: link_open=k_ra8_ok
c6_join: coprocessor fw=2.12.11 chip_id=0x0d
c6_join: station mac=... ssid=ra8-bench
c6_join: associated
c6_join: dhcp bound ip=10.0.40.123 mask=255.255.255.0 gw=10.0.40.1 server=10.0.40.1
c6_join: reachability gateway ping=ok
c6_join: events_seen=...
c6_join: PASS ra8_c6link joined the bench Wi-Fi and DHCP leased an address
```
