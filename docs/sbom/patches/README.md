# Third-party patch provenance

Every intentional change to upstream third-party bytes has an ordered,
numbered patch series registered in [`registry.toml`](registry.toml). The
offline `check_third_party_patches.py` gate rejects unregistered patch files,
undeclared targets, misleading metadata classifications, and bytes that the
registered series cannot reconstruct exactly.

## Delivery models

- **Vendored dependencies** keep ready-to-build patched bytes in their declared
  SBOM registry path. Ordinary builds never apply these patches. The gate copies
  each patched target, reverses the series to the upstream Git blob recorded in
  `docs/sbom/upstream/<component>.manifest`, reapplies it, and requires exact
  equality with both the manifest's local blob and the checked-in file.
- **Fetched dependencies** are checked out at the full commit in their pin file,
  then their build entry point applies every filename in `patches/series` in
  order. The offline gate proves that the pin, series, and application code are
  connected; the build performs `git apply --check` against the fetched pin.

`functional` patches change runtime or build behavior. `metadata` is deliberately
narrow: it may change only `.gitattributes`, `.gitignore`, or `.gitmodules`.
First-party integration files and generated configuration headers belong in the
SBOM registry's `local_files`; they are not represented as fictitious upstream
patches.

## Updating a dependency

1. Resolve and record the immutable upstream pin. Obtain the upstream target
   bytes and verify their Git blob IDs through `check_soup_upstream.py`.
2. Rebase each deliberate change onto those bytes. Keep patches focused;
   formatter sweeps and unrelated comment churn in vendored source are not an
   acceptable normalization step.
3. Generate `0001-description.patch`, `0002-description.patch`, and so on,
   list them in `series`, and register their review classification.
4. For vendored code, apply the series before checking in the ready-to-build
   bytes and refresh the corresponding upstream manifest. For fetched code,
   keep the series application in the build entry point after checkout of the
   pin and before compilation.
5. Run the offline gate and its mutation self-test. Network verification is a
   separate maintainer action; normal builds and CI provenance checks remain
   offline and reproducible.

The registry derives vendored locations from `scripts/gen/sbom_registry.py`.
That includes a future explicitly registered `tools/<tool>/third_party/<dep>`
component without teaching this checker a new hard-coded root.
