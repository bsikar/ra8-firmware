# Tools and scripts inventory

Per RA8FW-998: every host tool and generator under `tools/` and `scripts/`, with the
consumer that runs it and the keep-and-gate vs delete decision. Method: exact-name
reference scan across this repository plus the lab automation, whose gate and HIL
scripts run from this checkout's root and therefore consume this tree's copies.
Generators with no consumer are deleted, not gated. Every kept tool names a consumer.

Gate means the unit is held to the current build bar: a test contract, a recipe, a
CMake test target, or a selftest wired into a gate suite.

## tools/

| Unit | Consumer | Gate | Decision |
| --- | --- | --- | --- |
| `tools/cache_bench` | `just/tools.just`, `scripts/builders/build_host_tools.sh`, `tests/cmake/zig_libraries.cmake`, referenced from `docs/SOUP/miniz.md` and the qualification docs | CMake unit test (`tests/src/test_cache_bench.c`) | keep |
| `tools/check_portability.py` | `just/checks.just` (`checks::portability`, `checks::portability-image-builds`); consumes `tools/portability_gate_libc/` | self-test inside the recipe | keep-and-gate |
| `tools/check_txm_module_relocs` | `build.zig.zon` dependency; `tests/zig_build_graph/txm_module_*` | `.zig-test-contract.json` suite | keep |
| `tools/epub_compile` | `scripts/builders/books.sh`; consumed by `apps/shared_libs/book`, `apps/shared_libs/rabook_compile`, format docs | `tests/epub_selftest.py` | keep |
| `tools/exfat_mkimage` | `scripts/dev/exfat_macos_interop.sh`; built through the Zig graph (`build.zig`, RA8FW-999) | `zig build test` (integration suite) | keep |
| `tools/font_to_c` | five example CMakeLists (`pagecache`, `ereader_ui`, `reflow_tofu_demo`, `reflow_webp_demo`, `sd_font_render`); `libs/ra8_fonts/literata_latin1.h` is its output | `.zig-test-contract.json` suite | keep |
| `tools/gen_jlink_w4` | `just/tools.just` via `scripts/builders/gen_jlink_w4.sh` | `.zig-test-contract.json` suite | keep |
| `tools/glyph_bench` | `just/tools.just`, `scripts/builders/build_host_tools.sh`, `tests/cmake/zig_libraries.cmake` | CMake build | keep |
| `tools/list_tests` | `just/tests.just` via `scripts/builders/list_tests.sh`; `libs/ra8_widget/build.zig` | `.zig-test-contract.json` suite | keep |
| `tools/mcp` | `.mcp.json` (agent MCP server); `scripts/dev/run_just.sh`; `tools/mcp/README.md` | `tests/mcp_selftest.py` | keep |
| `tools/merge_ihex` | `build.zig.zon`, `cmake/ra8_add_ns_image.cmake`, `cmake/ra8_zig_tool.cmake`, secure-boot NS example | `.zig-test-contract.json` suite | keep |
| `tools/mkbookimg` | root `CMakeLists.txt`; `scripts/emu/smoke_apps.sh`, `scripts/emu/smoke_run.sh`; `tests/cmake/unit_tests.cmake` | CMake build + `tests/integration.sh` | keep |
| `tools/mkfontimg` | root `CMakeLists.txt`; `scripts/emu/eil_all.sh`, `emu_fixtures.sh`, `smoke_*`; `import_reader` and `sd_font_render` examples | CMake build + `tests/integration.sh` | keep |
| `tools/portability_gate_libc` | `tools/check_portability.py` (the gate's scan input) | covered by the portability gate | keep |
| `tools/ra8_emulator` | git submodule (root `.gitmodules`); `README.md`, devcontainer, VS Code configs | upstream repo's own gates | keep |
| `tools/rabook_imagepack` | root `CMakeLists.txt`; `cmake/ra8_app/sources.cmake`, `cmake/ra8_webp_vendor.cmake`; format docs | CMake build + five integration/unit suites | keep |
| `tools/rabook_viewer` | root `CMakeLists.txt`; `just/tools.just`; `tests/cmake/zig_libraries.cmake`; format docs | CMake build + corpus/workspace suites | keep |
| `tools/reader_vmem` | `just/tools.just`, `scripts/builders/build_host_tools.sh`, `tests/cmake/zig_libraries.cmake`; consumed by `tools/cache_bench` `sweep_block` | CMake unit test | keep |
| `tools/roadmap_stats` | `just/docs.just`; regenerates `docs/ROADMAP.md` and the qualification docs | `.zig-test-contract.json` suite | keep |
| `tools/vela` | NPU model compiler, pinned in `build.zig.zon`; serves the `npu_vela` example; SOUP record `docs/SOUP/vela.md` | `tools/vela/tests/test_vela_gen.py` | keep |
| `tools/zig_build` | root `build.zig` and every host Zig build (`apps/host/firmware_pipeline`, `image_pyramid`, `reg_gen`); `cmake/ra8_add_ns_image.cmake` | `.zig-test-contract.json` suite | keep |

## scripts/builders

| Script | Consumer | Decision |
| --- | --- | --- |
| `all_examples.sh` | `just/example.just`; referenced from the cmake layer and `docs/adr/0011` | keep |
| `books.sh` | `just/tools.just`; drives `tools/epub_compile` | keep |
| `build_cross_compile_db.py` | `just/apps.just`; consumed by root `build.zig` and `tests/zig_build_graph` | keep |
| `build_host_tools.sh` | `just/tools.just` | keep |
| `build_shared_libs.sh` | `just/shared.just` | keep |
| `gen_jlink_w4.sh` | `just/tools.just`; drives `tools/gen_jlink_w4` | keep |
| `host_cmake.sh` | `just/host.just`, `just/tests.just`, `just/tools.just`, `just/app_test.just` | keep |
| `list_tests.sh` | `just/tests.just`; drives `tools/list_tests` | keep |
| `roadmap_stats.sh` | `just/docs.just`; drives `tools/roadmap_stats` | keep |
| `select_host_compiler.sh` | `just/apps.just`, `scripts/builders/host_cmake.sh`, `scripts/emu/*`, `tests/build_tests.sh` | keep |

## scripts/checks

| Script | Consumer | Decision |
| --- | --- | --- |
| `audit_init_order.py` | documented in `docs/DOCS.md` and `just/docs.just`; annotations in the manual examples point at it | keep |
| `check_cmake_source_paths.py` | cited by `cmake/ra8_app/sources.cmake` | keep |
| `check_comment_format.py` | `.clang-format`, `docs/STYLE_GUIDE.md`, `just/tools.just` | keep |
| `check_file_size.py` | cited by the files it caps; `tests/zig_build_graph`; `scripts/checks/lint_targets.py` | keep |
| `check_freestanding_runtime.py` | `just/apps.just`; `docs/ARCHITECTURE.md` | keep |
| `check_rtos_symbol_isolation.py` | the lab automation's gate suite, which runs this checkout's copy; carries the RA8FW-363 Zig-sources sweep (`.zig` suffixes, `@extern` string handling) | keep |
| `check_sg_offsets.py` | `cmake/toolchain-ra8d2.cmake` and the SG-using example CMakeLists | keep |
| `check_tool_versions.py` | `.devcontainer/Dockerfile`, `cmake/ra8_app/zig_libs.cmake`, `docs/TOOLCHAIN.md` | keep |
| `freestanding_runtime_selftest.py` | `scripts/checks/check_freestanding_runtime.py` | keep |
| `freestanding_static_checks.py` | `scripts/checks/check_freestanding_runtime.py` and the runtime selftest | keep |
| `lint_targets.py` | `pyproject.toml`, `.gitignore` | keep |
| `run_fuzz.sh` | `tests/fuzz/CMakeLists.txt`, `docs/FUZZING.md`, the fuzz targets | keep |
| `selftest_assert.py` | `scripts/checks/audit_init_order.py` | keep |
| `stack_usage_check.py` | `cmake/ra8_add_app.cmake`, `cmake/ra8_app/sources.cmake`, `cmake/ra8_shared_libs.cmake`, `cmake/ra8_warnings.cmake`, `cmake/ra8_webp_vendor.cmake`, `docs/STACK_USAGE.md` | keep |

## scripts/dev

| Script | Consumer | Decision |
| --- | --- | --- |
| `assemble_candidate.py` | `scripts/dev/git_environment.py` | keep |
| `bootstrap_uv.py` | `scripts/dev/setup_python.sh`, `.devcontainer/Dockerfile`, `docs/PYTHON_ENVIRONMENTS.md` | keep |
| `bootstrap_uv_exec.py` | `scripts/dev/bootstrap_uv.py` | keep |
| `bootstrap_uv_mode_selftest.py` | `scripts/dev/bootstrap_uv.py` | keep |
| `debug.sh` | `just/hw.just`, `docs/DEBUG.md` | keep |
| `exfat_macos_interop.sh` | `tools/exfat_mkimage/CMakeLists.txt` | keep |
| `flash.sh` | `just/hw.just` context; `docs/DEBUG.md`, the coprocessor flow | keep |
| `git_environment.py` | `scripts/dev/git_environment.sh` | keep |
| `git_environment.sh` | `scripts/gen/gen_sbom.py`, `scripts/gen/sbom_selftest.py`, `scripts/builders/build_cross_compile_db.py`, `scripts/dev/assemble_candidate.py` | keep |
| `list_libs.py` | `just/libs.just` | keep |
| `managed_python_env.py` | `scripts/dev/setup_python.sh` | keep |
| `managed_python_env_checks.py` | `scripts/dev/managed_python_env.py` | keep |
| `monitor.sh` | `just/hw.just` | keep |
| `openocd_debug.sh`, `openocd_flash.sh` | `just/hw.just`, `docs/DEBUG.md` | keep |
| `ozone.sh` | `just/hw.just`, `docs/DEBUG.md` | keep |
| `ra8_apps.py` | `just/filter.just`, the example HIL configs and docs | keep |
| `remote_gdb_args.py` | none in either tree; the IDE remote-debug tunnel is owned by the lab automation's own server (`just hil::remote_gdb` resolves there; `docs/IDE.md` and `.vscode/tasks.json` name the recipe, not these files) | **delete** |
| `remote_gdb_guard.py` | none; only `scripts/dev/remote_gdb_process.py` imports it | **delete** |
| `remote_gdb_process.py` | only `scripts/dev/remote_gdb_guard.py` | **delete** |
| `remote_gdb_remote.py` | none | **delete** |
| `run_just.sh` | `.mcp.json`, `.vscode/tasks.json`, `docs/IDE.md`, the emu scripts | keep |
| `scaffold.py` | `just/board.just`, `just/example.just`, `just/host.just`, `just/libs.just`, `just/shared.just` | keep |
| `search.py` | `justfile` | keep |
| `setup_python.sh` | `justfile`, `README.md`, `CONTRIBUTING.md`, the cmake Zig-lib layer | keep |
| `ssh_loopback_proxy.py` | none of this copy: the lab automation carries and runs its own, newer revision; no reference to this copy in either tree | **delete** |
| `stage_worktree_context.py` | the lab automation's gate suite (`--selftest`) and the dev-box provisioning role, both of which stage this checkout's copy | keep |
| `tty_resolve.sh` | `scripts/dev/monitor.sh`, the coprocessor flash flow | keep |
| `uv_release.json` | `scripts/dev/bootstrap_uv.py`, the devcontainer | keep |
| `verify_locked_environment.py` | the lab automation's gate suite (`--selftest`) and its bench/k3s provisioning roles, which take this checkout's copy | keep |

## scripts/emu

| Script | Consumer | Decision |
| --- | --- | --- |
| `eil_all.sh` | the example `hil.conf` files and `scripts/emu/README.md` | keep |
| `emu_fixtures.sh` | `scripts/emu/eil_all.sh`, `matrix.sh`, `smoke.sh` | keep |
| `matrix.sh` | `scripts/emu/README.md`, `eil_all.sh`, `matrix_triage.sh` | keep |
| `matrix_triage.sh` | `scripts/emu/matrix.sh` | keep |
| `setup_macos.sh` | `docs/TOOLCHAIN.md` | keep |
| `smoke.sh` | the example HIL docs and configs; drives the other smoke scripts | keep |
| `smoke_apps.sh` | `scripts/emu/smoke.sh`, `docs/adr/0016` | keep |
| `smoke_assert.sh` | `scripts/emu/smoke.sh` | keep |
| `smoke_run.sh` | `scripts/emu/smoke.sh`, `smoke_apps.sh` | keep |

## scripts/gen

| Script | Consumer | Decision |
| --- | --- | --- |
| `build_chapter_map.sh` | `docs/ROADMAP.md`, `docs/reference/CHAPTER_MAP.md` | keep |
| `display_atlases.py` | `scripts/gen/sbom_registry.py`, `libs/ra8_fonts/README.md`; imported by the atlas verifier | keep |
| `ereader_golden.py` | `scripts/emu/smoke_apps.sh`, `scripts/emu/smoke_run.sh`, `tests/golden/ereader_chrome/README.md` | keep |
| `gen_bluenoise_mask.py` | `just/tools.just`; its output backs `libs/ra8_gfx` dithering | keep |
| `gen_comic_fixture.py` | the `ereader_comic` example (fixture header, HIL config, docs) | keep |
| `gen_comic_large_fixture.py` | the `ereader_comic` example's large fixture header | keep |
| `gen_driver_status.py` | `docs/DRIVER_STATUS.md` | keep |
| `gen_manga_page_fixture.py` | the `ereader_manga` example and the shelf test | keep |
| `gen_pinouts.py` | `docs/pinouts/*` (the generated pinout tables); consumes the `pinout_*` modules | keep |
| `gen_sbom.py` | `docs/sbom/ra8-firmware.cdx.json`, `THIRD_PARTY_LICENSES.md`, the SOUP records | keep |
| `gen_unarch_xz_fixture.py` | `apps/shared_libs/unarch/tests` | keep |
| `literata_atlas.py` | `libs/ra8_gfx/src/internal/literata_atlas.NOTICE`; imported by the atlas verifier | keep |
| `pinout_model.py`, `pinout_parse.py`, `pinout_render.py`, `pinout_selftest.py` | `scripts/gen/gen_pinouts.py` | keep |
| `rabook_gray8_fixture.py` | `just/tools.just`; the `ereader_rabook` example's fixture header | keep |
| `rabook_parity_gen.py` | `just/tools.just`; the rabook_compile parity fixtures and tests | keep |
| `sbom_registry.py` | the SOUP records and `THIRD_PARTY_LICENSES.md` cite it as the registry authority | keep |
| `sbom_selftest.py` | `scripts/gen/gen_sbom.py` | keep |
| `test_atlas_v2.py` | no runner existed; verified passing this change (all four atlases decode byte-for-byte to the v1 coverage, v2 saves 29.4%) and wired as `just checks::atlas-parity` | keep-and-gate |
| `widget_icons.py` | regenerates the committed `libs/ra8_widget/src/widget_icons.zig`, `icons/`, and `icons_atlas.bin` that `ra8_widget_abi.zig` imports | keep |

## scripts/git, scripts/lib, scripts/report, scripts/secrets

| Script | Consumer | Decision |
| --- | --- | --- |
| `scripts/git/*` (hooks, `hook-launcher`, `install-hooks.sh`) | `justfile` installs them; the hooks run through `hook-launcher` | keep |
| `scripts/lib/parallelism.sh` | `scripts/builders/*`, `scripts/checks/run_fuzz.sh`, `scripts/emu/*`, `tests/build_tests.sh` | keep |
| `scripts/report/app_sizes.py` | `just/docs.just`, `docs/DOCS.md` | keep |
| `scripts/secrets/rot_keystore.py` | `scripts/secrets/rot_provision.sh`, `rot_patch_pubkey.py`; `docs/ROOT_OF_TRUST.md` | keep |
| `scripts/secrets/rot_patch_pubkey.py` | `scripts/secrets/rot_provision.sh` | keep |
| `scripts/secrets/rot_provision.sh` | `docs/ROOT_OF_TRUST.md`, `libs/ra8_rot/src/rot_abi.zig`, `tools/README.md` | keep |
| `scripts/secrets/rot_sign.py` | the secure-boot and RoT example flows and format docs | keep |
| `scripts/secrets/wifi_provision.py` | the lab automation's HIL run scripts and build-credential checker, which run this checkout's copy | keep |

## Removed in this change

- `scripts/dev/remote_gdb_args.py`, `remote_gdb_guard.py`, `remote_gdb_process.py`,
  `remote_gdb_remote.py`: no consumer in either tree. The IDE remote-debug tunnel is
  owned by the lab automation's own guarded server; `docs/IDE.md` and
  `.vscode/tasks.json` name the `hil::remote_gdb` recipe, which resolves in the lab
  checkout, not these files. Last touched at the repository migration checkpoint.
- `scripts/dev/ssh_loopback_proxy.py`: the lab automation carries and runs its own,
  newer revision of this helper; nothing in either tree references this copy.

## Kept and newly gated

- `scripts/gen/test_atlas_v2.py` had no consumer and no runner. It verifies the
  committed font atlases (v2 RLE decodes byte-for-byte to the v1 quantized coverage
  for every glyph; v2 saves 29.4 percent over v1) and passes today, so it is kept and
  gated as `just checks::atlas-parity` instead of deleted.
