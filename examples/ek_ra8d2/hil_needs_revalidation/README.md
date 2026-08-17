# examples/ek_ra8d2/hil_needs_revalidation/

Apps that were in [`../hw_validated/hil/`](../hw_validated/hil/) and did not
pass the most recent full bench run. They sit here so that `hw_validated/hil/`
keeps meaning what it says: currently green.

**A reason below is a hypothesis, not a verdict.** `threadx_netx_tcp_echo` was
labelled "needs an Ethernet peer"; it got one (#292) and failed anyway, on a
firmware regression that had been sitting on `dev` for a month behind that wrong
label (#499). Expect clearing a blocker to sometimes reveal a defect rather than
a pass.

They still build like any other app -- `make <appname>` from the repo root, the
target name does not change with the tier. Re-validating one is a `git mv` back
into `hw_validated/hil/`; its `hil.conf` travels with it.

## What is blocking them

| Blocker | Way out | Apps |
|---|---|---|
| **C6 on the OSPI pins.** These pass in the stock bench configuration, but the ESP32-C6 is wired onto the PMOD1 pins that *are* the Octo-SPI flash bus, so storage-backed apps cannot reach their storage. | The carrier PCB (#318) gives the C6 dedicated pins; until then, unwire it from PMOD1 and re-run. | `mem_subsystem`, `ra8_cache_store_demo`, `ra8_io_cache_demo`, `ra8_io_compress_demo`, `ra8_io_demo`, `ra8_io_fsfmt_demo`, `ra8_io_sdram_demo`, `ra8_io_xspi_demo` |
| **SD card not seated.** All reported `sd=0` on the last run. Not a code defect. | Reseat the microSD, confirm `sd=1`, re-run. | `epub_open`, `epub_parse`, `epub_toc`, `ereader_shelf`, `pagecache`, `ra8_io_sd_demo`, `reflow_content` |
| **The harness cannot see the outcome, or the app regressed.** Mostly sleep and standby states the probe cannot reach while the core is halted, edges that need a scope or a current probe, and a handful of previously-green crypto and security apps that are now failing for reasons not yet triaged. | Triage against the last green run, then either repair the probe or fix the regression. | `bkup_survival_demo`, `cpu1_pingpong_ipc`, `gpt_irq_demo`, `lpm_deep_standby_1_demo`, `lpm_deep_standby_2_demo`, `lpm_deep_standby_3_demo`, `lpm_idle_demo`, `lpm_periodic_idle`, `lpm_software_standby_demo`, `lpm_ulpt_standby`, `lpm_wake_matrix_demo`, `lvd_monitor_demo`, `mem_ecc_fault_demo`, `mpu_partition_simple`, `pdg_delay_demo`, `poeg_safe_shutoff`, `power_profiler`, `psa_crypto_hil`, `reset_cause_demo`, `rng_demo`, `rot_verify_hil`, `rsip_sha256_kat`, `rtc_alarm`, `rtc_periodic_demo`, `touch_cal`, `touch_demo`, `tz_nsc_cgc_usb`, `usb_selftest_wlun` |

## The coverage this tier gives up

Every app here was passing the `eil-integration` gate in `tools/ra8_emulator`
when it was moved: they pass in the emulator and fail on the bench, which under
the EIL == HIL rule is a divergence worth keeping visible.

But `eil_all.sh` and the `hil-eil-parity` gate discover apps only under
`hw_validated/hil/`, so moving an app here **drops it from the enforcing EIL
run set** -- no more per-`hil.conf` assertion in the emulator. The io-fabric
demos are still booted by the `emulator-io-fabric` and `emulator-matrix` gates,
which resolve apps by name and are tier-agnostic, so their boot coverage
survives; the assertion does not.
