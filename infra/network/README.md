# FortiGate-managed bench network

This directory contains the FortiGate and access point tooling for the bench.
The tracked `fortigate-bench.conf` is a historical configuration and does not
represent the current appliance state. Do not run bootstrap or configure from
it until a reviewed declaration matches a complete read-only capture.

## Current operating model

Operational addressing, reservations, virtual IPs, services, and policy
configuration remain outside the repository.

Network coordinates and device identifiers are deployment data. Keep them in
a verified declaration or private operator inventory; do not duplicate them in
narrative documentation.

## Credentials

No credential or credential-derived device identifier belongs in this
directory, a commit, or a retained log. The console driver reads its configured
secret record through `scripts/secrets/openbao_client.py` using the operator's
mode-0600 OpenBao environment file. The record contains the router and access
point credentials, WLAN settings, and console-device identity.

Treat chassis identifiers used in recovery authentication as credentials. Do
not print them or document recovery-password derivation.

## Console access

`fg_bringup.py` resolves the console by `/dev/serial/by-id/` identity and fails
if the identity is absent or ambiguous. Drive it through the repository's
`infra::fortigate_*` recipes, which isolate the environment and mask secrets.
Use `just infra::fortigate_bootstrap` only after the declaration has been
reviewed and an authorized change window is scheduled. Use
`just infra::fortigate_ap_configure` to configure the bench AP and
`just infra::fortigate_verify` for read-only verification. These recipes are
the supported entrypoints; do not invoke the Python driver directly.

## Offline declaration checks

The lint, selftest, and replay dry-run recipes do not access credentials or
hardware. The current declaration is historical; these checks do not make it
safe to apply. Keep bootstrap disabled until a reviewed declaration matches a
complete read-only capture of the live interface, DHCP, VIP, service, and
firewall policy state.

Bootstrap performs a factory reset and disrupts the network. It must not be run
casually. Review a complete declaration and schedule an authorized change
window before re-provisioning.

## ESP32-C6 Wi-Fi test contract

The test requires a 2.4 GHz WPA2-PSK WLAN bridged to WiFi-IoT. SSID,
passphrase, gateway, and address plan come from protected configuration.
