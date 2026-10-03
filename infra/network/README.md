# Network bench tooling

This directory contains offline declaration checks and an operator-driven
console utility. `fortigate-bench.example.conf` is a synthetic fixture using
TEST-NET address ranges and locally administered placeholder MAC addresses;
it is never a deployment declaration.

Live modes require `RA8_FORTIGATE_CONF` to name a private declaration file.
There is no tracked declaration fallback. The conventional in-repository
private path `infra/network/fortigate-bench.conf` is ignored by Git. Bootstrap
refuses to start when the environment path is unset or its declaration is
invalid.

## Offline checks

These commands use only the safe example fixture and do not access credentials
or hardware:

```sh
just infra::fortigate_config_selftest
just infra::fortigate_config_lint
just infra::fortigate_replay_dry_run
```

Live operations use the sanitized Just entrypoints:
`just infra::fortigate_bootstrap`, `just infra::fortigate_ap_configure`, and
`just infra::fortigate_verify`. Bootstrap performs a factory reset. Run a live
operation only under the applicable bench change procedure.
