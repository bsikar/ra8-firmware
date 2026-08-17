# examples/ek_ra8d2/hw_validated/hil/

Apps that are hardware-confirmed **and** machine-checkable: a job on the
self-hosted Pi runner flashes the binary, exercises it unattended, and asserts
pass or fail with no human in the loop.

Each app's own `hil.conf`, beside its `main.c`, is the source of truth for how
it is verified -- the mode, the string or counter to look for, and the timeout.
`scripts/hil/all.sh` reads them; there is no second list here to drift from
them. `make <appname>` builds any app, and the top-level CMake discovers this
directory, so adding one needs no edit anywhere else.

## How an app gets asserted

- **UART scrape.** The app prints on SCI8, which the J-Link OB bridges to a USB
  CDC port; the runner opens the console by device identity (never by ttyACM
  number) and waits for the expected line.
- **J-Link memprobe.** For apps with nothing useful to print, the runner reads a
  counter or sentinel out of SRAM over SWD -- a heartbeat that only advances
  once the app reaches the state under test.
- **RTT scrape** and a **wire-side TCP probe** cover the rest.
- **USB self-loop.** The board's two USB jacks are cabled **to each other**
  (J7 High-Speed, J11 Full-Speed) and one image runs both roles, so the host
  stack and the device stack validate each other on-chip with no PC involved.
  This is the preferred transport for anything USB: it exercises more of the
  path than an external host would, unattended. Device-mode apps that genuinely
  need a separate host live in [`../manual/`](../manual/).

## What is not here

Apps whose acceptance is a human observation -- a picture on the LCD, a button
press, a file copied from a PC -- are in [`../manual/`](../manual/). Apps that
were in this tier but did not pass the most recent bench run are in
[`../../hil_needs_revalidation/`](../../hil_needs_revalidation/README.md), which
records the blocker and the way back for each.
