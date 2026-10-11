# Python Environments and Lock Maintenance

`scripts/dev/setup_python.sh` is the entry point for the locked project
environment and the git hooks. The Ansible, k3s, and compiler-image setup
paths live in the ra8ci repo now; this page covers the local `.venv` flow.

No setup path writes to the system Python. Do not use system `pip`, override
the PEP 668 system-package boundary, run an unpinned `uvx`, or pipe a download
directly into a shell.

## Supported hosts

The complete repository workflow supports Linux, macOS, and Linux under WSL2.
It requires a standard-library Python in the project range declared by
`pyproject.toml` (currently 3.11 through 3.14), network access for the first
bootstrap, and the native prerequisites documented in `TOOLCHAIN.md`.

The uv bootstrap also recognizes native Windows x86_64 and aarch64 so its
platform and checksum policy can be tested there. Native Windows is not a full
firmware build environment: use WSL2 for setup, compilation, and CI
parity. The POSIX setup scripts and embedded toolchains deliberately do not
claim native Windows support.

## Authorities and ownership

`pyproject.toml` is the only direct Python dependency authority and `uv.lock`
is the committed transitive resolution. Direct packages are declared exactly
once; dependency groups include one another instead of repeating pins:

- `hil`: bench camera, outlet, and USB libraries.
- `runtime`: shared script imports, including the HIL closure.
- `dev`: local and static-analysis tools, including the runtime closure.
- `ci`: the complete Python closure used by CI gates.
- `infra` and `k3s`: still declared, but their consumers (Ansible, the k3s
  node tooling) moved to the ra8ci repo; they stay until that rewire lands.
- `vela`: the offline Ethos-U model compiler.

The normal local environment is `.venv`. The authenticated uv archive and
binary are retained under the ignored `.tools/uv` cache so every reuse can be
reverified.

Authenticated execution has an OS-specific boundary. Linux executes uv from a
sealed anonymous memory descriptor, which also resists a same-UID process that
retained a file writer. macOS executes a digest-verified, read-only descriptor
through an unpredictable name in a held mode-0700 directory. The name remains
bound to the authenticated descriptor for the child lifetime and is reverified
before and after execution, then removed with a descriptor-relative unlink.
That rejects other UIDs, cache-path replacement, and symlink, hardlink, or name
races, but it cannot resist a malicious peer process under the same UID that
opens a writer or replaces the private name. The same peer could also rewrite
the checkout, lock, manifest, or caller before verification. Use the Linux
devcontainer or VM, or a separate OS account, when that stronger boundary is
required.

Managed environments have narrower ownership:

- `/opt/ra8-python-tools` contains the exact CI group on dev boxes and runner
  images. `RA8_TOOL_VENV` selects it and prevents a mounted checkout `.venv`
  from changing managed behavior.
- ESP-IDF owns its own upstream environment through the pinned C6 installer.
  That vendor environment is not part of this project lock and must not be
  converted to, or silently mixed with, a repository uv environment.

The devcontainer and CI image synchronize the committed `ci` group during the
image build. Their root build context is a fail-closed allowlist that includes
`pyproject.toml`, `uv.lock`, and the uv bootstrap authority; context digest
checks cover every allowed path, byte, and relevant mode.

## Adding or upgrading a dependency

Use Just for ordinary setup and checks. The direct uv commands below are for
maintainers changing dependency metadata:

1. Add or change one exact direct pin in the smallest owning group in
   `pyproject.toml`. Include that group elsewhere instead of copying the pin.
2. Resolve with the repository-authenticated uv while preventing interpreter
   downloads:

   ```sh
   UV_PYTHON_DOWNLOADS=never python3 scripts/dev/bootstrap_uv.py \
     --ensure-and-run --no-config lock
   ```

3. Run `scripts/dev/setup_python.sh` to rebuild `.venv`, then the relevant
   checks. (The managed k3s/HIL exports moved to the ra8ci repo with their
   roles; regenerate them there.)

Do not hand-edit `uv.lock` or either generated export. A changed lock and an
unchanged export is a policy failure, not an acceptable partial update.

## Updating the uv bootstrap pin

The one uv version and asset mapping live in `scripts/dev/uv_release.json`.
The bootstrap derives official GitHub release URLs from that manifest and
supports the checked-in Linux GNU/musl, macOS, and Windows x86_64/aarch64
matrix.

To update uv:

1. Select an exact official `astral-sh/uv` release and change the manifest
   version.
2. Obtain the official release checksum list through an authenticated review
   path. Independently verify and record the SHA-256 for every supported asset;
   do not execute an asset to discover whether it is trustworthy.
3. Keep asset names exact for their OS, architecture, and Linux libc mapping.
   Do not add a floating `latest` URL or a second version constant.
4. Run `python3 scripts/dev/bootstrap_uv.py --selftest`, then the pre-commit
   hooks.
5. Review the bootstrap and checksum manifest together. uv is host tooling and
   is recorded in `THIRD_PARTY_LICENSES.md`; it is not linked into firmware and
   does not belong in the firmware SBOM.

The bootstrap fails closed for unsupported platforms, ambiguous Linux libc,
unexpected archive members, symlinks, oversized payloads, and checksum or
version mismatches. `--print-path` is intentionally side-effect-free.
