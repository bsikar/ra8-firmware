# gpt_one_shot_demo

Runs board timer 0 as a one-shot through the board's timer port
(`ra8_board_timer()`, `fw_if_timer.h`). On EK-RA8D2 that is GPT channel 0 in
saw-wave one-shot mode (HUM Ch 25.2.1, GTCR.MD = 001b): the counter runs up to
the period once, reports the wrap, then stops on its own. The loop starts the
timer, polls `fw_timer_take_wrap` and starts it again, so a bench probe can
watch the completion counter advance.

That counter is the whole test. If the timer's clock gate is closed or its
mode is programmed wrong, the wrap is never reported and the counter simply
stops -- none of which a "did it fault?" liveness check would notice.

The app names no GPT register or driver call. Which chip channel and clock
divider back board timer 0 belongs to
`libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_gpt_profile.c`.

## Bench status

The `hw_validated/hil/` tier records a bench run of the app as it was before
it moved onto the timer port (#693). This version has not been run on the
bench yet. Two things differ on the wire: the counter is now clocked at
PCLKD/1 rather than PCLKD/4, so each one-shot is a quarter as long; and the
earlier version restarted through `ra8_gpt_start_free_run`, which rewrites
GTCR to saw-wave PWM, so after the first pass it was counting continuous
wraps rather than true one-shots. This one stays in one-shot mode every pass.
