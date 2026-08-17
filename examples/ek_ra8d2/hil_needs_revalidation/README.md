# examples/ek_ra8d2/hil_needs_revalidation/

Apps that were in [`../hw_validated/hil/`](../hw_validated/hil/) and did not
pass the most recent full bench run. They sit here so that `hw_validated/hil/`
keeps meaning what it says: currently green. Each app's own README carries its
suspected blocker.

**A blocker written down is a hypothesis, not a verdict.** One app here was
labelled "needs an Ethernet peer"; it got one, and failed anyway -- on a
firmware regression that had been sitting on `dev` for a month behind that
wrong label (#499). Expect clearing a blocker to sometimes reveal a defect
rather than a pass.

They build like any other app and the target name does not change with the
tier. Re-validating one is a `git mv` back into `hw_validated/hil/`; its
`hil.conf` travels with it.

## The recurring blockers

- **The C6 sits on the Octo-SPI pins.** The companion radio is wired onto the
  PMOD1 pins that *are* the Octo-SPI flash bus, so in that bench configuration
  storage-backed apps cannot reach their storage. A carrier PCB (#318) gives
  the C6 dedicated pins; until then, unwire it and re-run.
- **Consumables and cabling.** An unseated microSD fails a whole family of
  reader apps at once and reads as a code defect until someone looks at the
  board.
- **The probe cannot see the outcome.** Sleep and standby states the debugger
  cannot reach while the core is halted, and edges that need a scope or a
  current probe rather than a console line.
- **A real regression.** Triage against the last green run before assuming it
  is one of the above.

## The coverage this tier gives up

Every app here was passing in `tools/ra8_emulator` when it moved: they pass in
the emulator and fail on the bench, which under the EIL == HIL rule is a
divergence worth keeping visible.

But the EIL run set and the parity gate discover apps only under
`hw_validated/hil/`, so moving an app here **drops it from the enforcing EIL
run set** -- no more per-`hil.conf` assertion in the emulator. The io-fabric
demos are still booted by the emulator gates that resolve apps by name and are
tier-agnostic, so their boot coverage survives; the assertion does not.
