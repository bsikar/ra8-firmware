# scripts/secrets

Everything here touches key material. The dividing line that governs the whole
directory:

> **`infra/` deploys the vault. `scripts/secrets/` operates it.**
> A playbook that handles a root token can log a root token, so no playbook
> holds one. These scripts run in the operator's hands, on the node, and read
> their credentials from files the operator owns outside any checkout.

Nothing in this directory contains a secret, and nothing in it may. Values
arrive from stdin, from an operator-owned file, or from OpenBao itself.

## OpenBao

The vault every other part of the rig reads its credentials from at run time.
`infra/ansible/roles/openbao` deploys it and stops; the three steps below
produce or handle secrets and are therefore manual, in this order.

### 1. Initialise (once, ever)

```sh
kubectl exec -n openbao openbao-0 -- bao operator init -format=json \
  > ~/.openbao/init.json
chmod 600 ~/.openbao/init.json
```

`init.json` holds the Shamir unseal keys **and** the root token. It is the one
file whose loss is unrecoverable -- that is what a Shamir seal means, and no
backup of the vault's data substitutes for it. Keep it at mode 0600, outside
every checkout, and back it up somewhere that is neither this repository nor the
same machine.

It must never be committed. `infra/.gitignore` and the repo's pre-commit gates
are a safety net, not the control: the control is that it lives in `~/.openbao`
and nothing copies it out.

### 2. Unseal (after every restart)

```sh
/bin/bash -p scripts/secrets/openbao_unseal.sh
```

A Shamir-sealed OpenBao comes up **sealed** after every pod or node restart, by
design. The `openbao` role reports the seal state rather than treating it as a
failure, and this is the routine follow-up.

### 3. Configure a secret path, its policy and its AppRole

```sh
/bin/bash -p scripts/secrets/openbao_configure.sh <secret-path> <policy-name> <role-name> \
  < values.env > approle.env
```

Idempotent: it mounts KV v2 if absent, writes the secret from the `KEY=VALUE`
lines on stdin, writes a read-only policy scoped to exactly that path, enables
AppRole if absent, and binds a role to the policy. The emitted `ROLE_ID` /
`SECRET_ID` go to **stdout** and every status line to **stderr**, so a redirect
captures the credentials and nothing else.

The consumer then reads them through `openbao_client.py` with a
`~/.config/hil/openbao.env` (mode 0600) naming `BAO_ADDR`, `BAO_KV_MOUNT`,
`BAO_SECRET_PATH`, `ROLE_ID` and `SECRET_ID`.

Peer onboarding -- a `userpass` login plus a per-person AppRole scoped to a
read-only policy -- is deliberately not here. It is a rare, interactive,
one-person-at-a-time operation whose output is a password, so it stays a
hand-run script on the vault node; the two above are the ones the rig cannot be
rebuilt without.

## Root of Trust

`rot_provision.sh` and `rot_keystore.py` handle the firmware signing key. See
the module docstrings; the private key's location and its OpenBao backup are
recorded with the key ceremony, never here.
