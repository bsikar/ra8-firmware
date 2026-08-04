# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# mk/docs.mk -- documentation + reports.

.PHONY: docs docs-push dashboard app-sizes audit-init

docs:
	bash scripts/builders/docs.sh

# `make docs-push` -- build the Doxygen HTML and force-publish it to gh-pages
# (the same script CI runs on push to main). See scripts/builders/publish_docs.sh.
docs-push: docs
	bash scripts/builders/publish_docs.sh

dashboard:
	python3 scripts/report/roadmap_dashboard.py

# `make app-sizes` -- per-app .text/.data/.bss footprints (writes docs/APP_SIZES.md).
app-sizes:
	python3 scripts/report/app_sizes.py --write

# `make audit-init` -- per-app init-order linter (writes docs/INIT_ORDER_AUDIT.md).
audit-init:
	python3 scripts/checks/audit_init_order.py --report docs/INIT_ORDER_AUDIT.md
