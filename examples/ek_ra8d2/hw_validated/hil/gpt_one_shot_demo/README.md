# gpt_one_shot_demo

Arms GPT channel 0 in saw-wave one-shot mode (HUM Ch 25.2.1, GTCR.MD = 001b):
the counter runs up to the period once, raises an overflow IRQ, then stops on
its own. The loop arms, waits for the IRQ and re-arms, so a bench probe can
watch the completion counter advance.

That counter is the whole test. If the GPT clock gate is closed, GTCR.MD is
programmed wrong, or the IRQ is masked, the overflow callback never fires and
the counter simply stops -- none of which a "did it fault?" liveness check
would notice.
