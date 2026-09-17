# tools/vela

[Arm Ethos-U Vela](https://gitlab.arm.com/artificial-intelligence/ethos-u/ethos-u-vela)
is the offline compiler that turns a quantized `.tflite` model into an
Ethos-U-optimized one: the operators the NPU supports are fused into a single
`ethos-u` custom operator wrapping a Vela-generated **command stream**, and the
rest fall back to CPU kernels in the on-device runtime.

## A build-time HOST tool, not firmware

Nothing from Vela is linked into firmware. It runs on the developer or CI host;
the on-device runtime is the vendored TFLite-micro, which executes the command
stream through the first-party `ra8_npu` driver. It is a version-pinned build
dependency rather than vendored source SOUP because it is a Python tool run at
build time -- but it still carries a qualification record
([`docs/SOUP/vela.md`](../../docs/SOUP/vela.md)) and a row in
[`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md), because a build tool
that shapes the bits in the image is part of the supply chain.

The pin exists so the emitted command stream is reproducible and so the operator
protocol Vela targets stays matched to the vendored TFLite-micro `ethosu`
operator. Moving either one is a re-qualification against the other, not a
version bump.

## The `.npub` container and its loader (#227)

A bare-metal target does not parse a TFLite flatbuffer at run time. An offline
build step distils a model into a lean, linkable container -- an Ethos-U55
command stream plus its tensor region layout, defined by `ra8_npu_blob.h` --
baked as a C byte array the firmware links. The on-target loader validates that
container, resolves every region base (baked regions into the blob, runtime
regions into a caller-supplied arena) and fills an `ra8_npu_job_t` for
`ra8_npu_submit()`.

The committed golden header is regenerated in memory and diffed against the
tree, which needs no Vela toolchain at all, so CI guards the container format
without ever provisioning Vela. That golden's command stream is the documented
stand-in convention (`ra8_npu_fake_cmd.h`) -- the only Ethos-U55 stream this
repo can produce deterministically without a Vela install. Distilling a real
Vela output into a `.npub` is a follow-up on the RA8P1 NPU epic: it needs both a
validated Vela run and an RA8P1 board to confirm on silicon.
