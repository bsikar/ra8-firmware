# Proxmox lab security gate

This runbook is part of the Terraform review. It is intentionally separate
from the resource definitions because Terraform cannot prove that a Proxmox
bridge, switch VLAN, firewall, template, or API ACL is safe.

## Current safe state

- `lab_enabled` is `false`.
- The VM NIC is disconnected and the LXC NIC is disabled.
- No API endpoint or API token is present in the repository or exported
  environment. Credentials belong in a protected local or runtime secret
  store; do not document their storage location here.
- Terraform reads the Proxmox token from OpenBao through a dedicated,
  read-only AppRole and an ephemeral KV v2 value. The AppRole cannot create
  child tokens, and the Terraform wrapper supplies its credentials only at
  runtime.
- Keep Proxmox TLS verification enabled. `proxmox_insecure=true` is permitted
  only for a deliberate, read-only smoke test when the private endpoint's CA
  is not available to the provider; never use it for an apply.
- `vmbr9` is configured persistently as a host-only bridge with no physical
  port, address, or gateway, but it is not currently active because host
  networking has not been reloaded.
- `ra8-tf-lab` is a dedicated datastore with capacity appropriate for the full
  test matrix and a separate storage boundary from production. Keep exact
  disk names, capacities, hostnames, and credential identifiers in local
  operator notes rather than this repository.
- The token must be scoped to the lab pool, dedicated lab datastore, and node
  audit. Its datastore privileges should be limited to audit and guest-space
  allocation; it must not create or modify storage definitions.
- No current guest occupies the reserved `9000-9099` ID range.

Do not change these defaults as part of a routine Terraform test.

## Threat model

There are four separate compromise paths:

1. A malicious or vulnerable template executes code inside the guest.
2. A guest compromise reaches the personal LAN or Proxmox management plane.
3. A stolen or over-privileged API token changes production resources.
4. A provider or local Terraform process is compromised and uses the token.

`prevent_destroy`, pool names, VM-ID ranges, and Terraform validation reduce
operator error. They are not substitutes for network isolation or Proxmox ACLs.

## Network gate

Before enabling a guest NIC, verify all of the following outside Terraform:

- `vmbr9` is a dedicated bridge or VLAN and is not `vmbr0`, `vmbr1`, or
  any other production bridge.
- `ra8-tf-lab` is a dedicated datastore with an explicit capacity and physical
  disk boundary; do not use the production datastore as a fallback.
- For a host-only lab, the bridge has no physical port (`bridge-ports none`),
  no host address, and no gateway. Provide a dedicated lab DHCP service only
  if the guest needs an address.
- For a VLAN-backed lab, the switch/firewall places the VLAN in a separate
  security zone. Do not rely on a VLAN tag alone if the switch permits routing
  between the lab and personal zones.
- Guest traffic cannot reach the personal LAN, storage networks, Proxmox API
  (`8006`), Proxmox SSH (`22`), cluster services, or the host filesystem.
- Any internet/package access goes through an explicit, default-deny egress
  policy or disposable NAT gateway. Do not bridge the guest directly to the
  home/personal LAN.
- IPv6 is either isolated by the same policy or disabled for the lab guest;
  the Terraform resources explicitly request manual IPv6 configuration.
- Do not reload host networking merely to activate this bridge until the
  dedicated datastore and first plan have been reviewed; the bridge can stay
  inactive while the control plane is prepared.

Proxmox describes a bridge with a physical port as a virtual switch to the
underlying network, and documents `bridge-ports none` for private guest
networks. See the [Proxmox network configuration guide](https://pve.proxmox.com/wiki/Network_Configuration).

## API credential gate

Create the credential only after the network gate passes:

- Use a dedicated service user and a separate API token with privilege
  separation enabled.
- Grant access only to the lab resource pool, approved lab templates, one
  `ra8-tf-lab` storage target, and the target node's minimum audit information.
- Use a custom least-privilege role limited to VM allocation, cloning, audit,
  CPU/memory/disk/network/options/cloud-init configuration, power management,
  pool audit, and the minimum datastore audit/allocation privileges. Do not
  use `PVEVMAdmin` or any cluster-wide administration role.
- Do not grant permissions at `/` or use `PVEAdmin`, `Sys.Modify`,
  `Permissions.Modify`, `SDN.*`, cluster/HA administration, host firewall
  administration, device passthrough, or arbitrary storage access.
- Verify both the backing user and the token's effective permissions before
  using the token. The token must be weaker than the backing user.
- Keep the token in a protected runtime environment, never in `.tfvars`, shell
  history, CI logs, Terraform plans, or this repository. Revoke it after the
  experiment if it is no longer needed.
- Treat the OpenBao AppRole credentials as sensitive runtime credentials too;
  rotate them independently from the Proxmox API token.

Proxmox documents that API-token privileges are a subset of the backing user's
privileges and supports privilege-separated tokens; see the [Proxmox API token
documentation](https://pve.proxmox.com/pve-docs/pve-admin-guide.pdf).

## Template gate

- Use a newly reviewed, minimal QEMU template; do not clone a production VM or
  a template containing credentials, private keys, host mounts, startup hooks,
  management agents, or unknown services.
- Record the template ID and provenance in the local ignored variables file.
- The disposable CI driver performs a read-only check for the Linux template
  name/ID pair `ra8-lab-debian-template`/`9001` and the description marker
  `RA8_LAB_TEMPLATE=linux-ci-v1`; it does not modify or delete that template.
- Prefer the QEMU VM path. The LXC path requires `allow_lxc = true` because an
  unprivileged LXC still shares the Proxmox host kernel.

## Terraform gate

1. Run `terraform fmt -check -recursive infra/terraform`.
2. Run `terraform -chdir=infra/terraform/environments/lab validate`.
3. Confirm the saved plan contains only a new, stopped lab guest with a
   reserved ID, the dedicated pool, the dedicated bridge, and expected small
   resource allocations.
4. Confirm the plan does not create or modify a bridge, VLAN, firewall, node,
   storage definition, existing guest, hook, mount, or passthrough device.
5. Apply only a reviewed saved plan; do not use `-auto-approve`.
6. Keep the NIC disconnected for the first boot. Enable it only after a
   separate network review and a second plan review.

The disposable CI driver additionally requires both
`RA8_LAB_NETWORK_APPROVED=1` and `RA8_LAB_EGRESS_APPROVED=1` before it
requests a started/connected guest. Those variables do not create or alter
the bridge, DHCP, NAT, firewall, or routing boundary.

The first plan should be treated as a credential and infrastructure boundary
test, not as a normal deployment.

## Disposable CI cleanup gate

`just infra::lab::ci linux` uses only guest ID `9000`. It never targets the
template IDs `9001` or `9010`, production guests, LXCs, storage definitions,
or the host network. Before clearing Proxmox protection and purging a guest,
the cleanup trap verifies the exact run ID in the description and tag, the
`ra8-lab-linux-*` name, the `ra8-tf-lab` pool/storage, non-template status, and
stopped state. A mismatch leaves the object and its temporary state for manual
review. The Windows profile currently refuses before Terraform apply.

## Windows template gate

The Windows Server path is not an ISO installer. It clones a reviewed amd64
template and assumes that Cloudbase-Init and the intended WinRM policy were
prepared before the template was sealed. Do not place a Windows password,
certificate private key, WinRM secret, or unattested answer file in Terraform.
Keep those values in OpenBao and have the later Ansible bootstrap retrieve them
at runtime.

Before enabling `lab_windows_vm`, verify that the template:

- Has only the expected Windows Server installation and VirtIO drivers.
- Has Cloudbase-Init configured to consume the Proxmox metadata disk on `ide2`.
- Has WinRM restricted to the isolated lab network and uses the chosen secure
  authentication mode.
- Has no production domain membership, tokens, user profiles, or private keys.
- Is stopped, protected, and assigned to the dedicated lab pool/storage.
