# infra/network/ -- isolated ESP32-C6 wireless bench LAN

A self-contained, air-gapped WiFi network for exercising the ESP32-C6
co-processor and future wireless test clients. It has **no uplink** to the home
router by design: a FortiGate 81E-POE is the router/DHCP/switch, a Meraki MR18
(running OpenWrt) is the access point, and the ESP32-C6 (2.4 GHz-only) joins a
dedicated bench SSID.

Everything here is reproducible from code plus OpenBao. No credential lives in
this directory, any commit, or any kept log -- and that includes the chassis
serial, which FortiOS turns into the `maintainer` recovery password (see
[Why the chassis serial is treated as a credential](#why-the-chassis-serial-is-treated-as-a-credential)).

## Topology

```
                 (NO uplink -- deliberately islanded; no WAN, no default route)

   +-------------------------+           PoE + data (802.3af, Cat5)
   |  FortiGate 81E-POE       |  port on ===============================+
   |  ra8-bench-fw            |  the LAN                                |
   |  FortiOS v6.4.6          |  hard-switch                            |
   |  serial: see OpenBao     |                                    +----------------+
   |  lan (hard-switch):      |                                    | Meraki MR18    |
   |    10.0.40.1/24          |                                    | OpenWrt (ath9k)|
   |  DHCP .100-.199          |                                    | static .10     |
   |  reserves .10 -> AP MAC  |                                    | br-trusted     |
   |  admin: ssh + https      |                                    | radio1 2.4GHz  |
   +------------+------------+                                     | radio0 5GHz    |
                | console (DB9, 9600 8N1)                          +--------+-------+
                |                                                           | SSID: ra8-bench
      /dev/serial/by-id/usb-FTDI_FT232R_*                                   | WPA2-PSK (radio1)
                |                                                           |
        +-------+--------+                                    +------------+-----------+
        | bench Pi (star)|                                    |  ESP32-C6 test client  |
        | ssh star       |                                    |  (2.4 GHz, joins       |
        +----------------+                                    |   ra8-bench)           |
                                                              +------------------------+
```

- Flat L2: the AP bridges every SSID + its uplink into one bridge (`br-trusted`),
  untagged, into the FortiGate `lan` hardware switch. **No VLAN tags.**
- The AP is a **dumb AP**: its own dnsmasq/odhcpd DHCP is disabled, so the
  FortiGate (10.0.40.1) is the single DHCP server.
- The AP is powered by FortiGate PoE. A FortiGate reboot cycles the AP --
  expected and harmless.

## Subnet plan (10.0.40.0/24)

| Range              | Use                                             |
|--------------------|-------------------------------------------------|
| `10.0.40.1`        | FortiGate LAN interface (gateway, DNS)          |
| `10.0.40.2 - .9`   | Reserved (spare infrastructure statics)         |
| `10.0.40.10`       | Meraki MR18 AP (static; also DHCP-reserved to its MAC) |
| `10.0.40.11 - .99` | Reserved statics (future bench gear)            |
| `10.0.40.100-.199` | FortiGate DHCP pool (C6, test hosts)            |
| `10.0.40.200-.254` | Free                                            |

The AP's MAC `00:18:0a:7b:dd:eb` is reserved to `10.0.40.10` in the FortiGate
DHCP server, so the AP holds `.10` whether it is static or DHCP.

## Credentials -- all in OpenBao, none here

Mount `secret` (KV v2), path **`secret/ra8d2/bench-network`**. Read it with the
existing read-only AppRole (`scripts/secrets/openbao_client.py`,
`~/.config/hil/openbao.env`). Keys:

| Key                          | Meaning                                       |
|------------------------------|-----------------------------------------------|
| `fortigate_admin_user`       | FortiGate admin account (`admin`)             |
| `fortigate_admin_pass`       | FortiGate admin password                      |
| `fortigate_hostname`         | `ra8-bench-fw`                                 |
| `fortigate_lan_ip`           | `10.0.40.1`                                    |
| `fortigate_serial`           | Chassis serial -- CREDENTIAL-EQUIVALENT, see below |
| `fortigate_maintainer_pass`  | `bcpb<serial>` recovery password (secret)     |
| `ap_ssh_user`                | AP OpenWrt ssh user (`root`)                   |
| `ap_ssh_pass`                | AP OpenWrt ssh / LuCI password                |
| `ap_ip`                      | `10.0.40.10`                                   |
| `bench_ssid`                 | `ra8-bench`                                    |
| `bench_psk`                  | WPA2-PSK for `ra8-bench` (generated in-vault) |
| `legacy_psk_iot_network`     | Old `iot-network` PSK                          |
| `legacy_psk_home_network`    | Old `home-network` PSK                         |
| `legacy_psk_guest_network`   | Old `guest-network` PSK                         |
| `subnet`                     | `10.0.40.0/24`                                 |
| `console_tty`                | FortiGate console `by-id` device path          |

The admin re-provision writer lives on the OpenBao host as
`~/.openbao/configure_bench_network.sh` (root-token path, mirrors
`configure_openbao.sh`); it preserves an already-generated `bench_psk`.

### Why the chassis serial is treated as a credential

FortiOS derives the console recovery account's password **mechanically** from
the chassis serial: the account is `maintainer` and the password is
`bcpb<serial>`. The serial is therefore not an inert asset tag -- publishing it
publishes the recovery password, which is why `fortigate_serial` lives in
OpenBao with the rest and appears nowhere in this tree.

An earlier revision of this table called the serial "not secret" and printed
it. That reasoning was wrong in exactly one step: it treated the serial as an
identifier rather than as the input to a password derivation that the very next
row of the same table documents. Do not reintroduce it -- neither here, nor in
the topology diagram, nor in a status note.

Two facts bound the blast radius, and neither is a reason to relax the rule:

- `maintainer` is usable **only over the physical console cable**, and only
  within roughly 60 seconds of a power cycle. It is not reachable over ssh,
  https, or the network at all.
- The unit is deliberately islanded (no WAN, no default route), so console
  access implies physical access to the bench.

A serial also cannot be rotated the way a password can -- it is stamped in the
chassis. The durable mitigations are the two above, not re-keying.

## Console access recipe

The FortiGate console cable is on the bench Pi:

```
host:  ssh star
tty:   /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*-if00-port0
baud:  9600 8N1
```

`fg_bringup.py` resolves that glob at run time and fails loudly if it matches
zero or more than one device, the same identity-not-enumeration rule
`scripts/hil/lib/tty_resolve.sh` applies to the other bench consoles. The
cable's own serial is maintainer-specific and stays out of the tree; set
`FG_CONSOLE_TTY` (or OpenBao `console_tty`) to pin a specific device.

Drive it with `fg_bringup.py` (never an interactive `screen`). It reads every
credential from OpenBao and masks them in the transcript
(`~/ra8-bench/fortigate_console.log`). Login mechanics that matter on this unit:
**lines end with a bare CR** (a trailing LF submits an empty password and
desyncs the login), prompt matching is case-insensitive, and v6.4.6 **forces a
password change on the first post-wipe login** (handled automatically).

```
python3 infra/network/fg_bringup.py login        # login + capture config (read-only)
python3 infra/network/fg_bringup.py verify        # status, DHCP, routing, ping the AP
python3 infra/network/fg_bringup.py ap-inspect    # jump to the AP, dump wireless/network
python3 infra/network/fg_bringup.py ap-status     # confirm ra8-bench hostapd is beaconing
```

## Re-provision from scratch

1. **Store creds** (once, or to rotate): on the OpenBao host run
   `~/.openbao/configure_bench_network.sh` with the values on stdin.
2. **Wipe + configure the FortiGate** (from `ssh star`):
   ```
   python3 infra/network/fg_bringup.py bootstrap    # login, factoryreset, reconfigure
   ```
   `bootstrap` logs in, issues `execute factoryreset`, then after the wiped boot
   completes the forced password change from OpenBao and replays
   `fortigate-bench.conf` on the detected LAN interface (`lan`).
   (`configure` alone re-runs just the post-wipe configuration.)
3. **Configure the AP** over the FortiGate console jump (no direct IP path):
   ```
   python3 infra/network/fg_bringup.py ap-configure
   ```
   Stands up `ra8-bench` on radio1 (2.4 GHz, WPA2-PSK), disables the AP's DHCP
   (dumb AP), disables the orphaned iot/guest SSIDs, keeps home-network.
   `ap_openwrt.sh` is the equivalent for a host cabled directly onto the LAN.
4. **Verify** (from `ssh star`):
   ```
   python3 infra/network/fg_bringup.py verify        # FortiGate + AP reachability
   infra/network/verify_bench_wifi.sh                # wlan0 join test (needs RF range)
   ```

## What the ESP32-C6 WiFi-join test needs

| Fact          | Value                                               |
|---------------|-----------------------------------------------------|
| SSID          | `ra8-bench`                                          |
| Band / radio  | 2.4 GHz -- MR18 **radio1** (phy2); channel 6, HT20  |
| Security      | WPA2-PSK (`psk2`, CCMP; PMF off for compatibility)  |
| Passphrase    | OpenBao `secret/ra8d2/bench-network` key `bench_psk` |
| Gateway / DNS | `10.0.40.1` (FortiGate)                             |
| Addressing    | DHCP from the FortiGate, pool `10.0.40.100-.199`    |

## Current status (2026-07-27) -- NETWORK UP

The bench LAN is wiped, reconfigured, and live:

- **FortiGate** (`ra8-bench-fw`, FortiOS v6.4.6): factory
  reset from the junk multi-VLAN config, then `lan` = 10.0.40.1/24, DHCP
  .100-.199 with `.10` reserved to the AP MAC, admin over ssh+https,
  fortiguard/central-management/autoupdate disabled. The routing table shows
  **only** `C 10.0.40.0/24 connected, lan` -- no default route, wan1/wan2 dark:
  isolation confirmed.
- **AP** (Meraki MR18, OpenWrt): reachable at 10.0.40.10 (FortiGate ping 3/3,
  ARP present on `lan`). `ra8-bench` is beaconing -- `iw dev` reports
  `phy2-ap1 ssid ra8-bench type AP txpower 11 dBm`, WPA-PSK (CCMP), channel 6,
  bridged into `br-trusted` (forwarding). Dumb AP (own DHCP disabled); the
  orphaned iot/guest SSIDs are disabled; home-network is kept.

**One verification caveat:** the end-to-end wlan0 join test from the bench Pi
could not be run because the Pi's onboard wlan0 is **out of RF range** of the
ceiling-mounted MR18 (its scan sees neither `ra8-bench` nor even the AP's
`home-network`, only the Pi's own home AP). This is placement, not config --
the SSID is proven beaconing on the AP itself and bridged to the DHCP path. The
ESP32-C6, co-located with the AP, is the live join test; everything it needs is
in place. A wired alternative (star's RTL8153 cabled onto a FortiGate LAN port)
would also give a direct end-to-end lease/ping proof if desired.
