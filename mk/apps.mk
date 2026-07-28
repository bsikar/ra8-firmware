# mk/apps.mk -- firmware app discovery, build, clean.
#
# App-list variables (RA8_APPS, RA8_APP_DIR_<app>, ...) are defined in the top
# Makefile before this file is included, so every module sees them.
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: default apps clean compile_commands build-all $(RA8_APPS)

# `make` with no arg builds the default app.
default: $(RA8_DEFAULT_APP)

# `make apps` -- the app catalogue, grouped by tier dir (hardware-support
# maturity). Descriptions come from ra8_add_app(DESCRIPTION ...) per app.
apps:
	@printf '== FIRMWARE apps (%s) -- build: make <app> | flash: make flash-<app> | emulate: make emu-<app>\n' "$(words $(RA8_APPS))"
	@for tier_dir in $(ROOT)/examples/*/; do \
		tier=$$(basename "$$tier_dir"); \
		[ "$$tier" = "shared" ] && continue; \
		for main in "$$tier_dir"*/main.c "$$tier_dir"*/*/main.c "$$tier_dir"*/*/*/main.c; do \
			[ -f "$$main" ] || continue; \
			d=$$(dirname "$$main"); \
			app=$$(basename "$$d"); \
			group=$$(dirname "$${d#$(ROOT)/examples/}"); \
			desc=$$(sed -n 's/.*DESCRIPTION "\([^"]*\)".*/\1/p' "$$d/CMakeLists.txt" 2>/dev/null | head -1); \
			printf '%s\t%s\t%s\n' "$$group" "$$app" "$$desc"; \
		done; \
	done | sort | awk -F'\t' '{ if ($$1 != g) { g=$$1; printf "\n  [%s]\n", g } printf "    %-30s %s\n", $$2, $$3 }'
	@printf '\nUI preview: run the e-reader chrome on the emulator, e.g. make emu-ereader_ui [PANEL=ek_ra8d2]   (tools/ra8_emulator)\n'

# Forward `make <app>` to the per-app Makefile via RA8_APP_DIR_<app>, keeping
# build/compile_commands.json fresh when a CMake input changes (clangd reads it).
$(RA8_APPS): $(RA8_COMPILE_COMMANDS)
	$(MAKE) -C $(RA8_APP_DIR_$@) build

$(RA8_COMPILE_COMMANDS): $(_RA8_CMAKE_INPUTS)
	$(CMAKE) -DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake -B $(ROOT)/build $(ROOT)

# Convenience alias: `make compile_commands` forces an up-to-date check.
compile_commands: $(RA8_COMPILE_COMMANDS)

clean:
	@for d in $(ROOT)/examples/*/*/main.c $(ROOT)/examples/*/*/*/main.c $(ROOT)/examples/*/*/*/*/main.c; do \
		[ -f "$$d" ] || continue; \
		$(MAKE) -C "$$(dirname $$d)" clean; \
	done
	rm -rf $(TESTS_BUILD) $(TESTS_BUILD_COV) $(TESTS_BUILD_UBSAN) $(TIDY_BUILD) \
	       $(MEDIA_DL_DIR)/build $(RA8_VIEWER_DIR)/build

# `make build-all` -- cross-compile every firmware app (CI's "Cross-build all
# apps" job); per-app logs in build/build_all_examples/.
build-all:
	bash scripts/builders/all_examples.sh
