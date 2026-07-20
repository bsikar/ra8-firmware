# mk/docs.mk -- documentation + reports.
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: docs docs-push dashboard app-sizes audit-init

docs:
	bash scripts/build_docs.sh

# `make docs-push` -- build the Doxygen HTML and force-publish it to gh-pages
# (the same script CI runs on push to main). See scripts/publish_docs.sh.
docs-push: docs
	bash scripts/publish_docs.sh

dashboard:
	python3 scripts/utils/roadmap_dashboard.py

# `make app-sizes` -- per-app .text/.data/.bss footprints (writes docs/APP_SIZES.md).
app-sizes:
	python3 scripts/utils/app_sizes.py --write

# `make audit-init` -- per-app init-order linter (writes docs/INIT_ORDER_AUDIT.md).
audit-init:
	python3 scripts/utils/audit_init_order.py --report docs/INIT_ORDER_AUDIT.md
