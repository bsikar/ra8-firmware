# Persistent ra8ci control VM

This is a separate Terraform root for the protected ra8ci/PostgreSQL control
VM. It does not call the disposable lab modules, register a GitHub runner, or
run repository jobs. `service_enabled` defaults to `false`; the first enabled
plan is stopped, protected, and disconnected. VM IDs 9000-9099 belong to the
disposable lab and cannot be selected here.

The root deliberately has no endpoint, network, storage, template, VM ID,
backend, or backup destination in source. The operator must approve and supply
each exact value. The `network_approved` and `off_vm_backup_approved` values
are attestations, not proof that a bridge, switch, firewall, or destination is
safe. Verify those boundaries outside Terraform. No Tailscale or personal
network discovery is used.

## Required before a plan

1. Review an unused VM ID >= 9100, dedicated persistent pool, approved
   Debian/Ubuntu cloud-init template, node, boot and data datastores, and a
   private management bridge/address. Verify the bridge has effective
   default-deny firewall rules: only operator clients and enrolled agents may
   reach the mTLS API; guests cannot reach the Proxmox API, database socket,
   host SSH, storage segment, or personal LAN. Terraform `firewall = true`
   does not establish those host/guest firewall rules by itself.
2. Provision a dedicated privilege-separated Proxmox API token. Its backing
   user and token must have only the reviewed service pool, source template,
   target datastores, and node audit/configuration privileges. Neither gets
   permissions at `/`, arbitrary storage administration, host networking,
   firewall administration, or VM deletion. Store the token in a dedicated
   OpenBao KV v2 path; the AppRole may read only that path.
3. Approve an encrypted off-VM backup destination, retention, and a separate
   restore VM. Provide a reviewed remote S3-compatible state backend through
   runtime `-backend-config` values. Never initialize production with
   `-backend=false`, local state, embedded credentials, or a backend path under
   the guest being created. Protect saved plans because provider-derived data
   and topology can still be sensitive.
4. Compile the exact ra8ci commit for the guest architecture, record its
   SHA-256, and supply reviewed server/client CA and certificate materials to
   the separate Ansible playbook. The template must contain no credentials or
   personal-network agent. A cloned boot disk must already be bootable; the
   additional `scsi1` disk is blank and has serial `RA8CIDATA` for identity
   verification by Ansible.

Use `terraform init` with a reviewed encrypted S3 backend configuration, then
`terraform validate`, `terraform plan -out=<protected path>`, and inspect the
saved plan. Apply only that exact reviewed plan. Never use `-auto-approve`.
The S3 backend credentials and OpenBao AppRole inputs come from protected
runtime injection, not checked-in tfvars or shell history. Set
`service_enabled=true` only after all inputs are known. Keep
`start_after_review=false` for the first plan; enable startup/network only
after the separate firewall and guest-bootstrap review.

`prevent_destroy` and Proxmox `protection` intentionally prevent routine
teardown. Disaster recovery is a manual, identity-checked procedure: reconcile
VM ID, name, pool, node, disks, state, and database/board generations before
any change. A missing Terraform state is not authorization to create a second
control VM or delete an apparently orphaned one.
