# reset_cause_demo

HIL gate for `libs/ra8_hal/ra8_reset` (#52). The app boots, triggers a
software reset via `ra8_reset_software_reset()`, and on the post-reset
boot proves that `ra8_reset_get_cause()` decodes `RSTSR1.SWRF` correctly
by entering a 20 Hz loop that advances `g_reset_cause_loop` only when
the observed cause is `k_ra8_reset_cause_software`.

The HIL probe (`hil.conf`) uses `jlink_memprobe` to sample
`g_reset_cause_loop` twice over 5 s and assert advance >= 3 -- which is
only possible if the SW-reset round-trip completed.

For triage, `g_reset_cause_initial` is also visible to J-Link and holds
the `ra8_reset_cause_t` value observed on this boot.

## Build + flash

    make reset_cause_demo            # build (top-level)
    ./scripts/hil/flash.sh examples/ek_ra8d2/hw_validated/hil/reset_cause_demo/build/reset_cause_demo.hex

CI runs the same probe via `.github/workflows/hil.yml`.
