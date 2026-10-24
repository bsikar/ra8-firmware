# txm_sd_tour_m85

The syscall-tour proof of concept for `ra8_app_api_t` (RA8FW-1023, under the
ThreadX Modules M85 epic RA8FW-289): one app, one module, and every request
the call table defines driven end to end.

CPU0 brings up the micro-SD card, mounts its FAT volume and reads the signed
`txm_tour_m33.ra8app` (card.zig), admits it through ra8_app's gate with the
pinned test key and refuses a tampered copy (verify.zig), reads the
manifest, and runs the module through the ThreadX Module Manager
(module.zig). The image's `_txm_module_manager_application_request`
(backend.zig) is the hook the manager's kernel dispatch calls: it names the
module from the admitted manifest, reads the module's data range off the
manager's module instance, and serves each request through
`app_api.kernel.dispatch` with this app's backend:

- a 64x64 ARGB8888 screen region backed by a shadow buffer,
- files under `/apps/com.ra8.txm_tour_m33/` on the same volume (ra8_fs),
- the tick clock (1 ms per tick) and a sleep,
- an empty touch queue.

The module (examples/ek_ra8d2/hw_pending/txm_tour_m33/module_start.zig)
binds the typed shims (`libs/ra8_app/src/internal/api/module.zig`) and walks
the table: api_version, ticks, sleep, screen_info, draw_rect, draw_image, a
file write/read round trip under its own directory, the empty touch_event,
then `exit(0)`. Every dispatch logs one line (`request N -> status`), the
pass line goes last, and the unload must give the manager's byte pool back
whole.

## Build and run

`zig build txm-tour-m33` installs the signed module under `zig-out/arm/`,
and the app image builds with the rest of the M85 apps (`zig build arm`,
`zig-out/arm/txm_sd_tour_m85.elf` and `.bin`). Copy `txm_tour_m33.ra8app`
to the card root and run; the backend creates `/apps` and the module's own
directory on the first create. The verdict goes to the SCI8 / J-Link OB
VCOM console. Not yet run on the bench (hw_pending).
