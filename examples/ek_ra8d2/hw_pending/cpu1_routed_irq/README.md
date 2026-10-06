# cpu1_routed_irq

CPU0 releases CPU1 and waits for it to arm, then assigns GPT0's overflow event (0xC1) to CPU1 in INTSELR and starts GPT0. CPU1 links that event to its own ICU line 0 (IELSR0) and records the interrupt in shared SRAM from the IRQ handler. CPU0 prints PASS only after it sees that handler completion. Both halves are Zig (RA8FW-809).
