# lcd_draw_x

Draws a yellow X corner to corner on a dark-blue RGB565 framebuffer in SRAM and
composites it through GLCDC graphics layer 1 over the BG plane, on the same
7-inch parallel TFT as `lcd_color_cycle`. Where that app exercises the BG plane
alone, this one is the canonical reference for layer-1 work: a statically
allocated, AXI-aligned framebuffer, pixel-level CPU writes into it, and flipping
layer 1 from hidden to visible.

The framebuffer covers the top-left 512 x 512 of the panel and the BG plane
paints the rest black. That is not an arbitrary choice: a full-panel 1024 x 600
RGB565 framebuffer does not fit in on-chip SRAM, so a full-resolution demo has
to place its framebuffer in external SDRAM.

Verification is visual: a yellow X on a blue square in the top-left corner and
black everywhere else, with the blue LED as a heartbeat and the red LED latching
on an init failure.

The controller is HUM Ch 63 "Graphics LCD Controller (GLCDC)"; the panel pin
assignments are EK-RA8D2 v1 UM Table 33 p 42.
