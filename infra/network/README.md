# Network bench tooling

This directory contains offline declaration checks and an operator-driven
console utility. `fortigate-bench.example.conf` is a synthetic fixture using
TEST-NET address ranges and locally administered placeholder MAC addresses;
it is never a deployment declaration.

Live modes load their declaration from `RA8_FORTIGATE_CONF`, defaulting to
`~/.config/ra8/fortigate-bench.conf`. Keep that private file out of the
repository and restrict it to the operator. Bootstrap refuses to start when
the private declaration is missing or invalid.

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
