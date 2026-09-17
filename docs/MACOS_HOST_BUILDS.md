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
probes the macOS SDK with `xcrun --sdk macosx --show-sdk-path`, parses the
`targets` list out of `usr/lib/libSystem.tbd`, and decides between two
outcomes:

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

## Which SDK gets probed

The probe names the SDK it wants: `xcrun --sdk macosx --show-sdk-path`. That is
the same call Zig makes for a macOS target
(`std.zig.system.darwin.getSdk` maps `.macos` to `macosx`), and matching it is
the whole point. The bare `xcrun --show-sdk-path` asks a different question: it
reports the *active* SDK, which `SDKROOT` in the environment redirects. Any
shell spawned from an Xcode build phase carries one, and so does a developer
who exported `SDKROOT=iphoneos` for cross work.

With the bare form, such a shell had the graph read
`iPhoneOS.sdk/usr/lib/libSystem.tbd`. Its targets are `arm64-ios` and friends,
so `arm64-macos` came back absent and the finding printed was *the SDK stub
lists its targets and `arm64-macos` is not among them (#899)*: a report about a
file no macOS link would ever have opened, in the exact words of the bug this
whole document is about. The pin that followed was harmless; the diagnosis was
not.

Two things now keep that apart. The probe asks for the macOS SDK by name, so it
reads what the compiler reads. And a stub that declares no macOS target at all
is reported as another platform's stub rather than as a macOS stub that omits
us, so the two remain distinguishable even if some other route hands the probe
a foreign SDK. `maccatalyst` does not count as a macOS target here, and neither
does a bare `macos` word outside a triple.

`zig build explain-host-target` prints the SDK it asked for on its own line:

    sdk query: macosx
    sdk:       /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

`scripts/ci/lib/macos_sdk.sh` runs the same named call, so the gate's
precondition and the graph resolve the same SDK. Its selftest asserts both
sides, including that `tools/zig_build` still pins the same SDK name, so the
two cannot drift apart quietly.

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

Five different findings all end in a pinned target, and they need different
fixes, so the graph names which one it saw rather than reporting them as one
state:

* the stub lists its targets and `arm64-macos` is absent -- this is #899 itself;
* the stub lists targets and none of them is a macOS target at all, so what was
  read is another Apple platform's stub (see below);
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
    xcrun --sdk macosx --show-sdk-path
    grep -n 'targets:' "$(xcrun --sdk macosx --show-sdk-path)/usr/lib/libSystem.tbd" | head

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

## The stub the fix links to is checked too

The whole workaround is "pin an explicit `aarch64-macos` target so Zig links
its own `libSystem.tbd` instead of the SDK's". That is a fix for exactly as
long as Zig's own stub declares `arm64-macos`. Nothing here ever read it, so
that was an assumption rather than a check, and the day a toolchain bump ships
a stub without our slice the pinned build fails with the same
`undefined symbol: _abort` wall the issue is about, caused this time by the
fix. The build graph would still have reported that it had worked around the
problem.

`zig build verify-bundled-stub` in `tools/zig_build` reads
`<zig lib dir>/libc/darwin/libSystem.tbd` and refuses a toolchain whose stub
cannot link the pinned target:

```
$ cd tools/zig_build && zig build verify-bundled-stub
verify-bundled-stub: /usr/local/zig/lib/libc/darwin/libSystem.tbd declares arm64-macos,
so the pinned host target links against it
```

Two things about where it runs. It runs on **every** host, not only on a Mac,
because the thing it guards against arrives with a Zig upgrade rather than with
a machine, and a Linux checkout cross-building `-Dtarget=aarch64-macos` links
that very same file. Whoever bumps the pin sees it fail on their own box. And
the classification goes through `macos_host.classifyTbd`, the same reader the
SDK stub goes through, so the two stubs can never be judged by different rules.

The states are kept apart the way the SDK ones are, because the fixes differ:
the stub declares the target, it lists targets and ours is absent, it names no
macOS target at all (some other platform's stub), it carries no target list in
any spelling we read, the file could not be read, or the Zig lib directory is
unknown to the build runner. `zig build explain-host-target` prints the finding
on its own `bundled finding:` line beside the SDK one, so a single report says
what both stubs can do.

The macOS gate runs it immediately after the host-target decision, before any
root is built. A red run then names the cause on one line instead of burying it
under an undefined-symbol wall from three build roots at once.

## Checking what the link produced, not just that it succeeded

`zig build` exiting zero says the link succeeded. It says nothing about what
came out of it, and the #899 rule is entirely a claim about what comes out: a
native arm64 Mach-O, stamped with the deployment target the build was
configured for, linked against the system `libSystem`. A build that quietly
took Zig's default macOS floor instead of the host's version exits zero too.

So `apps/host/image_pyramid` carries a step that reads the emitted image back:

    cd apps/host/image_pyramid && zig build verify-host-artifact

It reads the Mach-O header and load commands and checks these against what the
build was configured for:

- the image is a single-architecture 64-bit Mach-O, not a universal archive;
- its cpu type matches the target architecture;
- it carries a macOS platform stamp;
- its minimum OS version equals the target's configured minimum;
- it links `/usr/lib/libSystem.B.dylib`, listing what it does link when not;
- for an arm64 target it carries a usable code signature covering the whole
  image (see below).

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

### An arm64 image has to be signed or it cannot run

Apple silicon will not execute an unsigned Mach-O. The kernel kills the process
at exec, the shell reports `Killed: 9`, and nothing says why. That is the #899
failure shape exactly: the link succeeds, the artifact looks right, and the host
test step dies with no diagnostic to read. Zig's own Mach-O linker writes an
ad-hoc signature, so the check is that what came out still carries one.

For an arm64 macOS target the step now requires:

- an `LC_CODE_SIGNATURE` whose blob is an embedded signature super-blob holding
  a code directory;
- a `codeLimit` equal to where the signature blob starts, so the signature
  covers every byte in front of it. A mismatch means the image was stripped,
  patched or appended to after the link, which macOS rejects at exec rather
  than reporting as an edit;
- the ad-hoc or linker-signed flag, since nothing in this build signs with an
  identity.

x86_64 macOS still runs unsigned binaries, so for an x86_64 target the absence
of a signature is reported in the summary line and does not fail the step. What
a Linux checkout can prove:

    zig build verify-host-artifact -Dtarget=aarch64-macos
    # ... linking /usr/lib/libSystem.B.dylib, linker ad-hoc signed as
    # "image_pyramid" over all 1683648 bytes

    zig build verify-host-artifact -Dtarget=x86_64-macos
    # ... no readable code signature (MissingCodeSignature), which x86_64
    # does not require

The signature structures are big-endian and their offsets are relative to the
blob rather than to the file, so they are parsed separately from the load
commands. Every case is unit tested against blobs built byte by byte: a linker
ad-hoc signature, an unsigned image, a signature stopping short of its own blob
and one claiming more bytes than precede it, an identity signature, a
non-embedded blob, a super-blob with no code directory slot, a wrong directory
magic, a region pointing past the end of the file and a stripped (zero-length)
region.

## The Rust archives are read before they are linked

Three host roots link a static archive that a separate `cargo` invocation
produced:

| Root | Archive |
| --- | --- |
| `tests/rust_abi_fixture/zig` | `libra8_rust_abi_fixture.a` |
| `tests/abi_chain_fixture` | `libra8_rust_abi_fixture.a` |
| `apps/host/firmware_pipeline/zig` | `libfirmware_pipeline_rust.a` |

`cargo build` with no `--target` builds for the machine it runs on, so the
archive and the Zig target agree only while nobody pins a target and nobody
reuses a target directory that a different host filled in. `-Drust-lib-dir=`
hands the archive over without running `cargo` at all, and those directories
live inside the checkout, which is shared between a Linux devcontainer and the
Mac that opens it.

When they disagree, the linker is the one that complains, and it complains
about symbols:

    error: undefined symbol: _ra8_rust_abi_fixture_create
        note: referenced by .../test.o:_adapter.create

which reads like a missing export rather than an archive for the wrong
platform. That is the one reason these three roots are still outside the
`macos-host-build` gate: a red run could not be told apart from a genuine ABI
break.

Each of them now depends on a step that reads the archive first
(`ra8_build.addRequireArchiveForTargetStep`). It walks the `ar` members, skips
the symbol and long-name tables, and reads the first real object's format and
architecture, so the mismatch is named before any link is attempted:

    error: .../libra8_rust_abi_fixture.a holds ELF aarch64 objects, but test is
    linked for aarch64-macos, which needs Mach-O aarch64 objects. The archive
    was built for a different host than this build targets; build it for
    aarch64-macos, or point -Drust-lib-dir= at one that is (#899).

A missing archive is its own message rather than a `FileNotFound` out of the
linker. An archive that does match prints what it found and gets out of the
way:

    require-archive: .../libra8_rust_abi_fixture.a holds Mach-O aarch64
    objects, which test can link for aarch64-macos

The expectation comes off the resolved target at configure time, so the check
is against what that build actually asked for. The step sits after the `cargo`
step when the graph runs one, so it reads the archive `cargo` just wrote. The
reader itself (`tools/zig_build/ar.zig`) is unit tested against archives
synthesised byte by byte: System V and Apple symbol tables, BSD `#1/<len>` long
names, a truncated member, a malformed size field, a text member, a universal
Mach-O and a 32-bit ELF.

## Which build roots the gate measures

`macos-host-build` does not build every Zig root in the tree, and the list is
declared in `scripts/ci/lib/macos_host_roots.sh` rather than inlined in the
gate. Each row carries the root, whether it is covered, and the reason either
way; the gate prints the whole thing under `=== gate coverage ===` so a green
run states what it did *not* measure instead of implying the tree builds on
macOS.

Covered today: `tools/zig_build` (it defines the selection, so it fails first),
`apps/host/image_pyramid` (the root whose Mach-O the gate reads back), and
`tests/zig_abi_fixture`. All three need nothing beyond `zig`.

Deferred today, with the reason in the manifest: `apps/host/reg_gen`, because
its generated-header contract resolves a C23 front end at run time and the
`zig cc` fallback leg has never been exercised, so cover it once the nightly
shows which front end the runner resolves; and `apps/host/firmware_pipeline/zig`,
`tests/abi_chain_fixture`, `tests/rust_abi_fixture/zig`, because each links a
cargo-built archive and the macOS workflow provisions no Rust toolchain.

`bash scripts/ci/lib/macos_host_roots.sh --selftest` runs inside `ci-parity`
and fails when a `build.zig` exists that the manifest does not mention in
either direction, and when the gate stops reading the list. That matters
because `check_zig.py`'s host-target rule keeps a new root *looking* correct
from Linux: it takes its default target from
`ra8_build.hostDefaultTargetQuery`, passes every Linux check, and would never
be built on a Mac at all. Adding a root is therefore a deliberate edit here:
cover it, or write down why not.

## The SDK precondition runs the probe

`command -v xcrun` is not a question about the SDK. macOS ships `/usr/bin/xcrun`
as a stub on every install, whether or not any developer tools sit behind it,
and it fails only when run:

    xcrun: error: invalid active developer path (/Library/Developer/CommandLineTools),
           missing xcrun at: /Library/Developer/CommandLineTools/usr/bin/xcrun

So the gate's old `require_cmd xcrun` passed on a Mac with no Command Line
Tools at all. The graph then located no SDK, reported `sdk_not_probed`, pinned
the bundled libSystem stub, and every root built and tested cleanly, so the
gate reported **green for the native SDK link path it never took**. The
forced-SDK informational leg, the one channel through which a real Mac reports
the state of Apple's stub back to this repository, recorded nothing either.

`scripts/ci/lib/macos_sdk.sh` runs the probe the graph runs and keeps the
failures apart, because each is a different morning:

| state | what it means | fix |
| --- | --- | --- |
| `ok` | an SDK is readable and carries `usr/lib/libSystem.tbd` | nothing |
| `xcrun_absent` | no `xcrun` on `PATH` or at `/usr/bin/xcrun` | `xcode-select --install` |
| `developer_dir_invalid` | `xcrun` ran, no developer directory is active | `xcode-select --install`, then `sudo xcode-select --reset` |
| `license_unaccepted` | `xcrun` refuses until the licence is accepted | `sudo xcodebuild -license accept` |
| `sdk_path_empty` | the probe succeeded and printed nothing | run `xcrun --sdk macosx --show-sdk-path` and read the error |
| `sdk_path_missing` | the named SDK is not on this disk | `xcode-select -p`, then `--reset` |
| `stub_missing` | the SDK ships no `libSystem.tbd` | reinstall the Command Line Tools |
| `not_macos` | a Linux checkout | run the gate on an arm64 Mac |

Anything but `ok` is a refusal with that remedy printed, and the gate prints the
state under `=== active macOS SDK ===` on the way past either way. To ask
directly, on your own Mac:

    bash scripts/ci/lib/macos_sdk.sh --report

The build graph is deliberately left forgiving. A developer with no Command
Line Tools can still build the host apps here: they are libc-only and the
bundled stub serves them, so `sdk_not_probed` pinning the bundled stub is the
right local behaviour. It is only the *gate* that must refuse, because a gate
that cannot ask its question must not answer it.

`bash scripts/ci/lib/macos_sdk.sh --selftest` runs inside `toolchain-parity`.
It drives every state through a stubbed `xcrun` seam (the on-disk rows use real
directories), proves the probe is executed exactly once at the resolved path
and not at all off macOS, proves no two states share a diagnosis, and reads the
gate body to check it still calls `ra8_macos_sdk_require` and no longer leans
on `xcrun` merely being present.

## The informational leg says what it found

The gate ends with a leg that is allowed to fail:

    === apps/host/image_pyramid: -Dmacos-libsystem=sdk (informational) ===

Failing is the point. Forcing the SDK stub is the thing #899 reports, so the
verdict comes from the pinned-target legs above it and this one only reports.
It is also the *only* step anywhere in this repository that still touches
Apple's own `libSystem` stub: every other step, on every other machine, builds
against the bundled stub the workaround pins.

It used to be a boolean, and that was the defect. `zig build
-Dmacos-libsystem=sdk` exits non-zero for reasons that have nothing to do with
the SDK, and the old else-branch called every one of them the expected #899
failure. The worst of them is a rename: drop or rename the option in
`tools/zig_build` and zig answers

    error: invalid option: -Dmacos-libsystem
    error:   access the help menu with 'zig build -h'

which the old leg read as an affected SDK. The nightly would have gone on
printing the finding every night while building nothing at all. A compile error
in the app, an unwritable cache, or a broken toolchain read the same way.

`scripts/ci/lib/macos_sdk_link.sh` classifies the outcome from the build output
rather than the exit status alone. Six states, and the verdict is part of the
state, not a guess made later:

    linked               informational  the SDK stub linked cleanly here
    symbols_unresolved   informational  the undefined-symbol wall, i.e. #899
    stub_unusable        informational  the stub could not be resolved at all
    option_gone          REFUSES        zig rejected -Dmacos-libsystem
    unrelated_failure    REFUSES        a failure that says nothing about the SDK
    log_unreadable       REFUSES        the output was not captured

`option_gone` is checked before the symbol wall on purpose: a rejected option
means nothing downstream ran, so any wall text in the same log is stale.
`linked` is decided by the exit status alone, so a passing test whose name
contains the wall text is not mistaken for a failure. The two informational
findings stay apart because they need different mornings: an incomplete stub is
a `arm64-macos` slice Apple did not ship, an unusable one is an SDK that is
missing or broken, and `scripts/ci/lib/macos_sdk.sh --report` is what tells
those apart on the machine.

The state table is printable without running a build:

    bash scripts/ci/lib/macos_sdk_link.sh --explain

`bash scripts/ci/lib/macos_sdk_link.sh --selftest` runs inside
`toolchain-parity`. It drives a fixture log through every state, asserts the
ordering above holds when a log carries both shapes, asserts no state is
reported as another and that each refuses or informs the right way round, reads
`tools/zig_build/build.zig` to check the option this leg drives still exists
(without that, `option_gone` could only ever be reached by a real nightly), and
reads the gate body to check it still calls `ra8_macos_sdk_link_run`.

## The compiler the runner downloads is recorded per release

The hosted `macos-14` runner ships no Zig, so `.github/workflows/macos-host.yml`
provisions its own. That takes three values, and until recently only one of them
had an owner:

| value | where it lives | what held it |
| --- | --- | --- |
| `ZIG_VERSION` | workflow `env` | `.devcontainer/Dockerfile` (`ARG ZIG_VERSION`), enforced by `scripts/checks/check_workflow_toolchain_pins.py` |
| `ZIG_SHA256_AARCH64_MACOS` | workflow `env` | nothing |
| the archive name in the download URL | the install step | nothing |

That check deliberately does not invent a Dockerfile owner for a workflow-only
pin, which is right: the digest genuinely belongs to the workflow. The
consequence was that bumping the Dockerfile moved `ZIG_VERSION` (the agreement
rule insists on it) while the digest stayed where it was. The runner then
fetched the new tarball and checked it against the old digest. Every Linux leg
stayed green, because no Linux leg reads either value, and the Mac died at
provisioning with `zig.tar.xz: FAILED`. That reads as a corrupted download or a
tampered mirror, so the natural response is to re-run the job, and the re-run
fails identically.

The archive *name* has the same shape of problem. Zig renamed its release
archives at 0.14.1: `zig-macos-aarch64-0.14.0.tar.xz` became
`zig-aarch64-macos-0.14.1.tar.xz`, target before os. The workflow spells the
newer shape out, so pinning any release at or before 0.14.0 404s at `curl`
before the digest is ever consulted.

`scripts/checks/check_zig_dist_pins.py` records, per release and target, the
archive name Zig publishes and its sha256, transcribed from
<https://ziglang.org/download/index.json> by whoever bumps the pin, in the same
commit. The table is not a source of truth; it is a transcription. Its value is
that the transcription is checkable from Linux, in `gate_toolchain_parity`,
minutes after the bump, rather than one night later on the only machine in this
suite nobody can re-run locally. Five rules:

- **recorded** -- a workflow `ZIG_SHA256_<TARGET>` pin needs a row for that
  workflow's `ZIG_VERSION` and that target.
- **digest-agrees** -- the row's digest must equal the pin's value.
- **owner-recorded** -- the Dockerfile's `ARG ZIG_VERSION` must appear in the
  table, so bumping the owner pin alone refuses before any workflow is touched.
- **tarball-name** -- the download URL, with `${ZIG_VERSION}` resolved, must
  name exactly the archive recorded for that release, and each recorded name
  must match the convention for its own release (target first from 0.14.1).
- **record-shape** -- every recorded release parses as a dotted version and
  every digest is 64 lowercase hex characters.

`--roster` prints what is recorded and what each workflow would fetch:

```console
$ python3 scripts/checks/check_zig_dist_pins.py --roster
0.14.1 aarch64-macos: zig-aarch64-macos-0.14.1.tar.xz sha256=39f3dc5e...
.github/workflows/macos-host.yml:72 fetches zig 0.14.1 as zig-aarch64-macos-0.14.1.tar.xz
```

Bumping Zig is therefore a three-line change: `ARG ZIG_VERSION` in the
Dockerfile, `ZIG_VERSION` and `ZIG_SHA256_AARCH64_MACOS` in the workflow, and a
row here. Miss any of them and Linux says so by name, in
`gate_toolchain_parity`, before the nightly runs at all.

Still open: the install step's own `shasum` failure text. A mismatch on the
runner still prints only `zig.tar.xz: FAILED`, which reads as a bad download
rather than a forgotten digest. Saying the likelier cause out loud there means
editing `.github/workflows/macos-host.yml`, which needs a token carrying the
`workflow` scope; the check above is what makes that message unlikely to be
needed.

## What runs on a clock, and what does not

`.github/workflows/macos-host.yml` asks a GitHub-hosted `macos-14` (arm64)
runner for the `macos-host-build` gate at 07:41 UTC, `41 7 * * *`,
provisioning the pinned Zig itself, and also declares `workflow_dispatch`. The
gate body lives in `scripts/ci/gates/manual.sh` and is registered in
`scripts/ci.sh`, so the workflow schedules it rather than restating it.

Neither trigger fires while that file is off the default branch. GitHub runs
`schedule` from the latest commit on the default branch only, and offers
`workflow_dispatch` only for workflows that exist there: the ref to run is
chosen at dispatch time, but a workflow absent from the default branch is
never listed, and `gh workflow run` answers "could not find any workflows
named". This repository's default branch is `main`, and the workflow reaches
`zig/dev` through the #899 stack. Merging that stack therefore starts no
nightly and produces no Run workflow button; only `zig/dev` reaching `main`
does.

Until then the only way to get a real macOS reading is to run the gate by hand
on an arm64 Mac:

    just quality::local::gate macos-host-build

That reading is the only observation of the real SDK stub anywhere in this
repository: every other job runs on Linux, where an explicit `aarch64-macos`
target makes the query non-native and Zig links its own bundled stub. Until
the gate has run once on a Mac, by hand or on a clock, everything on this page
about a real Mac is a prediction, and this page says so rather than pointing
at a nightly log that does not exist.

## This page is held to the workflow it describes

Everything the section above states about the workflow is a second copy of a
fact that lives in `.github/workflows/macos-host.yml`, and a second copy goes
stale in silence. `scripts/checks/check_macos_doc_workflow_parity.py`, run by
`gate_toolchain_parity`, reads the workflow and refuses when this page
disagrees with it:

  * **runner** -- every `macos-<n>` label named here must be the label the job
    actually takes. The Apple silicon advice on this page is only true for an
    arm64 image, and the older hosted macOS images are x86_64; naming one of
    those here is a finding, which is why this bullet describes them rather
    than spelling a label out. A target triple or an archive name
    (`zig-aarch64-macos-0.14.1.tar.xz`) is not a runner label and is ignored.
  * **clock** -- every `HH:MM UTC` time and every quoted five-field cron here
    must agree with the workflow's `schedule`.
  * **caveat** -- while the workflow declares `schedule` or
    `workflow_dispatch`, both the workflow and the clock section here must say
    plainly that neither fires off the default branch. Deleting that sentence
    is a finding, not a silent regression; it is the exact claim this page got
    wrong before.
  * **gate** -- the gate the workflow runs must be named here and registered
    in `scripts/ci.sh`, so the manual runbook above cannot outlive a rename.

The workflow is the source of truth and this page is the copy; the check never
edits either, and asserts nothing about whether the nightly has run.
`--roster` prints what it read from each side, and `--selftest`, run beside the
check in the same gate, sabotages each rule in turn and asserts it fires alone.
