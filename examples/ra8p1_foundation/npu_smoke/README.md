# npu_smoke

Exercises the whole `ra8_npu` command/queue driver surface on the RA8P1: come
out of module-stop and soft-reset, read the NPU identity probe, program the
command-queue base and size plus a tensor-region base, kick the job, and read
the status back into a debugger-visible global.

It proves the Arm Ethos-U55 driver compiles and links for the RA8P1 and
documents the call sequence the hardware expects. The RA8P1 toolchain is what
makes `ra8_device.h` declare the NPU present and compile the otherwise
device-gated driver; a `#error` guard in `main.c` fails the build loudly under
the RA8D2 toolchain.

The command stream is a zeroed placeholder rather than a Vela-compiled
program, so a real inference would not complete on silicon even if there were a
board to try it on. The driver's behaviour is actually pinned by the host unit
test, which asserts the exact register submission sequence against mock MMIO.
Sibling apps in this tier take the next steps: loading a real container, and
the runtime above the driver.
