# Protected control VM convergence

`site.yml` installs the verified `ra8ci` binary and colocated PostgreSQL on a
reviewed Debian/Ubuntu VM. It is not a CI runner playbook. It has no GitHub
runner registration, checkout, repository shell execution, Proxmox host SSH,
Tailscale, or automatic VM lifecycle operation. Run only against an exact
`ra8ci_control` inventory host after its VM identity, management address, and
SSH host key are pinned. Do not use the disposable lab inventory.

No agent TLS private key or task checkout is placed on this VM. The current
embedded task catalog still contains shell-outs to checkout-controlled
scripts, so untrusted pull-request dispatch must stay disabled until those
checks are absorbed or a reviewed guest sandbox prevents task code from
reading its agent key. The control VM runs only the server and database.

The playbook requires these out-of-band inputs:

- `ra8ci_deployment_approved`, `ra8ci_network_approved`, and
  `ra8ci_off_vm_backup_approved` must all be true after the corresponding
  reviews. The first converge creates a full backup but leaves the ra8ci
  server stopped. Only after a successful restore into a separate VM should
  the operator set `ra8ci_restore_drill_approved=true` and rerun the playbook
  to start the API. That value is not set by this playbook.
- `ra8ci_data_device` is the exact `/dev/disk/by-id/...` path of the separate
  `RA8CIDATA` disk; `ra8ci_data_disk_serial` must be `RA8CIDATA`.
  `ra8ci_initialize_blank_disk=true` is required only on a brand-new, empty
  disk. Existing signatures other than ext4 or any partition are rejected.
- `ra8ci_postgres_major` names the reviewed distribution package major.
  The playbook refuses an existing unrelated PostgreSQL cluster.
- `ra8ci_binary_src`, `ra8ci_binary_sha256`, `ra8ci_tls_cert_src`,
  `ra8ci_tls_key_src`, `ra8ci_client_ca_src`, and `ra8ci_listen_addr` are
  controller-supplied reviewed artifacts/settings. The binary is hashed both
  before and after transfer. The listener must bind the approved management
  address; host/firewall policy must limit 8443 (or the chosen port) to
  authorized mTLS clients.
- `ra8ci_board_agent_keys_src` is a reviewed JSON keyring with schema version
  1 and an `agents` array of `{ "key_id", "public_key_base64" }` entries. Each
  public key is a 32-byte Ed25519 key from a trusted board agent; provision it
  from the board-agent trust process, never from runner jobs. The server
  installs it root-owned and read-only to its service identity, and refuses to
  start if the allowlist is absent, writable by group/others, malformed, or
  contains duplicate/invalid keys. Rotation should overlap old and new public
  keys until outstanding board challenges have expired.
- `ra8ci_terraform_state_key_b64` is the strict Base64 encoding of a dedicated
  32-byte AES-256 key. Source it from the control-plane's protected secret
  manager, not from an inventory or command-line extra-vars. The server uses
  it to encrypt Terraform state before PostgreSQL writes; retain this key
  independently of the encrypted PostgreSQL backups or state is unrecoverable.
- `ra8ci_backup_s3_endpoint`, `ra8ci_backup_s3_bucket`,
  `ra8ci_backup_s3_region`, `ra8ci_backup_s3_key`,
  `ra8ci_backup_s3_secret`, `ra8ci_backup_s3_uri_style`,
  `ra8ci_backup_repo_path`, and `ra8ci_backup_cipher_pass` select a genuinely
  off-VM S3-compatible store. Inject secrets from protected controller
  storage with Ansible Vault or a reviewed secret plugin; never put them in
  an inventory, command-line extra-vars, repository, CI log, or Terraform
  state. TLS verification remains enabled.

The PostgreSQL cluster is created directly on the mounted dedicated disk.
PostgreSQL listens only on its Unix socket and peer-authenticates three
distinct local identities: `ra8ci_migrate` owns the schema, `ra8ci_service`
serves requests without DDL or history mutation, and `ra8ci_operator` is a
separate local identity for reviewed principal/fixture maintenance. Neither
the server nor any guest receives a migration or operator connection string.
`api_principals`, `api_grants`, `agents`, `board_fixture_profiles`, and
`schema_migrations` are runtime read-only. `audit`, `board_events`, and
`run_events`, `local_runs`, and `local_run_steps` are runtime append-only. A new migration that adds a mutable
table requires an explicit review of this playbook's write allowlist before
the server is restarted.

pgBackRest archives WAL continuously to the encrypted remote repository,
forces a WAL switch within 15 minutes, takes a daily full backup, and retains
at least 30 days of full-backup history. Its check timer runs every 15
minutes. A successful `pgbackrest check` and first full backup are required
before ra8ci starts. Monitor timer failures, archive lag, and S3 capacity
externally; a timer alone is not an alert. The playbook never restores over a
live cluster. A restore drill must stop the server on a separate isolated VM,
restore with pgBackRest, verify data and audit rows, and keep dispatch/board
grants drained until Proxmox guests and the board agent's generation
high-water are reconciled. The encrypted database backup includes the
Terraform backend-state ciphertext. Keep the application-layer state key and
the pgBackRest repository cipher key recoverable from separate protected
secret-manager paths; do not include either plaintext key in the backup repository.

Install the pinned collections from `requirements.yml`, then run
`ansible-playbook --syntax-check` and a reviewed `--check --diff` against the
exact inventory before apply. Review task output for secrets before sharing
it. A check run is not a substitute for a real restore drill or a reviewed
network/credential boundary.

The API process also verifies a signed readiness attestation at startup and
refuses to listen if its signature is invalid, the last `pgbackrest info`
check is older than 20 minutes, the latest completed full backup is older
than 48 hours, or the restore drill is older than 90 days. The `ra8ci
backup-monitor` oneshot runs as `postgres` with a separate Ed25519 signing
key; the API identity receives only the public key and read-only attestation.
The timer refreshes this evidence every 15 minutes from pgBackRest's JSON
output. The server identity cannot create or alter readiness evidence.

On first convergence, the playbook creates the key pair without replacing
existing files. After the isolated restore drill, an operator must set
`ra8ci_restore_drill_approved=true` and pass its actual UTC time as
`ra8ci_restore_drill_at` (`YYYY-MM-DDTHH:MM:SSZ`), then rerun the playbook.
The persisted receipt is not refreshed on later converges; a changed
approval or timestamp is rejected. The monitor creates an attestation before
the API is started. Revoking approval stops the API and its evidence timer.

