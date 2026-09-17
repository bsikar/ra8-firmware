# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show documentation help menu
default:
    @echo "DOCS - Documentation, Dashboards & Architecture Audits"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just docs::build          Generate Doxygen HTML documentation"
    @echo "  just docs::clean          Remove generated Doxygen HTML documentation"
    @echo "  just docs::zig            Generate Zig autodoc into build/docs/api/zig/"
    @echo "  just docs::zig_units      Gate: every Zig build root is documented or excluded"
    @echo "  just docs::zig_prose      Gate: public Zig declarations carry doc comments"
    @echo "  just docs::push           Build and publish Doxygen to gh-pages"
    @echo "  just docs::record_stats   Refresh historical HAL completion summary"
    @echo "  just docs::sizes          Summarize app .text/.data/.bss footprints"
    @echo "  just docs::audit_init     Audit peripheral init order across apps"
    @echo ""

# Remove generated Doxygen HTML documentation
clean:
    rm -rf build/docs

# Generate Doxygen HTML documentation into build/docs/html/
build:
    bash scripts/builders/docs.sh

# Generate Zig autodoc HTML into build/docs/api/zig/
zig:
    bash scripts/builders/docs_zig.sh

# Fail when a Zig build root is neither documented nor excluded on purpose
zig_units:
    python3 scripts/checks/check_zig_doc_units.py --selftest
    python3 scripts/checks/check_zig_doc_units.py --check

# Fail when a public Zig declaration in a documented unit has no /// doc comment
zig_prose:
    python3 scripts/checks/check_zig_doc_comments.py --selftest
    python3 scripts/checks/check_zig_doc_comments.py --check

# Build Doxygen docs and publish to the gh-pages branch
push: build
    /bin/bash -p scripts/builders/publish_docs.sh

# Refresh the summary embedded in the historical HAL completion record
record_stats:
    python3 scripts/report/roadmap_stats.py

# Summarize built app footprints into ignored build/reports/APP_SIZES.md
sizes:
    python3 scripts/report/app_sizes.py --write

# Run the read-only init-order audit across apps
audit_init:
    python3 scripts/checks/audit_init_order.py
