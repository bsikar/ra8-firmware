# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show documentation help menu
default:
    @echo "DOCS - Documentation, Dashboards & Architecture Audits"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just docs::hub            Build the Markdown documentation hub"
    @echo "  just docs::hub_serve      Serve the hub locally with live reload"
    @echo "  just docs::hub_check      Gate: every docs/ page is reachable"
    @echo "  just docs::build          Generate the C ABI reference (Doxygen)"
    @echo "  just docs::capi           Generate the C ABI reference (/api/c/)"
    @echo "  just docs::capi_check     Check the C ABI header manifest"
    @echo "  just docs::clean          Remove generated Doxygen HTML documentation"
    @echo "  just docs::push           Build and publish Doxygen to gh-pages"
    @echo "  just docs::site           Assemble the whole documentation site"
    @echo "  just docs::site_check     Check the documentation site slot manifest"
    @echo "  just docs::record_stats   Refresh historical HAL completion summary"
    @echo "  just docs::sizes          Summarize app .text/.data/.bss footprints"
    @echo "  just docs::audit_init     Audit peripheral init order across apps"
    @echo ""

# Remove generated documentation (Doxygen HTML and the hub)
clean:
    rm -rf build/docs

# Build the Markdown documentation hub into build/docs/hub/ (ADR-0005)
hub:
    bash scripts/builders/docs_hub.sh

# Serve the documentation hub locally with live reload
hub_serve:
    bash scripts/builders/docs_hub.sh --serve

# Fail when a docs/ Markdown page is in neither the hub nav nor exclude_docs
hub_check:
    python3 scripts/checks/check_docs_hub_nav.py

# Generate Doxygen HTML documentation into build/docs/html/
build:
    bash scripts/builders/docs.sh

# Generate the C ABI Doxygen reference into build/docs/api/c/
capi:
    bash scripts/builders/docs_capi.sh

# Fail when a library's public headers are missing from the C ABI reference
capi_check:
    python3 scripts/checks/check_c_abi_doc_headers.py --check

# Assemble every wired generator into build/docs/site/ (ADR-0005)
site:
    bash scripts/builders/docs_site.sh

# Fail when a documentation site slot's state stops matching the tree
site_check:
    python3 scripts/checks/check_docs_site_slots.py --check

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
