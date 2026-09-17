# Native macOS Host Builds for the Zig Applications

The host-side Zig programs in this repository build and run natively on an
arm64 Mac. This page records why a plain `zig build` used to fail there, what
the build graph does about it now, and how to reproduce and check the result on
a real machine. Issue #899 tracks the work.

## Why the native link used to fail

Zig links Mach-O binaries against a text-based stub library rather than a real
`libSystem.dylib`. On a machine with the Command Line Tools installed, the
active SDK ships `usr/lib/libSystem.tbd`, and its `targets` list names:

    x86_64-macos, x86_64-maccatalyst, arm64e-macos, arm64e-maccatalyst

There is no `arm64-macos` entry. Zig 0.14.1 resolves a *native* target on an
Apple silicon host to the `aarch64-macos` (`arm64-macos`) triple, finds no
matching slice in the SDK stub, and resolves none of the libSystem symbols, so
every host executable fails to link. The same source cross-compiles cleanly
from Linux, because a non-native target query makes Zig fall back to its own
bundled `libSystem.tbd`, which does declare `arm64-macos`.

The distinction is the target query, not the host: an explicit `-Dtarget=` (or
any query carrying an explicit OS tag) is non-native, and non-native builds use
the bundled stub.

## What the build graph does

`tools/zig_build` is a path dependency shared by the Zig build roots. It
probes the active SDK with `xcrun --show-sdk-path`, parses the `targets` list
out of `usr/lib/libSystem.tbd`, and decides between two outcomes:

* the stub declares `arm64-macos`, so the native query is kept; or
* it does not, so the root pins an explicit `aarch64-macos` query and Zig uses
  its bundled stub.

An unreadable SDK, or any non-macOS host, falls through to the native query.
Each root feeds that decision into `b.standardTargetOptions` as its default
target, so the rule applies to a bare `zig build` and to `zig build test`.

Two escape hatches override the probe:

* `-Dtarget=<triple>` always wins; an explicit target is used verbatim.
* `-Dmacos-libsystem=sdk|bundled` forces one side of the decision. `sdk` keeps
  the native query even when the probe says it cannot link, which is how you
  reproduce the original failure on a fixed machine. `bundled` pins
  `aarch64-macos` even when the SDK stub looks usable.

The build roots that carry the rule are `tools/zig_build`, the host
applications under `apps/host`, `apps/host/firmware_pipeline/zig`,
`tests/abi_chain_fixture`, `tests/rust_abi_fixture/zig`, and
`tests/zig_abi_fixture`. The Zig check runs `zig build test` in every build
root, so a single unwired root is enough to break the gate on a Mac.

## The pinned target keeps the host's macOS version

Pinning `aarch64-macos` is a stand-in for the native build, so it has to agree
with the native build about more than the architecture. A target query that
names an OS but no version takes Zig's default range for that OS, whose floor
is several releases below any Apple silicon Mac. Cross-compiled from Linux with
no version, the binary comes out with:

    LC_BUILD_VERSION  minos 13.0.0

A native build on the same Mac would have stamped the version the machine is
running, and would have answered `Target.Os.isAtLeast` against it. So the
pinned query carries the host version across: the build runner is compiled for
the native target, `builtin.os.version_range` therefore already holds the
detected running version, and `hostMacosVersion()` reads it back and pins it as
both ends of the range. The same build with a version named comes out as:

    LC_BUILD_VERSION  minos 26.0.0

A reading below macOS 11 is discarded rather than pinned, because no Apple
silicon Mac runs anything older, and pre-release and build metadata are dropped
because a deployment target has no use for them. Off macOS there is no host
version to carry and the query is unchanged, which is why an explicit
`-Dtarget=aarch64-macos` from Linux still gets Zig's default floor. Name the
version yourself (`-Dtarget=aarch64-macos.26.0`) to reproduce what a Mac would
pin.

## Reproducing on an arm64 Mac

Run these from a checkout on an Apple silicon machine with the pinned Zig
version on `PATH`. The first two commands report the environment the decision
depends on:

    zig version
    xcrun --show-sdk-path
    grep -n 'targets:' "$(xcrun --show-sdk-path)/usr/lib/libSystem.tbd" | head

Then build and test a representative root three ways:

    cd tools/zig_build && zig build test
    cd ../../apps/host/image_pyramid && zig build && zig build test
    zig build test -Dmacos-libsystem=bundled
    zig build test -Dmacos-libsystem=sdk

The default run and the `bundled` run are expected to pass. The `sdk` run is
expected to fail with unresolved libSystem symbols on any machine whose SDK
stub omits `arm64-macos`; that failure is the bug this work routes around, and
it is the check that the probe is looking at the right thing. A machine whose
SDK stub does declare `arm64-macos` passes all four, and the probe keeps the
native query there.

Confirm the emitted binary is a native arm64 Mach-O rather than a cross
artefact:

    file zig-out/bin/image_pyramid

## What a Linux checkout can and cannot show

Cross-compiling from Linux exercises the graph, the target selection, and the
Mach-O link path:

    zig build -Dtarget=aarch64-macos
    zig build test -Dmacos-libsystem=bundled

Both compile and link `aarch64-macos` binaries. The test binaries are compiled
and linked too, and their *run* steps are then reported as skipped, because a
Linux host cannot execute a Mach-O arm64 binary:

    Build Summary: 2/3 steps succeeded; 1 skipped
    +- zig test Debug aarch64-macos success
    +- run test skipped

That is the intended outcome, and it is what makes the command usable as a
link check off a Mac: the compile and link are real, the run is honestly
reported as not having happened. A skipped run is never a passed test, so read
the summary line rather than the exit status when the build root is
cross-configured. On an arm64 Mac the pinned `aarch64-macos` target is native,
nothing is skipped, and the tests run.

A Linux checkout cannot exercise the `xcrun` probe, the SDK stub parse against
a real `.tbd`, the `sdk` failure mode, the host version carry described above,
or any behaviour of the produced binaries. Those need a real Mac.

## Keeping the rule applied to new build roots

The rule is only worth having if every Zig build root follows it, and a root
added later is exactly where it would be forgotten: nothing about a plain
`b.standardTargetOptions(.{})` looks wrong from Linux, and the failure only
appears on someone's Mac.

So `scripts/checks/check_zig.py --test` enforces it structurally. For every
discovered build root, either

- `build.zig` takes its default target from
  `ra8_build.hostDefaultTargetQuery(b)`, or
- the root declares an exemption in `.zig-host-target.json`:

      {"rule": "exempt", "reason": "cross-compiles for ARM targets only"}

Anything else is a finding that names #899. Comments are stripped before the
wiring is read, so a mention of the helper in prose cannot satisfy the rule.
A root that declares `{"rule": "host_default"}` must really carry the wiring;
the declaration on its own is not accepted. `--selftest-test` proves both
directions.

An exemption is the right answer for a graph that never produces host
binaries, such as a firmware-only cross-build root. It is the wrong answer for
a host tool, which should carry the rule instead.

## Not yet automated

No scheduled job runs these commands on macOS today; the checks above are
manual. Automating them needs an arm64 macOS runner and a gate registered in
`scripts/ci.sh`, tracked under #899.
