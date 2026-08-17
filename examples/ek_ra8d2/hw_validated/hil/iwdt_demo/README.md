# iwdt_demo

IWDT in window mode, as distinct from `watchdog_demo`, which simply refreshes
until it stops. Polls the live 14-bit IWDTSR.CNTVAL counter and writes the
refresh sequence only while the counter sits inside the legal window, then
clears the error status so a window violation stays visible.

The real window bounds come from the OFS0 option-setting register, programmed
at flash time and not by this firmware. The demo's compiled window constants
are chosen to match the conventional EK-RA8D2 OFS0 layout that the project's
shared linker scripts use -- change one without the other and every refresh
lands outside the window.
