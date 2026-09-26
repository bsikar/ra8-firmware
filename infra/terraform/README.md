# Terraform lab scaffold

This directory is intentionally limited to disposable Proxmox lab guests. It
does not manage the Proxmox host, host networking, storage pools, firewall
rules, production guests, or application services.

The first environment is `environments/lab`. It is disabled by default and
requires an explicit lab-only network bridge, template IDs, and resource
values before it can create anything. It has separate Linux and Windows
paths: `lab_linux_vm` is the existing Linux amd64 path, while `lab_windows_vm` is a
disabled-by-default Windows Server amd64 path. The checked-in example contains
no real endpoint, address, username, token, password, or private key.

The disposable CI lifecycle is exposed through the repository root:

```text
just infra::lab::setup [target]
just infra::lab::check
just infra::lab::ci linux
just infra::lab::ci windows
```

The CI driver uses a run-local Terraform state file, a localhost-only API
tunnel through the `pve` SSH alias, and a temporary SSH key. It creates only a
run-marked Linux guest, invokes the dedicated lab Ansible playbook, runs the
committed `HEAD` snapshot (including checked-out submodules) through `scripts/ci/devcontainer_run.sh -- just ci`,
and removes the matching guest in an exit trap. `--keep` is an explicit
debugging exception. Windows and `both` are intentionally rejected until the
Windows template, WinRM boundary, and Windows CI command are reviewed.

## Safety contract

- `lab_enabled` defaults to `false`.
- Terraform defaults guests to stopped and with `on_boot = false`.
- Terraform defaults their network interface to disconnected/disabled. The
  lifecycle driver can set `started` and `network_enabled` only after its two
  explicit operator gates are present; it never creates DHCP/NAT or edits
  `vmbr9`.
- Guest protection is enabled in Proxmox and Terraform `prevent_destroy` is
  enabled.
- Enabled guests must use `ra8-lab-*` names, reserved guest/template IDs in
  the 9000-9099 range, and the exact dedicated `ra8-tf-lab` resource pool.
- Full clones must target the exact pre-created `ra8-tf-lab` datastore; the
  configuration never falls back to a production datastore.
- `ra8-tf-lab` must be a pre-created dedicated datastore sized for the full
  test matrix and physically/logically separated from production storage. Keep
  host disk names, capacities, and other deployment-specific details in local
  operator notes, not in this repository.
- The VM is capped at 4 vCPUs/8192 MB and the optional container at 2
  vCPUs/4096 MB; both have a 10 MB/s interface limit.
- Enabled guests must use the exact `vmbr9` lab bridge. That bridge must be
  pre-created outside this Terraform configuration with no physical bridge
  port and no route to the personal LAN, or be an explicitly isolated VLAN.
  The name check is an additional guard, not proof that the host network is
  safe.
- The configuration accepts a pre-existing `vmbr9` bridge; it never creates or edits
  a bridge, VLAN, firewall, storage pool, or host setting.
- There are no shell, SSH, Ansible, or `local-exec` provisioners.
- No cloud-init commands, hooks, device passthrough, bind mounts, or nested
  container features are configured.
- Guests use DHCP with no configured gateway and explicit manual IPv6 until
  the isolated network is reviewed, and require SSH public keys rather than a
  configured password.
- The VM should be the first and preferred isolation boundary. The optional
  LXC module remains disabled by default and must receive a separate review
  because an LXC shares the Proxmox host kernel even when unprivileged. It
  additionally requires the explicit `allow_lxc = true` gate.
- The Windows path clones only from a prebuilt Windows Server amd64 template
  with VirtIO drivers, Cloudbase-Init, and a reviewed WinRM policy. Terraform
  does not download Windows media, run Sysprep, inject a password, or enable a
  Windows network interface.
- Windows credentials belong in OpenBao and the later Ansible inventory path;
  they must not be placed in Terraform variables, state, plans, or templates.
- The Proxmox API credential is read from OpenBao through a dedicated,
  read-only AppRole and an ephemeral KV v2 value; it is not stored in this
  repository or Terraform state.
- Proxmox TLS verification remains enabled by default. The `proxmox_insecure`
  switch is only for a deliberate, read-only smoke test against a private
  self-signed endpoint and must remain `false` for normal plans and applies.
- OpenBao AppRole credentials are supplied at runtime from protected local
  secret storage. The `run-with-openbao.sh` wrapper handles this without
  putting secret values in command arguments or shell history.
- Terraform state is local and ignored by Git; keep it on encrypted storage
  and treat it as sensitive operational data. Git-ignored does not mean safe
  to share.
- CI runs require `RA8_LAB_NETWORK_APPROVED=1` and
  `RA8_LAB_EGRESS_APPROVED=1` in the protected operator environment. The
  latter acknowledges controlled package/image egress; it is not permission
  to bridge the guest onto a personal LAN.
- There is no production environment configuration here.

The provider is pinned to the 0.83 line for the initial Proxmox VE 8
compatibility target. Terraform 1.10 or newer is required for the ephemeral
OpenBao KV read, and the dependency lock file is kept alongside this
configuration for review. Commit it with the Terraform configuration only
after review. Provider compatibility must be checked in an isolated lab
before any apply; this scaffold has been initialized and validated locally,
and a no-resource OpenBao-backed plan has passed, but no guest-creating apply
has run.

Read [SECURITY.md](SECURITY.md) before creating credentials or enabling a
guest network.

## Provider credentials

The wrapper supplies these runtime values. Do not put credentials in `.tfvars`
files:

```text
TF_VAR_proxmox_endpoint
TF_VAR_openbao_address
TF_VAR_openbao_role_id
TF_VAR_openbao_secret_id
```

Run Terraform through the wrapper so the Proxmox credential comes from
OpenBao:

```text
infra/terraform/run-with-openbao.sh validate
infra/terraform/run-with-openbao.sh plan
```

The OpenBao AppRole is read-only on the single Terraform KV path and cannot
create child tokens. The Proxmox API token itself is a dedicated,
privilege-separated, least-privilege token for the lab only.
The provider configuration does not enable SSH because the selected resources
use API-backed operations.

## Intended lifecycle

1. Review the configuration as text.
2. Populate runtime variables through `run-with-openbao.sh`; keep endpoint and
   AppRole values out of checked-in variable files.
3. Run formatting and validation locally.
4. Independently verify the API token ACL, pool, templates, storage, and
   bridge/VLAN before any API-backed plan.
5. Run a plan against an isolated Proxmox lab only.
6. Review the plan for guest IDs, storage, bridge, and resource limits.
7. Apply only after an explicit operator decision.

For the disposable CI path, use `just infra::lab::ci linux`. The driver first
checks the reviewed Linux template marker `RA8_LAB_TEMPLATE=linux-ci-v1`,
creates a 16-hex run marker, and saves Terraform state under a private
temporary directory. Cleanup refuses to act unless the guest ID, name,
description marker, run tag, pool, storage, and stopped state all match. The
Linux template is inspected but never modified or deleted.

Production guests are deliberately outside this first environment. A future
production environment would require a separate review and a separate state
file; it must not be added by copying the lab variables.

## Supported Proxmox guest profiles

The current Linux fixture is Debian 12 amd64 (`template_vm_id = 9001`, VM
9000). Additional Linux distributions can use the same `lab_linux_vm` module after
their templates are reviewed; keep each template and guest in the reserved
9000-9099 range.

The Windows profile expects a prebuilt Windows Server amd64 template. Before
enabling `lab_windows_vm`, prepare and review a template with:

- Windows Server with the selected Desktop Experience or Server Core choice.
- VirtIO storage/network drivers installed and verified.
- Cloudbase-Init configured for Proxmox metadata and the intended local
  management account.
- WinRM configured for the lab security boundary, with no broad LAN trust.
- No production credentials, SSH keys, host mounts, passthrough devices, or
  unknown startup tasks.

The first Windows enablement should remain stopped and disconnected, just like
the Linux fixture. Windows-specific Ansible bootstrap and credentials are a
separate follow-up from the Terraform clone.

## Threat model

Terraform runs a third-party provider process locally with the API credential.
The provider is therefore pinned and locked, and the credential must be
privilege-separated and scoped to the lab pool. The guest template is also a
trusted-code boundary: verify its provenance before cloning it. A compromised
guest must have no path to the personal LAN, Proxmox management endpoint, or
host filesystem. Network isolation is enforced by the pre-created Proxmox
bridge/VLAN and firewall policy, not by Terraform alone.
