# elc_event_demo

Brings the Event Link Controller up, routes external IRQ0 into ELC slot 0, and
then fires that slot in software once a second so the demo never depends on
anyone pressing a button. The ELC is how one peripheral triggers another
peripheral's input with no CPU involvement at all (HUM Ch 19 p 817-836); the
software trigger is the three-step ELSEGR unlock-arm-set sequence (HUM
Ch 19.2.2). LED1 toggles per cycle and LED2 latches on if any ELC call
hard-fails. Bare EK-RA8D2, no external pins needed.
