# DA16600 HIL Setup Guide

How to wire and verify a US159-DA16600EVZ daughter card on an
EK-RA8D2 for the `tz_secure_only_da16600_scan` and
`tz_secure_only_da16600_tcp_echo` HIL tests.

## Bill of Materials

| Item | Notes |
|------|-------|
| EK-RA8D2 evaluation kit | Standard board, no rework |
| US159-DA16600EVZ Pmod | Renesas DA16600 break-out, Digikey 20-US159-DA16600EVZ-ND |
| Pi-as-AP (optional for TCP-echo) | Raspberry Pi running `hostapd` + `dnsmasq` |

## Pmod selection

Plug the US159-DA16600EVZ into **Pmod1 (J26)** on the EK-RA8D2.
Pmod1 maps to SCI channel 2 (TXD2 on P801, RXD2 on P802), which is
otherwise unused by every existing demo in this tree.

* Pmod2 (J25) is wired to SCI0, the same channel that the
  on-board `threadx_netx_tcp_echo` HIL app already exercises. Avoid
  the conflict by picking Pmod1.

EK-RA8D2 v1 UM section 6 (Pmod connectors) and Table 17 (page 26)
document the exact pin mapping for Pmod1. Sanity-check with a
multimeter: P801 should track TXD2 from the chip and P802 should
track RXD2.

## SW4 dip-switch state

The EK-RA8D2 hands Pmod1's TXD2/RXD2 to SCI2 only when SW4-1=OFF
and SW4-2=OFF (the default factory state). No rework needed.

## Power and reset

The DA16600 is powered through the Pmod connector's 3.3 V rail.
Tie the Pmod1.8 `RESET` line (P402) high through the EK-RA8D2's
default pull-up; the US159-DA16600EVZ daughter card releases its
own reset internally once VDD settles.

## Optional Pi-as-AP fixture (TCP-echo only)

The `tz_secure_only_da16600_tcp_echo` demo needs a Wi-Fi access
point with known SSID / passphrase to associate to. The simplest
fixture is a Raspberry Pi running `hostapd` + `dnsmasq` on a
dedicated wlan0.

### One-time Pi setup

```sh
sudo apt install hostapd dnsmasq
sudo systemctl unmask hostapd
```

Write `/etc/hostapd/hostapd.conf`:

```
interface=wlan0
driver=nl80211
ssid=hil_lab
hw_mode=g
channel=6
wmm_enabled=0
auth_algs=1
wpa=2
wpa_passphrase=test1234
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
```

Write `/etc/dnsmasq.conf`:

```
interface=wlan0
dhcp-range=192.168.4.10,192.168.4.50,255.255.255.0,24h
```

Bring it up:

```sh
sudo systemctl restart hostapd dnsmasq
```

After this, the DA16600 should associate to `hil_lab` with passphrase
`test1234` and be assigned a DHCP lease in 192.168.4.10..50. The
demo emits `wifi: ip=<addr>` on the J-Link VCOM channel; the HIL
harness can scrape that and connect `nc` to `<addr>:7` to verify
the echo round-trip.

### Alternative: existing AP

If you already have a Wi-Fi network in the lab, rebuild the tcp_echo
app with the matching credentials:

```sh
make tz_secure_only_da16600_tcp_echo \
  EXTRA_CFLAGS='-DDA16600_SSID="\"mywifi\"" -DDA16600_PASS="\"secret\""'
```

## What is currently blocked on hardware

This commit lands the driver scaffolding + two example apps but
does **not** flash either of them onto a live board. Specifically:

1. The DA16600's actual AT-command dialect varies subtly across
   firmware revisions; UM-WI-046 documents the canonical strings,
   but real silicon may need minor adjustments (e.g. whether
   `+WFJAP:` includes a leading space, whether `+TRTC` cid is
   emitted as an integer or quoted string). The first real-HW run
   will surface those.
2. We pick WPA2-PSK (security mode 4) for `AT+WFJAP`. WPA3-SAE
   targets (security mode 6) need a separate code path the driver
   does not yet implement.
3. The TCP-echo demo's `+TRDTC` parsing assumes payloads fit in a
   single AT line. The DA16600 occasionally chunks long payloads
   across multiple URCs; the driver will need a re-assembly buffer
   once a real frame demonstrates the chunk-boundary behaviour.

All three items get addressed during the first live HIL bring-up
session.
