# infra/network/ -- isolated ESP32-C6 wireless bench LAN

A self-contained, air-gapped WiFi network for exercising the ESP32-C6
co-processor and future wireless test clients. It has **no uplink** to the home
router by design: a FortiGate 81E-POE is the router/DHCP/switch, a Meraki MR18
(running OpenWrt) is the access point, and the ESP32-C6 (2.4 GHz-only) joins a
dedicated bench SSID.

Everything here is reproducible from code plus OpenBao. No credential lives in
this directory, any commit, or any kept log.

## Topology

```
                 (NO uplink -- deliberately islanded; no WAN, no default route)

   +-------------------------+           PoE + data (802.3af, Cat5)
   |   FortiGate 81E-POE      |  port1  =========================+
   |   ra8-bench-fw           |                                  |
   |   internal (LAN switch): |                             +----------------+
   |     10.0.40.1/24         |                             | Meraki MR18    |
   |   DHCP .100-.199         |                             | OpenWrt 25.12  |
   |   admin: ssh + https     |                             | static .10     |
   +------------+------------+                              | radio0 2.4GHz  |
                | console (DB9, 9600 8N1)                   | radio1 5GHz    |
                |                                           +--------+-------+
      /dev/serial/by-id/usb-FTDI_FT232R_..._A9MJ2SSQ                 | SSID: ra8-bench
                |                                                    | WPA2-PSK (2.4GHz)
        +-------+--------+                                           |
        | bench Pi (star)|                             +------------+-----------+
        | ssh star       |                             |  ESP32-C6 test client  |
        +----------------+                             |  + Pi wlan0 (verify)   |
                                                       +------------------------+
```

- Flat L2: one bridge (`br-lan`) on the AP, one broadcast domain, **no VLAN
  tags**. The AP's uplink is a plain access port into the FortiGate LAN switch.
- The AP is powered by FortiGate `port1` PoE. A FortiGate reboot cycles the AP
  -- expected and harmless.

## Subnet plan (10.0.40.0/24)

| Range              | Use                                             |
|--------------------|-------------------------------------------------|
| `10.0.40.1`        | FortiGate LAN interface (gateway, DNS)          |
| `10.0.40.2 - .9`   | Reserved (spare infrastructure statics)         |
| `10.0.40.10`       | Meraki MR18 access point (static, on the AP)    |
| `10.0.40.11 - .99` | Reserved statics (future bench gear)            |
| `10.0.40.100-.199` | FortiGate DHCP pool (C6, Pi wlan0, test hosts)  |
| `10.0.40.200-.254` | Free                                            |

The `10.0.40.0/24` subnet is kept from the pre-existing layout so the AP's
static `10.0.40.10` config keeps working after the FortiGate is re-provisioned.

## Credentials -- all in OpenBao, none here

Mount `secret` (KV v2), path **`secret/ra8d2/bench-network`**. Read it with the
existing read-only AppRole (`scripts/secrets/openbao_client.py`,
`~/.config/hil/openbao.env`). Keys:

| Key                        | Meaning                                        |
|----------------------------|------------------------------------------------|
| `fortigate_admin_user`     | FortiGate admin account (`admin`)              |
| `fortigate_admin_pass`     | FortiGate admin password                       |
| `fortigate_hostname`       | `ra8-bench-fw`                                  |
| `fortigate_lan_ip`         | `10.0.40.1`                                     |
| `ap_ssh_user`              | AP OpenWrt ssh user (`root`)                    |
| `ap_ssh_pass`              | AP OpenWrt ssh / LuCI password                 |
| `ap_ip`                    | `10.0.40.10`                                    |
| `bench_ssid`               | `ra8-bench`                                     |
| `bench_psk`                | WPA2-PSK for `ra8-bench` (generated in-vault)  |
| `legacy_psk_iot_network`   | Old `iot-network` PSK (from owner notes)        |
| `legacy_psk_home_network`  | Old `home-network` PSK                          |
| `legacy_psk_guest_network` | Old `guest-network` PSK                         |
| `subnet`                   | `10.0.40.0/24`                                  |
| `console_tty`              | FortiGate console `by-id` device path           |

The admin `re-provision` writer lives on the OpenBao host as
`~/.openbao/configure_bench_network.sh` (root-token path, mirrors
`configure_openbao.sh`); it regenerates the secret idempotently and preserves
an already-generated `bench_psk`.

## Console access recipe

The FortiGate console cable is on the bench Pi:

```
host:  ssh star
tty:   /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A9MJ2SSQ-if00-port0
baud:  9600 8N1
```

Drive it deterministically (never interactive `screen`):

```
python3 infra/network/fg_console.py probe        # report console state
python3 infra/network/fg_console.py show         # dump running config
```

Every run appends a masked transcript to `~/ra8-bench/fortigate_console.log`.

## Re-provision from scratch

1. **Store creds** (once, or to rotate): on the OpenBao host run
   `~/.openbao/configure_bench_network.sh` with the values on stdin.
2. **Wipe + configure the FortiGate** (from `ssh star`):
   ```
   python3 infra/network/fg_console.py factoryreset
   python3 infra/network/fg_console.py bootstrap infra/network/fortigate-bench.conf
   ```
   `bootstrap` logs in with the factory default, sets the admin password from
   OpenBao, then replays `fortigate-bench.conf`.
3. **Configure the AP** (needs a path to `10.0.40.10`):
   ```
   infra/network/ap_openwrt.sh show      # inspect live config first
   infra/network/ap_openwrt.sh apply     # push the bench SSID + flat br-lan
   ```
4. **Verify end-to-end** (no C6 needed), from the bench Pi:
   ```
   infra/network/verify_bench_wifi.sh
   ```
   Joins `ra8-bench` on wlan0, asserts a `10.0.40.x` DHCP lease, pings the
   FortiGate and AP, then restores wlan0. It arms a guaranteed auto-restore
   first, because wlan0 is star's only uplink.

## What the ESP32-C6 WiFi-join test needs

| Fact          | Value                                               |
|---------------|-----------------------------------------------------|
| SSID          | `ra8-bench`                                          |
| Band          | 2.4 GHz (radio0 on the MR18); channel 6, HT20       |
| Security      | WPA2-PSK (`psk2`)                                    |
| Passphrase    | OpenBao `secret/ra8d2/bench-network` key `bench_psk` |
| Gateway / DNS | `10.0.40.1` (FortiGate)                             |
| Addressing    | DHCP, pool `10.0.40.100-.199`                        |

## Current status (2026-07-27) -- BLOCKED on the FortiGate credential

The design, the OpenBao secret, and every artifact here are complete, but the
gear has **not** yet been re-provisioned:

- **FortiGate: cannot authenticate.** The pre-wipe config rejects BOTH the
  owner-supplied password and the FortiOS `admin`/`<blank>` factory default, so
  `execute factoryreset` cannot be reached over the console. The credential-free
  recovery paths (the `maintainer` account; the boot/BIOS "format boot device"
  menu) each require a hard power cycle within a short window, and the FortiGate
  is **not** on any Tapo-controlled plug (only `board` and `pi` exist) -- there
  is no authorised way to power-cycle it remotely. Per the standing rule,
  brute-forcing the login is out. **Owner action required** (any one of):
    1. Log in on the console and run `execute factoryreset`, or
    2. Power-cycle the FortiGate and, within ~60 s, log in as `maintainer` with
       password `bcpb<serial-number>` (official Fortinet recovery), then
       `execute factoryreset`, or
    3. Interrupt boot into the BIOS menu and format the boot device.
  After any of these, `fg_console.py bootstrap` finishes the job unattended.
- **AP: no reachable path yet.** The MR18 is PoE-powered by the un-wiped
  FortiGate, and an over-the-air scan from the Pi shows **none** of the legacy
  SSIDs (`iot-network` / `home-network` / `guest-network`) nor `ra8-bench`
  broadcasting -- consistent with the AP being unpowered (PoE off in the junk
  config) or at fresh OpenWrt defaults (radios ship disabled). Once the
  FortiGate LAN + PoE are up, `ap_openwrt.sh` configures it.

The OpenBao secret is live now, so the moment the FortiGate is unblocked the
whole bring-up is three scripted commands.
