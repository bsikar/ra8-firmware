# infra/network/ -- ESP32-C6 wireless bench LAN (odd-islanded + even-uplinked)

A bench WiFi network for exercising the ESP32-C6 co-processor and future
wireless test clients. A FortiGate 81E-POE is the router/DHCP/switch, a Meraki
MR18 (running OpenWrt) is the access point, and the ESP32-C6 (2.4 GHz-only)
joins a dedicated bench SSID.

The FortiGate's single physical switch is split into **two hard-switch
segments** on the same silicon (sw0):

- an **odd** segment (`lan`, ports 1,3,5,7,9,11 -- 10.0.40.0/24) that is
  deliberately **islanded**: no default route, and an explicit firewall policy
  denying it to the WAN. All the bench kit lives here.
- an **even** segment (`lan-even`, ports 2,4,6,8,10,12 -- 10.0.41.0/24) that
  **NATs out `wan1`** (a DHCP uplink to the house AT&T gateway). It carries the
  internet-dependent camera pipeline -- a Reolink doorbell and the `cam-relay`
  node that relays it onto the tailnet; nothing on the islanded bench uses it.

Everything here is reproducible from code plus OpenBao. No credential lives in
this directory, any commit, or any kept log -- and that includes the chassis
serial, which FortiOS turns into the `maintainer` recovery password (see
[Why the chassis serial is treated as a credential](#why-the-chassis-serial-is-treated-as-a-credential)).

## Topology

```
   ODD segment "lan" 10.0.40.1/24            EVEN segment "lan-even" 10.0.41.1/24
   ISLANDED: no default route,               NAT out wan1 (policy 1); DHCP
   policy 2 DENIES odd -> wan1               .41.100-.199; camera only
        (all bench kit lives here)                        |
   +-------------------------+                            |   wan1 (DHCP lease
   |  FortiGate 81E-POE       | odd ports (1,3,5,7,9,11)  |   from AT&T gw)
   |  ra8-bench-fw            |=========+                 |        |
   |  FortiOS v6.4.6          | even ports (2,4,6,8,10,12)|        v
   |  serial: see OpenBao     |=========|=======+         |   +---------------+
   |  lan       10.0.40.1/24  |         |       |         +-->| Reolink /     |
   |  lan-even  10.0.41.1/24  |         |       |             | cloud camera  |
   |  DHCP .40.100-.199       |         |       |             +---------------+
   |  DHCP .41.100-.199       |         |  +----------------+
   |  reserves .40.10 -> AP   |         |  | Meraki MR18    |
   |  admin: ssh + https      |         |  | OpenWrt (ath9k)|
   +------------+------------+          |  | static .40.10  |
                | console (DB9,9600 8N1)|  | br-trusted     |
                |                       |  | radio1 2.4GHz  |
      /dev/serial/by-id/usb-FTDI_*      |  +--------+-------+
                |                       |           | SSID: ra8-bench (WPA2-PSK)
        +-------+--------+     +--------+-------+    |
        | bench Pi (star)|     | wired bench kit|  +-+----------------------+
        | ssh star       |     | (ODD ports)    |  |  ESP32-C6 test client  |
        | .40.101 wired  |     +----------------+  |  (2.4 GHz, ra8-bench)   |
        +----------------+                         +------------------------+
```

- Flat L2 per segment: the AP bridges every SSID + its uplink into one bridge
  (`br-trusted`), untagged, into the FortiGate `lan` (odd) hard switch. **No
  VLAN tags.** The even segment is a separate hard switch, not a VLAN.
- The AP is a **dumb AP**: its own dnsmasq/odhcpd DHCP is disabled, so the
  FortiGate is the single DHCP server on each segment.
- The AP is powered by FortiGate PoE (an odd port). A FortiGate reboot cycles
  the AP -- expected and harmless.

### Load-bearing port occupancy -- odd for the bench, even for cam-relay

Isolation is enforced by **which physical port a cable is in**, not by any
per-device setting. Odd ports belong to the islanded `lan` switch; even ports
belong to the internet-facing `lan-even` switch. The rule cuts **both ways**,
and neither direction is optional:

- **All RA8 bench kit (AP, bench Pi, any wired test host) stays on ODD ports.**
  Moving a cable onto an even port silently drops that device onto the NAT'd,
  internet-reachable segment -- no warning, no config change, isolation gone.
- **`cam-relay` stays on an EVEN port -- deliberately.** It REQUIRES internet
  egress AND must reach the house LAN camera at `192.168.1.86`. Moving it to an
  odd port does not "restore isolation"; it BREAKS THE CAMERA PIPELINE -- no
  internet, and no NAT path to the doorbell, so the relay simply goes dark.

So the two constraints are fixed and opposite: **RA8 bench kit on ODD,
`cam-relay` on EVEN.** Anyone tidying cables must not "correct" `cam-relay` onto
an odd port, and must not let bench kit drift onto an even one. Treat the even
ports as "outside" for everything except `cam-relay`.

## Nodes

The bench-side nodes -- the bench Pi `star`, the Meraki MR18 AP, and the
ESP32-C6 test client -- all sit on the **odd** islanded segment and are drawn in
the topology above. One node sits on the **even** segment on purpose:

### cam-relay -- Reolink doorbell relay (EVEN segment)

A Raspberry Pi 4 Model B (Rev 1.5) brought up 2026-08-02 as the first live node
on the even segment. It runs `go2rtc` to relay the house-side Reolink doorbell
camera (`192.168.1.86`) onto the tailnet, so that Frigate at the remote site can
consume it.

| Fact       | Value                                                              |
|------------|--------------------------------------------------------------------|
| Host       | `cam-relay`, Raspberry Pi 4 Model B Rev 1.5                         |
| OS         | Debian GNU/Linux 13 (trixie), image `2026-06-18-raspios-arm64-lite` |
| Address    | `10.0.41.102` on `eth0`, DHCP from `lan-even` (FortiGate EVEN port) |
| Access     | ssh key-only as user `bsikar`; passwordless sudo                   |
| Console    | serial on `star` at `/dev/ttyUSB1` (FTDI, serial `B0046G3K`) @115200, `enable_uart=1` |
| `wlan0`    | deliberately unused (wired is sufficient)                          |
| Camera     | Reolink doorbell at `192.168.1.86` (house LAN, upstream of `wan1`) |

**Why it is on an even port (and must stay there):** `cam-relay` needs BOTH
internet egress AND a path to the house LAN camera at `192.168.1.86`, so it
lives on the internet-facing `lan-even` segment. It must never be "tidied" onto
an odd port -- see "Load-bearing port occupancy" above for both halves of that
rule.

**Verified 2026-08-02:** `cam-relay` reaches the camera `192.168.1.86` on tcp
80/554/8000 (the RTSP handshake returns `200 OK`) through FortiGate NAT out
`wan1`, and general internet egress works.

Its console is a second FTDI adapter on `star`, alongside the FortiGate console
cable; resolve it by its stable serial `B0046G3K` under `/dev/serial/by-id/`
rather than the enumeration-order `/dev/ttyUSB1`, the same
identity-not-enumeration rule the console recipe below applies.

Provisioning a fresh Pi on this bench has five non-obvious gotchas that each
cost `cam-relay` a boot cycle; they are captured in
[PI_PROVISIONING.md](PI_PROVISIONING.md).

## Subnet plan

### Odd / islanded -- 10.0.40.0/24 (`lan`)

| Range              | Use                                             |
|--------------------|-------------------------------------------------|
| `10.0.40.1`        | FortiGate `lan` interface (gateway, DNS)        |
| `10.0.40.2 - .9`   | Reserved (spare infrastructure statics)         |
| `10.0.40.10`       | Meraki MR18 AP (static; also DHCP-reserved to its MAC) |
| `10.0.40.11 - .99` | Reserved statics (future bench gear)            |
| `10.0.40.100-.199` | FortiGate DHCP pool (C6, test hosts)            |
| `10.0.40.200-.254` | Free                                            |

The AP's MAC `00:18:0a:7b:dd:eb` is reserved to `10.0.40.10` in the FortiGate
DHCP server, so the AP holds `.10` whether it is static or DHCP.

### Even / uplinked -- 10.0.41.0/24 (`lan-even`)

| Range              | Use                                             |
|--------------------|-------------------------------------------------|
| `10.0.41.1`        | FortiGate `lan-even` interface (gateway, DNS)   |
| `10.0.41.2 - .99`  | Reserved statics                                |
| `10.0.41.100-.199` | FortiGate DHCP pool (Reolink camera; `cam-relay` at `.102`) |
| `10.0.41.200-.254` | Free                                            |

`wan1` itself takes a DHCP lease from the house AT&T gateway (observed
`192.168.1.84`, default route via `192.168.1.254`, admin distance 5). That
house subnet is upstream of the even segment and off-limits to the bench.

## Isolation model (odd vs even)

Three firewall policies, in `fortigate-bench.conf`, do all the work:

| # | Name             | From        | To         | Action        |
|---|------------------|-------------|------------|---------------|
| 1 | `even-to-wan`    | `lan-even`  | `wan1`     | accept + NAT  |
| 2 | `deny-odd-to-wan`| `lan` (odd) | `wan1`     | **deny**      |
| 3 | `odd-to-even-mgmt`| `lan` (odd)| `lan-even` | accept        |

- **Policy 1** is the camera's only path out. Its `srcintf` is `lan-even`, and
  it **must never** be `lan`: it pre-existed as `lan -> wan1 all/all accept
  NAT`, harmless only while `wan1` had no address. `wan1` now has a lease, so
  reverting `srcintf` to `lan` would silently give the whole islanded bench
  full internet. The tracked `fortigate-bench.conf` encodes `lan-even` and
  says so in a comment; keep it.
- **Policy 2** is why the odd segment is islanded even though a routable `wan1`
  now exists on the box: it explicitly denies odd -> wan1. (It carries no `set
  action`, and deny is the FortiOS default.)
- **Policy 3** lets the odd bench reach the camera net for management; there is
  no reverse (even -> odd) policy, so the camera cannot reach the bench.

## Reaching the even segment from `star` -- PERSISTENT route

`star` is wired onto an **odd** port (`10.0.40.101`, USB-ethernet adapter
`enx00051bdb75d3`, NetworkManager profile "Wired connection 1") and reaches the
FortiGate and the whole odd segment directly. The even segment is a separate
subnet behind the FortiGate, so reaching it -- to inspect the camera's lease, or
to manage `cam-relay` at `10.0.41.102` -- needs a route via the FortiGate.

**Even-segment management is now LIVE** (`cam-relay` runs there), so this route
is no longer a throwaway: it has been made **persistent** on `star`. `star` is
NOT ansible-managed (it is the bench Pi, not a CI runner), so the netplan file
on the box is the source of truth and this section is its declaration in-repo.

The route lives in the NetworkManager-rendered netplan file for
`enx00051bdb75d3`:

```
/etc/netplan/90-NM-43978e25-9384-317d-9033-9d9c88369e3e.yaml
```

with this stanza added under that ethernet, a sibling of `dhcp4`:

```yaml
      routes:
        - to: "10.0.41.0/24"
          via: "10.0.40.1"
```

Applied with `sudo netplan generate && sudo netplan apply`, which renders it
into the NetworkManager keyfile as `route1=10.0.41.0/24,10.0.40.1`. Verified
active as:

```
10.0.41.0/24 via 10.0.40.1 dev enx00051bdb75d3 proto static metric 100
```

`cam-relay` answers at `10.0.41.102` in ~0.39 ms over it, while `star`'s own
default route (via `wlan0`) and general internet stay intact. A pre-change
backup of the file sits beside it with the suffix `.bak-preroute`
(`.../90-NM-43978e25-9384-317d-9033-9d9c88369e3e.yaml.bak-preroute`).

This is the **same class of defect as #563** -- a live-only route or ip-rule
that silently reverts on reboot. A runtime `ip route add 10.0.41.0/24 via
10.0.40.1` would have vanished on the next reboot of `star` and quietly cut
management of the even segment; persisting it in netplan is the fix. Both cases
are now either fixed (this route) or tracked (#563).

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
- The odd segment is deliberately islanded (no WAN, no default route). The even
  segment reaches the internet, but the admin planes (`ssh`/`https`) are only
  bound to the two LAN interfaces, not to `wan1`, so console access still
  implies physical access to the bench.

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
python3 infra/network/fg_bringup.py --selftest    # offline: check LAN-name detection logic
```

Admin `ssh`/`https` is also bound to both LAN interfaces (`10.0.40.1`,
`10.0.41.1`), so read-only `show`/`get` captures can be taken over the network
from a host on either segment instead of the console.

## Re-provision from scratch

1. **Store creds** (once, or to rotate): on the OpenBao host run
   `~/.openbao/configure_bench_network.sh` with the values on stdin.
2. **Wipe + configure the FortiGate** (from `ssh star`):
   ```
   python3 infra/network/fg_bringup.py bootstrap    # login, factoryreset, reconfigure
   ```
   `bootstrap` logs in, issues `execute factoryreset`, then after the wiped boot
   completes the forced password change from OpenBao and replays
   `fortigate-bench.conf` on the detected LAN interface (`lan`). detect_lan()
   rewrites the `internal` token to that real name; the second switch is the
   literal `lan-even`, which the rewrite leaves alone.
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
| Gateway / DNS | `10.0.40.1` (FortiGate, odd segment)               |
| Addressing    | DHCP from the FortiGate, pool `10.0.40.100-.199`    |

The C6 joins the AP, which bridges into the **odd** (islanded) segment -- it
does not touch the even/uplinked segment at all.

## Current status (2026-08-02) -- NETWORK UP, odd/even split live

The bench LAN is live and now carries the odd/even split, captured read-only
from the running unit and folded verbatim into `fortigate-bench.conf`:

- **FortiGate** (`ra8-bench-fw`, FortiOS v6.4.6 build1879): one physical switch
  (sw0) split into `lan` (odd ports, 10.0.40.1/24) and `lan-even` (even ports,
  10.0.41.1/24). DHCP server 1 serves `.40.100-.199` (`.10` reserved to the AP
  MAC); DHCP server 3 serves `.41.100-.199`. `wan1` holds a DHCP lease
  (default route, distance 5). Firewall: even NATs out `wan1`, odd is denied
  out (islanded), odd may manage even. fortiguard / central-management /
  autoupdate disabled.
- **AP** (Meraki MR18, OpenWrt): reachable at 10.0.40.10 on the odd segment,
  `ra8-bench` beaconing on radio1 (2.4 GHz, WPA-PSK/CCMP, channel 6), bridged
  into `br-trusted`. Dumb AP (own DHCP disabled); orphaned iot/guest SSIDs
  disabled; home-network kept.
- **cam-relay** (Raspberry Pi 4, `10.0.41.102`): the first live node on the even
  segment -- a `go2rtc` relay for the house Reolink doorbell (`192.168.1.86`),
  verified reaching the camera through FortiGate NAT out `wan1`. `star`'s route
  onto the even segment (`10.0.41.0/24 via 10.0.40.1`) is now persisted in
  netplan (see "Reaching the even segment from `star`" and the Nodes section
  above), no longer a runtime-only route.

**Bootstrap is deferred (#561):** the camera provisioning depends on the
current running config, so do not run `fg_bringup.py bootstrap` against the live
unit until this declaration has landed and been reviewed. The declaration IS
the running config; the earlier "islanded, no uplink" wording it replaced would
have reverted the camera network.

**One verification caveat carried over:** the end-to-end wlan0 join test from
the bench Pi could not be run because the Pi's onboard wlan0 is out of RF range
of the ceiling-mounted MR18. This is placement, not config -- the SSID is
proven beaconing on the AP and bridged to the DHCP path. The ESP32-C6,
co-located with the AP, is the live join test; a wired host on an **odd** port
(as `star`'s RTL8153 already is at `10.0.40.101`) gives a direct end-to-end
lease/ping proof on the islanded segment.
