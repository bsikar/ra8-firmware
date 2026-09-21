# gpt_dma_demo

Streams a table of GPT period values into GTPR over a DMAC channel, so the CPU
never touches the period register per tick. The DMA-completion callback bumps a
counter a bench probe watches, and the loop re-arms after each transfer.

The period table lives in `.rodata` and must outlive the transfer: the DMAC
walks a physical address of its own, so a stack-local table would be a
use-after-scope no compiler is going to warn about.

What the counter catches that a fault check would not: DMA init failure, a
refused DMAC channel allocation, a rejected `ra8_gpt_write_dma`, and a
completion callback that never fires because the transfer wedged.
