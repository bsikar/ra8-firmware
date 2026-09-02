# Plan: Quoting safety

## Epic: inj-epic -- Title with $(rm -rf /) and `backquotes`; and a semicolon
First body line; rm -rf / would be very bad.
Second body line with $(id) and `whoami` in it.
- labels: priority:P0, epic:inj-epic, area:$(rm -rf /), needs;review
- priority: P0
- track: CI health
- status: Ready

### Issue: inj-issue -- Another $(rm -rf /) title with 'single' and "double" quotes
A body that spans
two lines, so the emitted argument contains a newline.
- labels: priority:P0, epic:inj-epic, area:scripts
- priority: P0
- track: CI health
- status: Ready
- depends-on: inj-epic
