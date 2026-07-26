# examples/ek_ra8d2/hil_needs_revalidation/

Apps here were previously in [`../hw_validated/hil/`](../hw_validated/hil/)
but did **not** pass the most recent full hardware-in-the-loop run on the
bench (113 of 157 apps passed; these are the 44 that did not). They were moved
out so that `hw_validated/hil/` reflects reality -- only currently-green apps.

**None of these are known to be broken code.** Each is blocked by a bench-
configuration change, a card that needs reseating, external hardware that is
not attached, or a state the automated harness cannot observe. The table below
records the *real* reason for each and the path back to `hw_validated/hil/`.

## SIM == HIL note (coverage this move gives up)

Every app here was passing the `sil-integration` gate (board_sim, headless)
at the time of the move: they PASS in the simulator but FAIL on the current
bench. Under the owner's SIM == HIL rule a SIM-pass / HIL-fail is a
divergence worth keeping visible. Because `sil_all.sh` and the
`hil-sil-parity` gate discover apps only under `hw_validated/hil/`, moving
these 44 apps out **drops them from the enforcing SIL run set** -- board_sim no
longer gates them per-`hil.conf`. Their `hil.conf` files travel with them, so
re-validation (and SIL re-coverage) is a `git mv` back once the bench reason
is cleared. The io-fabric demos are still booted by the `board-sim-io-fabric`
and `board-sim-matrix` gates (those resolve apps by name, tier-agnostic), so
their board_sim boot is not lost -- only the per-`hil.conf` SIL assertion is.

## Summary by reason bucket

| Reason bucket | Count | What it means |
|---|---|---|
| bench-config (C6 on OSPI) | 8 | Passes in stock config; the ESP32-C6 is now wired onto the PMOD1 pins that ARE the Octo-SPI flash bus, so OSPI/XSPI/flash/FS apps cannot reach their storage. |
| SD-not-seated | 7 | Failed with sd=0 on the last run -- the microSD card was not detected. Not a code defect. |
| needs-external-hardware | 1 | Requires an external peer/instrument that is not on the bench. |
| known-hard / under-triage | 28 | Sleep/standby states the harness cannot probe, previously-validated crypto/security apps that regressed to a bench-state, or paths the current automated harness does not assert. |

## Per-app detail

| App | Reason bucket | Real reason | Re-validation path |
|-----|---------------|-------------|--------------------|
| `mem_subsystem` | bench-config (C6 on OSPI) | Memory-hierarchy demo (SD/OSPI-backed layers); passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_cache_store_demo` | bench-config (C6 on OSPI) | LevelX NOR cache-store on OSPI flash; passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_io_cache_demo` | bench-config (C6 on OSPI) | ra8_io LRU sector-cache demo; passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_io_compress_demo` | bench-config (C6 on OSPI) | ra8_io DEFLATE stream demo; passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_io_demo` | bench-config (C6 on OSPI) | ra8_io fabric round-trip; passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_io_fsfmt_demo` | bench-config (C6 on OSPI) | ra8_io FAT format/mount demo; passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_io_sdram_demo` | bench-config (C6 on OSPI) | ra8_io external-SDRAM backend; passes in stock config, blocked by the C6-on-PMOD1(OSPI) bench setup. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `ra8_io_xspi_demo` | bench-config (C6 on OSPI) | ra8_io OSPI-NOR (XSPI) backend; the C6 sits on the very Octo-SPI flash bus this app drives. | Re-validate on the carrier PCB (#318) where the C6 has dedicated pins, or temporarily unwire the C6 from PMOD1 and re-run the HIL suite. |
| `epub_open` | SD-not-seated | Reads an EPUB from SD; last run reported sd=0 -- SD card not detected, needs a reseat. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `epub_parse` | SD-not-seated | Parses an EPUB from SD; last run reported sd=0 -- SD card not detected, needs a reseat. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `epub_toc` | SD-not-seated | Builds an EPUB table-of-contents from SD; last run reported sd=0 -- SD card not detected, needs a reseat. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `ereader_shelf` | SD-not-seated | Full e-reader shelf (baked MRAM + SD books); last run reported sd=0 -- SD card not detected, needs a reseat. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `pagecache` | SD-not-seated | Paged book accessor over SD; last run reported sd=0 -- SD card not detected, needs a reseat. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `ra8_io_sd_demo` | SD-not-seated | SD-over-SPI round-trip failed with sd=0; SD card not detected -- needs a reseat, not a code defect. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `reflow_content` | SD-not-seated | Reflows book content from SD; last run reported sd=0 -- SD card not detected, needs a reseat. | Reseat / re-insert the microSD card, confirm sd=1, then re-run the HIL suite. |
| `threadx_netx_tcp_echo` | needs-external-hardware | NetX Duo TCP echo needs an Ethernet echo peer on the wire (#292); no peer attached on the bench. | Attach the required external peer/instrument, then re-run. |
| `bkup_survival_demo` | known-hard / under-triage | Backup-domain survival across reset/standby; the survive-across-reset outcome is not captured by the harness. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `cpu1_pingpong_ipc` | known-hard / under-triage | M85<->M33 IPC ping-pong; dual-core handshake not observed on the last run -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `gpt_irq_demo` | known-hard / under-triage | GPT interrupt demo; IRQ-driven output not observed on the last run -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_deep_standby_1_demo` | known-hard / under-triage | Enters Deep Software Standby (variant 1); not probeable by the harness while asleep. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_deep_standby_2_demo` | known-hard / under-triage | Enters Deep Software Standby (variant 2); not probeable by the harness while asleep. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_deep_standby_3_demo` | known-hard / under-triage | Enters Deep Software Standby (variant 3); not probeable by the harness while asleep. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_idle_demo` | known-hard / under-triage | Puts the core in Sleep; the harness cannot probe UART/RTT while the core is halted. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_periodic_idle` | known-hard / under-triage | Periodic Sleep/wake cycle; the harness cannot probe while the core is asleep between wakes. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_software_standby_demo` | known-hard / under-triage | Enters Software Standby; not probeable by the current automated HIL harness while asleep. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_ulpt_standby` | known-hard / under-triage | ULPT wake from Software Standby; not probeable by the harness while asleep (needs a wake-and-report probe). | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lpm_wake_matrix_demo` | known-hard / under-triage | Sweeps a matrix of LPM sleep/wake sources; not probeable by the harness while asleep. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `lvd_monitor_demo` | known-hard / under-triage | Low-voltage-detect monitor; LVD event path is silicon-sensitive -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `mem_ecc_fault_demo` | known-hard / under-triage | Injects an SRAM ECC fault; fault-injection outcome not captured by the harness -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `mpu_partition_simple` | known-hard / under-triage | MPU partition fault demo (LED2 on expected MemFault); LED/fault outcome not asserted by the harness -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `pdg_delay_demo` | known-hard / under-triage | PDG delay-generator edge shift; edge measurement needs a scope/logic analyser -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `poeg_safe_shutoff` | known-hard / under-triage | POEG safe GPT output shut-off; safe-shutoff path not asserted by the harness -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `power_profiler` | known-hard / under-triage | Power-profiling demo; measurement needs a current probe -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `psa_crypto_hil` | known-hard / under-triage | Previously validated (PSA crypto on the M85); now failing -- regressed or bench-state, needs triage (not confirmed broken). | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `reset_cause_demo` | known-hard / under-triage | Reset-cause reporting; requires driving specific reset sources -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `rng_demo` | known-hard / under-triage | TRNG output demo; TRNG banner not observed on the last run -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `rot_verify_hil` | known-hard / under-triage | Previously validated (RoT-enforced boot on silicon); now failing -- regressed or bench-state, needs triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `rsip_sha256_kat` | known-hard / under-triage | SHA-256 KAT, previously validated (software SHA path); now failing -- regressed or bench-state, needs triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `rtc_alarm` | known-hard / under-triage | RTC alarm; alarm-fired banner not observed (intermittent sub-clock) -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `rtc_periodic_demo` | known-hard / under-triage | RTC periodic interrupt; periodic tick not observed (intermittent sub-clock) -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `touch_cal` | known-hard / under-triage | Capacitive-touch calibration; touch-controller interaction not asserted by the harness -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `touch_demo` | known-hard / under-triage | Capacitive-touch demo; touch input not asserted by the harness -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `tz_nsc_cgc_usb` | known-hard / under-triage | TrustZone NSC veneer (CGC/USB) demo; TZ partition/veneer path under triage (see #60). | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |
| `usb_selftest_wlun` | known-hard / under-triage | USB writable-RAM-LUN self-test; writable-LUN pass banner not observed on the last run -- under triage. | Triage against the last green SIL/HIL run; repair the harness probe or fix the regression, then promote back to hw_validated/hil. |

## Building

These still build like any other app -- the top-level CMake auto-discovers
them at their new depth (`ek_ra8d2/hil_needs_revalidation/<app>/`), and
`make <appname>` from the repo root still works. Moving a tier does not change
an app's build-target name.
