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

Any non-macOS host keeps the native query. On an arm64 Mac an SDK that cannot
be read, or a stub whose target list cannot be parsed, pins as well: the
bundled stub is right for these libc-only tools either way, so an unknown SDK
must not reintroduce the link failure. Each root feeds that decision into
`b.standardTargetOptions` as its default target, so the rule applies to a bare
`zig build` and to `zig build test`.

Two stub generations are read. TAPI v4 carries `targets:` as a list of full
triples. TAPI v1 to v3 instead carry `archs:` plus a separate `platform:`, with
no triples anywhere:

```yaml
--- !tapi-tbd-v3
archs:    [ i386, x86_64, arm64, arm64e ]
platform: macosx
```

Both halves must agree before such a stub counts as declaring `arm64-macos`.

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

## Asking the graph what it decided, and why

```console
$ cd tools/zig_build && zig build explain-host-target
ra8 host target (#899)
  host:      aarch64-macos
  macos:     26.0.1
  selection: -Dmacos-libsystem=auto
  sdk:       /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
  stub:      /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib/libSystem.tbd
  finding:   the SDK stub lists its targets and arm64-macos is not among them (#899)
  decision:  pinned aarch64-macos, linking Zig's bundled libSystem stub
  deployment target: 26.0.1 (carried from the host)
```

(The transcript above is the shape of the output, not a reading taken from a
Mac: nothing in this tree has been run on Apple silicon yet.)

Four different findings all end in a pinned target, and they need different
fixes, so the graph names which one it saw rather than reporting them as one
state:

* the stub lists its targets and `arm64-macos` is absent -- this is #899 itself;
* the stub was read but declares no target list in a spelling the parser knows;
* an SDK was located but its `libSystem` stub could not be read;
* no SDK could be located at all, so `xcrun` is missing or failing.

`-Dmacos-libsystem=sdk|bundled` short-circuits the probe entirely, and the
report says so instead of attributing the forced choice to the SDK. The
`macos-host-build` CI gate prints this report before it builds anything, so a
red run carries its own diagnosis.

## What a forced selection reports

`-Dmacos-libsystem=sdk` and `-Dmacos-libsystem=bundled` change which stub the
build links. They do not change what is in the SDK stub on this machine, and
the probe still runs, so `zig build explain-host-target` prints both:

    selection: -Dmacos-libsystem=sdk
    sdk:       /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
    stub:      /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib/libSystem.tbd
    finding:   the SDK stub lists its targets and arm64-macos is not among them (#899)
    override:  -Dmacos-libsystem=sdk forced the native query, whatever the SDK stub says
    note:      this overrides the probe, which would have chosen the pinned aarch64-macos target
    decision:  native target, linking whatever stub the host resolves

`finding:` is always the reading of the machine; `override:` and `note:` say
what the force did to it. This matters most in the CI gate's informational
`-Dmacos-libsystem=sdk` leg, which exists to record what the SDK stub does on
that runner: reporting the forced choice as the probe's own conclusion is
exactly what would throw that observation away.


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

## Running the gate itself on your own Mac

    just quality::gate::run macos-host-build

`just quality::gate::run` sends a gate into the pinned Linux devcontainer on
macOS, because that is where the CI toolchain lives. A gate whose subject is
the host cannot be answered there: inside the container `macos-host-build`
sees Linux and refuses, which is the correct answer to the wrong question.
`scripts/ci/lib/native_host_gates.sh` lists the gates that must run natively
wherever they run at all, and the recipe asks it before routing, printing on
stderr why a gate skipped the container. Every other gate on macOS still goes
into the devcontainer, and on Linux nothing changed.

A gate added later that measures the host needs a row in that list, and its
own body must still refuse a foreign host; the list's `--selftest`, run by the
`ci-parity` gate, checks both, plus that every declared gate is really
registered in `scripts/ci.sh`.

### If the gate says your Mac is x86_64

`uname -m` reports what the *process* is, not what the *machine* is. Rosetta 2
translates a whole process tree, so a shell started from an x86_64 terminal
app, an x86_64 Homebrew, an IDE shipped as x86_64, or plain `arch -x86_64 zsh`
prints `x86_64` on an Apple silicon Mac. The gate used to read that as an
Intel host and tell the owner to go and find an arm64 runner, on the very
machine it needed.

`scripts/ci/lib/host_arch.sh` separates the two facts. Darwin publishes both:
`sysctl -n sysctl.proc_translated` is `1` when this process is translated, and
`sysctl -n hw.optional.arm64` is `1` on Apple silicon whatever the process is.
The gate still refuses under translation, because a translated `zig` links the
x86_64 path and cannot observe the missing `arm64-macos` slice at all, but it
now names Rosetta and prints the native re-run:

    arch -arm64 /bin/zsh -lc "just quality::local::gate macos-host-build"

A genuine Intel Mac answers neither sysctl and is still reported as
`Darwin/x86_64`, and an unreadable `sysctl` fails closed to whatever `uname`
said rather than promoting the host to arm64. Nothing changes on Linux, and a
native arm64 Mac is decided from `uname` alone without asking `sysctl`. The
matrix (Linux, native arm64, translated arm64, Intel, fallback signal, no
`sysctl` at all) is proved by `host_arch.sh --selftest`, which the
`toolchain-parity` gate runs.

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
cross-configured.

The excuse is scoped to a target the build host genuinely cannot execute, and
that scope is the point. On an arm64 Mac the pinned `aarch64-macos` target *is*
the host, so no run there is excused: a host test that cannot run fails the
build instead of vanishing from it. Excusing it unconditionally would let a Mac
on which the host tests cannot run exit zero with nothing in the status to say
so, and the `macos-host-build` gate's verdict is precisely that these tests run
natively on Apple silicon. `targetRunsOnBuildHost` in
`tools/zig_build/macos_host.zig` owns that comparison (same architecture, same
operating system, and no assumption of Rosetta 2 or a registered emulator), and
its unit tests cover both directions.

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

## The reg_gen C23 contract test resolves its own compiler

`apps/host/reg_gen` compiles every generated header with a real C23 front end
before it accepts it. That test used to spawn the bare name `clang-18`, which
is what the Linux CI image installs and what nothing else has. On an arm64 Mac
`std.process.Child` then reported `FileNotFound`, so `zig build test` in that
root failed before it reached a single header, and the root could not be part
of the macOS story at all.

The test now resolves a front end in a fixed order and proves it accepts C23
before using it:

1. `RA8_C23_CC`, if set. An explicit pin is the *only* candidate, and it may
   carry arguments (`RA8_C23_CC="xcrun clang"`). A pin that does not work
   fails; it never falls through to something else.
2. `clang-18`, `clang-19`, `clang-20`, `clang`, `cc`.
3. `zig cc`, using the absolute path of the zig running the build, handed to
   the test module as a build option.

Every candidate is probed with a translation unit that uses the C23
`static_assert` keyword without including `<assert.h>`, so a C17 front end is
rejected rather than silently accepted. A host with no C23 compiler at all is
still a hard test failure that prints every candidate it tried; it is not a
skip, and the compile flags are unchanged (`-std=c23 -Wall -Wextra -Werror
-fsyntax-only`).

Nothing about Linux CI changes: `clang-18` is first, so wherever it exists it
is still the compiler of record.

## Checking what the link produced, not just that it succeeded

`zig build` exiting zero says the link succeeded. It says nothing about what
came out of it, and the #899 rule is entirely a claim about what comes out: a
native arm64 Mach-O, stamped with the deployment target the build was
configured for, linked against the system `libSystem`. A build that quietly
took Zig's default macOS floor instead of the host's version exits zero too.

So `apps/host/image_pyramid` carries a step that reads the emitted image back:

    cd apps/host/image_pyramid && zig build verify-host-artifact

It reads the Mach-O header and load commands and checks four things against
what the build was configured for:

- the image is a single-architecture 64-bit Mach-O, not a universal archive;
- its cpu type matches the target architecture;
- it carries a macOS platform stamp;
- its minimum OS version equals the target's configured minimum;
- it links `/usr/lib/libSystem.B.dylib`, listing what it does link when not.

The expectations come from the resolved target, so the step checks the binary
against what this very build asked for rather than against a hardcoded answer.
Off macOS it says there is no Mach-O to read and passes, which is what lets a
Linux checkout run it unchanged.

This is also the one part of the story a Linux host can prove end to end,
because the reader parses bytes rather than asking the operating system:

    zig build verify-host-artifact -Dtarget=aarch64-macos      # 13.0.0
    zig build verify-host-artifact -Dtarget=aarch64-macos.15.0 # 15.0.0

The reader itself is unit tested against synthesised images (`tools/zig_build/
macho.zig` and its tests), including a universal archive, a 32-bit image, an
ELF, a truncated load-command region and a dylib name pointing outside its own
command.

## What runs on a clock

`.github/workflows/macos-host.yml` runs the `macos-host-build` gate nightly on
a GitHub-hosted `macos-14` (arm64) runner, provisioning the pinned Zig itself,
and can be started by hand with `workflow_dispatch`. The gate body lives in
`scripts/ci/gates/manual.sh` and is registered in `scripts/ci.sh`, so the
workflow schedules it rather than restating it.

That nightly is the only observation of the real SDK stub anywhere in this
repository: every other job runs on Linux, where an explicit `aarch64-macos`
target makes the query non-native and Zig links its own bundled stub. Until it
has run once, everything on this page about a real Mac is a prediction.
