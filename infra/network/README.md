# Isolated wireless bench LAN

This directory defines the reproducible network used to exercise the ESP32-C6
co-processor and other wireless test clients. The tracked replay declaration,
`fortigate-bench.conf`, is the authoritative desired state. This document
describes the security and operating model without publishing the deployment's
equipment inventory or network coordinates.

## Topology and isolation

The router divides its switch into two physical segments:

- an **isolated bench segment** for boards, probes, the bench controller, and
  wireless test clients; and
- an **uplinked management segment** for devices that require internet access.

The access point bridges the dedicated 2.4 GHz test WLAN into the isolated
segment. It operates only as an access point: DHCP is disabled there, and the
router is the sole DHCP authority. The segments are separate switch interfaces,
not VLANs.

Firewall policy must provide these properties:

| Source | Destination | Policy |
|---|---|---|
| Management segment | WAN | Accept with NAT |
| Isolated bench segment | WAN | Deny |
| Isolated bench segment | Management segment | Accept for management |
| Management segment | Isolated bench segment | No reverse rule |

Physical port placement determines which segment a device joins. Operators
must verify cabling before bench work and return temporarily uplinked equipment
to the isolated segment afterward.

Exact subnets, interface addresses, DHCP pools, reservations, MAC addresses,
port assignments, device models, host labels, and wireless identifiers are
deployment data. Keep them in the replay declaration or private operator
inventory; do not duplicate them in narrative documentation.

## Credentials

No credential or credential-derived device identifier belongs in this
directory, a commit, or a retained log. The console driver reads its configured
secret record through `scripts/secrets/openbao_client.py` using the operator's
mode-0600 OpenBao environment file. The record contains the router and access
point credentials, WLAN settings, and console-device identity.

Some appliances use a chassis identifier as part of console recovery
authentication. Treat such identifiers as credentials: do not print their
values or document the recovery-password derivation.

## Console access

The router console is attached to the bench controller. `fg_bringup.py`
resolves its configured `/dev/serial/by-id/` identity at run time and fails if
the identity is absent or ambiguous. Drive it only through the repository's
specific `infra::fortigate_*` recipes. Those recipes isolate the environment,
use the pinned Python environment, obtain credentials at run time, and mask
them in transcripts.

Operators may use network administration only from an authorized host on a
management interface. Do not publish literal SSH destinations or live
addresses in examples.

## Offline declaration checks

Before reviewing a replay, run:

```sh
just infra::fortigate_config_selftest
just infra::fortigate_config_lint
just infra::fortigate_replay_dry_run
```

These commands exercise the same loader and renderer as bootstrap without
accessing credentials, the console, or hardware. The dry run writes the
replayable command stream to standard output; review it for unexpected secret
or deployment data before retaining the output.

## Re-provisioning

1. Store or rotate the required values using the private vault-administration
   procedure.
2. From an authorized bench controller, run
   `just infra::fortigate_bootstrap`.
3. Configure the access point with
   `just infra::fortigate_ap_configure`.
4. Run `just infra::fortigate_verify` and, when an RF client is available,
   `/bin/bash -p infra/network/verify_bench_wifi.sh`.

Bootstrap performs a factory reset and must not be run casually. Confirm that
no bench job or dependent device is active and review the declaration first.

## ESP32-C6 Wi-Fi test contract

The test requires a 2.4 GHz WPA2-PSK WLAN bridged to the isolated segment, with
DHCP supplied by the router. The SSID, passphrase, gateway, and address plan are
deployment inputs obtained from protected configuration. The test must prove
association, lease acquisition, and reachability without granting the C6 a WAN
route.

## See also

- [PI_PROVISIONING.md](PI_PROVISIONING.md) -- provisioning a fresh Raspberry
  Pi headlessly, including cloud-init and serial-console considerations.
