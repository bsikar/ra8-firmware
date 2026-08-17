# reset_cause_demo

Checks that `ra8_reset_get_cause()` decodes `RSTSR1.SWRF` correctly (#52). The
app boots, triggers a software reset via `ra8_reset_software_reset()`, and on
the post-reset boot enters a loop that advances `g_reset_cause_loop` **only**
when the observed cause reads back as software.

That single condition is what makes the check meaningful without a console: a
counter that advances at all is proof the software-reset round-trip completed
and was correctly identified, so a probe sampling it across a few seconds needs
no other evidence. `g_reset_cause_initial` holds the raw
`ra8_reset_cause_t` observed on this boot, for triage when the counter is stuck.
