# infra/network/ -- ESP32-C6 wireless bench LAN (odd-islanded + even-uplinked)

A bench WiFi network for exercising the ESP32-C6 co-processor and future
wireless test clients. A FortiGate 81E-POE is the router/DHCP/switch, a Meraki
MR18 (running OpenWrt) is the access point, and the ESP32-C6 (2.4 GHz-only)
joins a dedicated bench SSID.

The FortiGate's single physical switch is split into **two hard-switch
segments** on the same silicon (sw0):

- an **odd** segment (`lan`, ports 1,3,5,7,9,11 -- 10.0.40.0/24) that is
  deliberately **islanded** by policy 2, which denies it to `wan1` even though
  `wan1` installs the FortiGate's device-wide default route. This is the local
  bench network with no internet; the RA8 boards, probes, and ESP32-C6 live here.
- an **even** segment (`lan-even`, ports 2,4,6,8,10,12 -- 10.0.41.0/24) that
  **NATs out `wan1`** (a DHCP uplink to the house gateway), for devices that
  need internet access. Nothing on the islanded bench uses it.

`fortigate-bench.conf` is the authoritative desired-state replay declaration;
this file explains its reasoning without restating every command.

Everything here is reproducible from code plus OpenBao. No credential lives in
this directory, any commit, or any kept log -- and that includes the chassis
serial, which FortiOS turns into the `maintainer` recovery password (see
[Why the chassis serial is treated as a credential](#why-the-chassis-serial-is-treated-as-a-credential)).

## Topology

```
   ODD segment "lan" 10.0.40.1/24            EVEN segment "lan-even" 10.0.41.1/24
   ISLANDED: policy 2 DENIES                 NAT out wan1 (policy 1); DHCP
   odd -> wan1                               .41.100-.199; internet devices
        (all bench kit lives here)                        |
   +-------------------------+                            |   wan1 (DHCP lease
   |  FortiGate 81E-POE       | odd ports (1,3,5,7,9,11)  |   from house gw)
   |  ra8-bench-fw            |=========+                 |        |
   |                          | even ports (2,4,6,8,10,12)|        v
   |  serial: see OpenBao     |=========|=======+         |   +---------------+
   |  lan       10.0.40.1/24  |         |       |         +-->| internet      |
   |  lan-even  10.0.41.1/24  |         |       |             | cam-relay     |
   |  DHCP .40.100-.199       |         |       |             | .41.102       |
   |  DHCP .41.100-.199       |         |       |             +---------------+
   |  reserves .40.10 AP      |         |  +----------------+
   |  reserves .40.101 star   |         |  | Meraki MR18    |
   |  reserves .41.102 cam    |         |  | OpenWrt (ath9k)|
   |  admin: ssh + https      |         |  | static .40.10  |
   +------------+------------+          |  | br-trusted     |
                | console (DB9,9600 8N1)|  | radio1 2.4GHz  |
                |                       |  +--------+-------+
      /dev/serial/by-id/usb-FTDI_*      |
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
  FortiGate is the single DHCP server on each segment. The AP's MAC is
  DHCP-reserved to `.40.10` in `fortigate-bench.conf`, so it holds that address
  whether it is static or DHCP.
- The wired bench Pi (`star-bench-wired`, MAC `00:05:1b:db:75:d3`) is pinned
  by DHCP server 1 reservation 2 to `10.0.40.101` on the odd segment. It stays
  islanded with the rest of the bench.
- The camera relay (`cam-relay`, MAC `88:a2:9e:9b:d0:ea`) is pinned by DHCP
  server 3 reservation 1 to `10.0.41.102` on the even segment, where it has the
  segment's normal `wan1` access.
- The AP is powered by FortiGate PoE (an odd port). A FortiGate reboot cycles
  the AP -- expected and harmless.
- C6, iPhone, Mac, and Windows clients deliberately remain transient DHCP
  leases; none belongs in the reservation inventory. The Win11 bench client
  (`win-ci`) remains wired to odd port3, so it has no internet and its Tailscale
  client is expected to remain offline. Resolve its current lease before
  reaching it through the `star` jump. A human may move its cable to an even
  port temporarily for pulls or updates, but must return it to port3 before
  resuming bench work.

### Cabling caution -- odd vs even ports

Which segment a device lands on is set purely by **which physical port its cable
is in** -- odd ports are the islanded `lan` switch, even ports the
internet-facing `lan-even` switch, with no per-device setting involved. Keep the
isolated bench kit on ODD ports (a cable moved onto an even port silently drops
that device onto the internet-reachable segment), and keep anything that needs
internet on EVEN ports.

## Subnet plan

### Odd / islanded -- 10.0.40.0/24 (`lan`)

| Range              | Use                                             |
|--------------------|-------------------------------------------------|
| `10.0.40.1`        | FortiGate `lan` interface (gateway, DNS)        |
| `10.0.40.2 - .9`   | Reserved (spare infrastructure statics)         |
| `10.0.40.10`       | Meraki MR18 AP (static; also DHCP-reserved to its MAC) |
| `10.0.40.11 - .99` | Reserved statics (future bench gear)            |
| `10.0.40.100-.199` | DHCP pool (`.101` reserved to `star-bench-wired`) |
| `10.0.40.200-.254` | Free                                            |

### Even / uplinked -- 10.0.41.0/24 (`lan-even`)

| Range              | Use                                             |
|--------------------|-------------------------------------------------|
| `10.0.41.1`        | FortiGate `lan-even` interface (gateway, DNS)   |
| `10.0.41.2 - .99`  | Reserved statics                                |
| `10.0.41.100-.199` | DHCP pool (`.102` reserved to `cam-relay`)      |
| `10.0.41.200-.254` | Free                                            |

Only these three DHCP reservations belong in the replay declaration:

| DHCP server | Segment    | Reservation ID | Address      | MAC                 | Description        |
|-------------|------------|----------------|--------------|---------------------|--------------------|
| 1           | odd `lan`  | 1              | `10.0.40.10` | `00:18:0a:7b:dd:eb` | `MR18-AP`          |
| 1           | odd `lan`  | 2              | `10.0.40.101` | `00:05:1b:db:75:d3` | `star-bench-wired` |
| 3           | `lan-even` | 1              | `10.0.41.102` | `88:a2:9e:9b:d0:ea` | `cam-relay`        |

Live lease-table entries for transient C6, iPhone, Mac, or Windows clients are
not desired-state declarations and must not be copied into this table.

`wan1` itself takes a DHCP lease from the house gateway. That house subnet is
upstream of the even segment and off-limits to the bench.

## Isolation model (odd vs even)

Three firewall policies, in `fortigate-bench.conf`, do all the work:

| # | Name             | From        | To         | Action        |
|---|------------------|-------------|------------|---------------|
| 1 | `even-to-wan`    | `lan-even`  | `wan1`     | accept + NAT  |
| 2 | `deny-odd-to-wan`| `lan` (odd) | `wan1`     | **deny**      |
| 3 | `odd-to-even-mgmt`| `lan` (odd)| `lan-even` | accept        |

- **Policy 1** is the even segment's only path out. Its `srcintf` is `lan-even`,
  and it **must never** be `lan`: it pre-existed as `lan -> wan1 all/all accept
  NAT`, harmless only while `wan1` had no address. `wan1` now has a lease, so
  reverting `srcintf` to `lan` would silently give the whole islanded bench
  full internet. The tracked `fortigate-bench.conf` encodes `lan-even` and
  says so in a comment; keep it.
- **Policy 2** is why the odd segment is islanded even though a routable `wan1`
  now exists on the box: it explicitly denies odd -> wan1. (It carries no `set
  action`, and deny is the FortiOS default.)
- **Policy 3** lets the odd bench reach the even net for management; there is
  no reverse (even -> odd) policy, so the even segment cannot reach the bench.

## Credentials -- all in OpenBao, none here

Mount `secret` (KV v2), path **`secret/ra8d2/bench-network`**. Read it with the
existing read-only AppRole (`scripts/secrets/openbao_client.py`,
`~/.config/hil/openbao.env`). It holds the FortiGate admin account and
password, its hostname and LAN address, the chassis serial and the derived
`maintainer` recovery password, the AP's ssh/LuCI credentials and address, the
bench SSID and its generated PSK, the superseded PSKs of the SSIDs that
predated it, the bench subnet, and the FortiGate console `by-id` device path.

The admin re-provision writer lives on the OpenBao host as
`~/.openbao/configure_bench_network.sh` (root-token path, mirrors
`~/.openbao/configure_openbao.sh`); both are host-owned utilities outside this
repository. The bench-network writer preserves an already-generated bench PSK.

### Why the chassis serial is treated as a credential

FortiOS derives the console recovery account's password **mechanically** from
the chassis serial: the account is `maintainer` and the password is
`bcpb<serial>`. The serial is therefore not an inert asset tag -- publishing it
publishes the recovery password, which is why it lives in OpenBao with the rest
and appears nowhere in this tree.

An earlier revision of this file called the serial "not secret" and printed
it. That reasoning was wrong in exactly one step: it treated the serial as an
identifier rather than as the input to a password derivation that the very next
entry of the same record documents. Do not reintroduce it -- neither here, nor
in the topology diagram, nor in a status note.

Two facts bound the blast radius, and neither is a reason to relax the rule:

- `maintainer` is usable **only over the physical console cable**, and only
  within roughly 60 seconds of a power cycle. It is not reachable over ssh,
  https, or the network at all.
- Policy 2 denies the odd segment access to the device-wide WAN route. The even
  segment reaches the internet, but the admin planes (`ssh`/`https`) are only
  bound to the two LAN interfaces, not to `wan1`, so console access still
  implies physical access to the bench.

A serial also cannot be rotated the way a password can -- it is stamped in the
chassis. The durable mitigations are the two above, not re-keying.

## Console access

The FortiGate console cable is on the bench Pi (`ssh star`), at 9600 8N1 on a
`/dev/serial/by-id/usb-FTDI_*` device. `fg_bringup.py` resolves that glob at run
time and fails loudly if it matches zero or more than one device, the same
identity-not-enumeration rule `scripts/hil/lib/tty_resolve.sh` applies to the
other bench consoles. The cable's own serial is maintainer-specific and stays
out of the tree. Pass the optional `tty` argument to a live FortiGate Just
recipe to pin a specific device; the recipe admits that one value after
clearing the ambient environment.

Drive the console only through the specific FortiGate Just recipes below
(never an interactive `screen` or a raw Python invocation). They clear the
ambient environment, require the repository-managed Python environment, and
run the driver in isolated mode. The driver reads every credential from the
default OpenBao configuration and masks them in the transcript. Login mechanics
that matter on this unit: **lines end with a bare CR** (a trailing LF submits an
empty password and desyncs the login), prompt matching is case-insensitive, and
the installed FortiOS **forces a password change on the first post-wipe login**
(handled automatically).

Admin `ssh`/`https` is also bound to both LAN interfaces, so read-only
`show`/`get` captures can be taken over the network from a host on either
segment instead of the console.

### FortiGate filesystem maintenance

GitHub issue #791 tracks the open operator action for the unit's
`File System Check Recommended` warning after an unsafe reboot. A human must
schedule `execute scan 259`: it reboots the FortiGate and can take up to an
hour, so it must never run during a bench job. Agents and automation must not
invoke it.

### Offline declaration checks

Run `just infra::fortigate_config_selftest`,
`just infra::fortigate_config_lint`, and
`just infra::fortigate_replay_dry_run` before reviewing a replay. The selftest
proves the exact reservation and recipe checks in both directions. The lint and
dry-run commands use the same loader and renderer as `bootstrap`, but stop
before console, credential, or hardware access. The dry run writes the
replayable command stream to standard output.

## Re-provision from scratch

1. **Store creds** (once, or to rotate): on the OpenBao host run
   `~/.openbao/configure_bench_network.sh` with the values on stdin.
2. **Wipe + configure the FortiGate** (from `ssh star`):
   ```
   just infra::fortigate_bootstrap
   ```
   `bootstrap` logs in, issues `execute factoryreset`, then after the wiped boot
   completes the forced password change from OpenBao and replays
   the sibling `fortigate-bench.conf` on the detected LAN interface (`lan`). An
   explicit declaration path and optional console TTY may be passed as the
   recipe's first and second arguments when reviewing a different file or
   selecting one of several adapters. `detect_lan()` rewrites the `internal`
   token to the real primary name; the second switch is the literal `lan-even`,
   which the rewrite leaves alone.
3. **Configure the AP** over the FortiGate console jump (no direct IP path):
   ```
   just infra::fortigate_ap_configure
   ```
   Stands up `ra8-bench` on radio1 (2.4 GHz, WPA2-PSK), disables the AP's DHCP
   (dumb AP), disables the orphaned SSIDs it shipped with, keeps home-network.
   `/bin/bash -p infra/network/ap_openwrt.sh` is the equivalent for a host
   cabled directly onto the LAN.
4. **Verify** (from `ssh star`): `just infra::fortigate_verify` checks FortiGate
   and AP reachability, and `/bin/bash -p infra/network/verify_bench_wifi.sh` is
   the wlan0 join test (it needs RF range).

**Do not `bootstrap` against a live unit casually (#561).** The even-segment
devices depend on the running config, and `fortigate-bench.conf` IS that
config: an out-of-date declaration replayed over a wipe reverts whatever it
does not describe.

## What the ESP32-C6 WiFi-join test needs

| Fact          | Value                                               |
|---------------|-----------------------------------------------------|
| SSID          | `ra8-bench`                                          |
| Band / radio  | 2.4 GHz -- MR18 **radio1** (phy2); channel 6, HT20  |
| Security      | WPA2-PSK (`psk2`, CCMP; PMF off for compatibility)  |
| Passphrase    | OpenBao `secret/ra8d2/bench-network`                 |
| Gateway / DNS | `10.0.40.1` (FortiGate, odd segment)               |
| Addressing    | DHCP from the FortiGate, pool `10.0.40.100-.199`    |

The C6 joins the AP, which bridges into the **odd** (islanded) segment -- it
does not touch the even/uplinked segment at all.

**One verification caveat.** The end-to-end wlan0 join test from the bench Pi
cannot be run from the Pi's onboard radio: it is out of RF range of the
ceiling-mounted MR18. That is placement, not config. The ESP32-C6, co-located
with the AP, is the live join test; a wired host on an **odd** port (as `star`'s
USB Ethernet already is) gives a direct end-to-end lease/ping proof on the
islanded segment.

## See also

- [PI_PROVISIONING.md](PI_PROVISIONING.md) -- provisioning a fresh Raspberry
  Pi headless on this bench (generic gotchas: cloud-init, the `ssh` enable
  flag, `userconf.txt`, and the serial console).
