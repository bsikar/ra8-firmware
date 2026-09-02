# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reviewed parsing and absence policy for Markdown reference validation."""

import re

VENDOR_PREFIXES = ("libs/third_party/", "apps/shared_libs/third_party/")
AUTHORED_VENDOR_INDEXES = frozenset(
    {"libs/third_party/README.md", "apps/shared_libs/third_party/README.md"}
)
REPO_PREFIXES = (
    ".claude/",
    ".github/",
    ".tools/",
    "apps/",
    "cmake/",
    "coprocessor/",
    "docs/",
    "examples/",
    "infra/",
    "just/",
    "libs/",
    "port/",
    "scripts/",
    "tests/",
    "tools/",
)
COMPONENT_RELATIVE_PREFIXES = ("inc/", "src/", "tests/")
ROOT_FILE_TOKENS = (
    "AGENTS.md",
    "CHANGELOG.md",
    "CLAUDE.md",
    "CMakeLists.txt",
    "CONTRIBUTING.md",
    "Doxyfile",
    "README.md",
    "THIRD_PARTY_LICENSES.md",
    "justfile",
    "pyproject.toml",
    "uv.lock",
)
ROOT_FILE_PATTERN = (
    r"(?:[A-Z][A-Z0-9_-]*\.(?:md|toml|lock|txt|ya?ml)|"
    + "|".join(re.escape(path) for path in ROOT_FILE_TOKENS)
    + r")"
)
BARE_MARKDOWN_PATTERN = r"[a-z0-9][A-Za-z0-9_-]*\.[Mm][Dd]"
BARE_FILE_SUFFIXES = (
    "S",
    "bin",
    "bmp",
    "c",
    "cbr",
    "cbz",
    "cc",
    "cmake",
    "conf",
    "cpp",
    "css",
    "csv",
    "dtsi",
    "elf",
    "env",
    "epub",
    "gif",
    "gz",
    "h",
    "hex",
    "hpp",
    "html",
    "jof",
    "jpeg",
    "jpg",
    "js",
    "json",
    "just",
    "ld",
    "manifest",
    "map",
    "o",
    "opf",
    "otf",
    "patch",
    "pdf",
    "png",
    "py",
    "rar",
    "sh",
    "svg",
    "tar",
    "toml",
    "tsv",
    "txt",
    "webp",
    "xhtml",
    "xz",
    "yaml",
    "yml",
    "zip",
)
BARE_FILE_SUFFIX_PATTERN = "|".join(re.escape(suffix) for suffix in BARE_FILE_SUFFIXES)
BARE_CODE_FILE_RE = re.compile(
    r"(?<![A-Za-z0-9_./*?{}<>$-])((?:(?:[A-Za-z0-9_.,-]|<[a-z][a-z0-9_-]*>|"
    r"\$\{[A-Z][A-Z0-9_]*}|\{[a-z][a-z0-9_]*}|"
    r"\{[A-Za-z0-9_-]+(?:,[A-Za-z0-9_-]+)+\}|[*?])+\."
    rf"(?:{BARE_FILE_SUFFIX_PATTERN})))"
    r"(?![A-Za-z0-9_./*?{}<>$-])"
)

MIN_TRACKED_MARKDOWN = 450
MIN_FIRST_PARTY_MARKDOWN = 370
MIN_VENDOR_MARKDOWN = 75
MIN_LINK_REFERENCES = 550
MIN_PATH_REFERENCES = 500
PARSER_RUNTIME_LIMIT_SECONDS = 2.0

REMOTE_SCHEMES = frozenset({"data", "ftp", "git", "http", "https", "mailto", "ssh", "tel"})
TRAILING_PATH_JUNK = ".,;:!?)`'\"|"
PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_./-])((?:"
    + ROOT_FILE_PATTERN
    + "|"
    + BARE_MARKDOWN_PATTERN
    + r"|(?:(?:\.\./)*(?:"
    + "|".join(re.escape(prefix) for prefix in (*REPO_PREFIXES, *COMPONENT_RELATIVE_PREFIXES))
    + r")[A-Za-z0-9_./*?{}$@,+<>:-]+|(?:\.\./)+[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)))"
)
HTML_TARGET_RE = re.compile(r"\b(?:href|src)\s*=\s*([\"'])(.*?)\1", re.IGNORECASE)
EXPLICIT_ANCHOR_RE = re.compile(r"\b(?:id|name)\s*=\s*([\"'])(.*?)\1", re.IGNORECASE)
HTML_PATH_SEPARATOR_RE = re.compile(r"(?:<br\b[^>]*>|</[A-Za-z][A-Za-z0-9_-]*\s*>)", re.IGNORECASE)
REFERENCE_DEF_RE = re.compile(r"^\s{0,3}\[(?!\^)([^]]+)]\s*:\s*(\S.*)$")
REFERENCE_USE_RE = re.compile(r"\[([^]]+)]\[([^]]*)]")
SHORTCUT_PATH_REFERENCE_RE = re.compile(
    r"(?<![!\]])\[((?:(?:\.\./)*(?:"
    + "|".join(re.escape(prefix) for prefix in REPO_PREFIXES)
    + r")[A-Za-z0-9_./-]+|"
    + ROOT_FILE_PATTERN
    + r"))](?!\s*[\[(])"
)
ATX_HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
SETEXT_HEADING_RE = re.compile(r"^\s{0,3}(?:=+|-+)\s*$")
FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
LINE_CITATION_RE = re.compile(r":\d+(?:-\d+)?(?::\d+)?$")
LOCAL_LINE_FRAGMENT_RE = re.compile(r"L\d+(?:-L?\d+)?$", re.IGNORECASE)
SYMBOL_SUFFIX_RE = re.compile(r"::.*$")
SOUP_LOCAL_PATH_RE = re.compile(r"^- \*\*Local path\*\*: `([^`]+)`\s*$", re.MULTILINE)
WORK_FIXTURE_PATH = "../escape"
SOUP_DECLARED_ABSENCES = {
    "docs/SOUP/libwebp.md": frozenset({"src/enc/*.c", "src/mux/", "src/demux/"})
}
LIBWEBP_ABSENCE_CLAUSE = (
    "  (`src/enc/*.c`), the muxer/demuxer (`src/mux/`, `src/demux/`), "
    "`sharpyuv/`, the CLI tools (`examples/`, `imageio/`) and `extras/` are "
    "**not** vendored"
)
QUALIFICATION_RELEASE_SOURCES = frozenset(
    {
        "docs/qualification/SCMP.md",
        "docs/qualification/SQAP.md",
        "docs/qualification/release/README.md",
    }
)
TOOL_PRIVATE_VENDOR_SOURCES = frozenset(
    {
        "apps/shared_libs/README.md",
        "docs/SOUP/README.md",
        "docs/sbom/patches/README.md",
        "libs/third_party/README.md",
        "tools/README.md",
    }
)
TOOL_PRIVATE_OWNERSHIP_INDEXES = frozenset(
    {"apps/shared_libs/README.md", "libs/third_party/README.md"}
)
TOOL_PRIVATE_CLAUSE_PATTERNS = {
    "apps/shared_libs/README.md": r"The future path `{token}` must not become",
    "docs/SOUP/README.md": r"may live at `{token}`, but only when it is truly tool-exclusive",
    "docs/sbom/patches/README.md": r"future explicitly registered `{token}` component",
    "libs/third_party/README.md": r"The future path is `{token}`, but only with an SBOM",
    "tools/README.md": r"`{token}` subtree is reserved for a dependency used exclusively",
}

SYSTEM_HEADER_BASENAMES = frozenset(
    {
        "arm_cmse.h",
        "assert.h",
        "ethosu_driver.h",
        "setjmp.h",
        "stdarg.h",
        "stdbool.h",
        "stdio.h",
    }
)
DECLARED_BARE_CODE_FILES = {
    ("apps/host/mdl/README.md", "robots.txt"): "remote HTTP resource name",
    ("CLAUDE.md", "_internal.h"): "documented internal-header naming convention",
    ("PHILOSOPHIES.md", "sqlite3.c"): "linked upstream SQLite amalgamation example",
    ("docs/HIL_SUITE.md", "dwf.h"): "header installed from the external WaveForms package",
    ("docs/COVERAGE.md", "summary.json"): "generated coverage report",
    ("docs/COVERAGE.md", "compile_commands.json"): "generated CMake compilation database",
    ("docs/COVERAGE.md", "summary.txt"): "generated coverage report",
    ("THIRD_PARTY_LICENSES.md", "miniz-3.0.2.zip"): "pinned upstream release artifact",
    (
        "coprocessor/esp32c6/README.md",
        "wifi.env",
    ): "gitignored local Wi-Fi credentials beside the tracked template",
    ("docs/ARCHITECTURE.md", "crt0.o"): "conceptual toolchain startup-object name",
    ("docs/DOCS.md", "footer.html"): "discarded doxygen HTML-template output",
    ("docs/IDE.md", "compile_commands.json"): "generated CMake compilation database",
    (
        "docs/INFRASTRUCTURE.md",
        "k3s-runner-maintenance.sh",
    ): "explicitly host-owned script outside this repository",
    ("docs/RING_AND_WORLD.md", "my_driver.c"): "illustrative @file fixture name",
    ("docs/ROOT_OF_TRUST.md", "history.json"): "runtime root-of-trust state file",
    ("docs/ROADMAP.md", "ra8_xxx_regs.h"): "archived driver-template placeholder",
    ("docs/STACK_USAGE.md", "${RA8_APP_NAME}.elf"): "generated app ELF template",
    ("docs/SOUP/esp-hosted-host.md", "esp_timer.h"): "explicitly unvendored upstream header",
    (
        "docs/SOUP/esp-hosted-host.md",
        "esp_wifi_types.h",
    ): "explicitly unvendored upstream header",
    ("docs/SOUP/esp-hosted-host.md", "portmacro.h"): "upstream FreeRTOS header",
    ("docs/SOUP/README.md", "miniz-3.0.2.zip"): "pinned upstream release artifact",
    ("docs/SOUP/README.md", ".tar.xz"): "archive-format extension, not a repository file",
    ("docs/SOUP/miniz.md", "miniz-3.0.2.zip"): "pinned upstream release artifact",
    (
        "docs/SOUP/tflite-micro.md",
        "*_test.cc",
    ): "explicitly excluded upstream test-source pattern",
    ("docs/SOUP/xz_embedded.md", ".tar.xz"): "archive-format extension, not a repository file",
    (
        "docs/SOUP/tf-psa-crypto.md",
        "generate_driver_wrappers.py",
    ): "upstream generation tool, not vendored source",
    ("docs/VENDOR_BLOBS.md", "r_rsip_*.c"): "explicitly absent protected FSP sources",
    ("docs/VENDOR_BLOBS.md", "hw_sce_*.c"): "older spelling of absent protected FSP sources",
    ("docs/VENDOR_BLOBS.md", "r_rsip_reg.h"): "explicitly absent protected FSP header",
    ("docs/VENDOR_BLOBS.md", "r_rsip_util.h"): "explicitly absent protected FSP header",
    ("docs/formats/BINARY_FORMATS.md", "foo.bin"): "illustrative binary input name",
    ("docs/formats/JOF.md", "sample.jof"): "generated walkthrough output",
    ("docs/formats/JOF.md", "sample.png"): "generated walkthrough input",
    ("docs/formats/ROT1.md", "body.bin"): "protocol payload filename",
    ("docs/formats/ROT1.md", "signed.bin"): "generated signed-envelope example",
    (
        "docs/sbom/patches/README.md",
        "0001-description.patch",
    ): "illustrative ordered patch filename",
    (
        "docs/sbom/patches/README.md",
        "0002-description.patch",
    ): "illustrative ordered patch filename",
    ("docs/qualification/HW_IN_LOOP_RUNNER.md", "svc.sh"): "retired runner tombstone",
    ("docs/qualification/HW_IN_LOOP_RUNNER.md", "config.sh"): "retired runner tombstone",
    (
        "docs/qualification/MISRA_DEVIATIONS.md",
        "misra.py",
    ): "cppcheck-owned upstream addon",
    ("docs/qualification/SVCP.md", "mcdc.txt"): "generated MC/DC report",
    ("docs/qualification/SVCP.md", "summary.txt"): "generated evidence summary",
    ("docs/qualification/SDD.md", "_internal.h"): "documented internal-header convention",
    ("docs/qualification/SRS.md", "_regs.h"): "documented register-header suffix",
    ("docs/qualification/SVR.md", "r_usb_pdriver.c"): "upstream FSP source comparison",
    (
        "docs/reference/ra8p1_vs_ra8d2.md",
        "bsp_linker.c",
    ): "upstream RASC-generated source comparison",
    (
        "docs/reference/ra8p1_vs_ra8d2.md",
        "R7KA8{P1,D2}KF_core0.h",
    ): "upstream FSP device-header brace pattern",
    (
        "examples/ek_ra8d2/hw_pending/ereader_zoom/README.md",
        "dst.h",
    ): "structure-member expression, not a filename",
    (
        "examples/ek_ra8d2/hw_pending/i2c_peripheral_responder/README.md",
        "bsp_elc.h",
    ): "explicitly absent upstream FSP header",
    ("infra/network/PI_PROVISIONING.md", "custom.toml"): "Raspberry Pi boot-media file",
    ("infra/network/PI_PROVISIONING.md", "userconf.txt"): "Raspberry Pi boot-media file",
    ("infra/network/PI_PROVISIONING.md", "cmdline.txt"): "Raspberry Pi boot-media file",
    ("infra/network/PI_PROVISIONING.md", "config.txt"): "Raspberry Pi boot-media file",
    ("infra/network/README.md", "userconf.txt"): "Raspberry Pi boot-media file",
    ("scripts/emu/README.md", "eil.conf"): "explicitly absent EIL configuration file",
    ("scripts/secrets/README.md", "init.json"): "external OpenBao initialization response",
    ("scripts/secrets/README.md", "approle.env"): "operator-owned credential output",
    ("scripts/secrets/README.md", "values.env"): "operator-owned secret input",
    (
        "tests/golden/ereader_chrome/README.md",
        "ereader_ui.elf",
    ): "generated firmware input for the golden test",
    ("tests/fixtures/webp/README.md", "pattern.png"): "generated fixture source image",
    ("tests/fixtures/webp/README.md", "tall.png"): "generated fixture source image",
    ("tests/fixtures/webp/README.md", "wide.png"): "generated fixture source image",
}

DECLARED_BARE_CONTEXT_SHA256 = {
    ("CLAUDE.md", "_internal.h"): (
        "2caf71102132104e14492534d8174f0e5873f0000d411c07c8e0c99e77a83385",
        "34fda9c9ca41bb45942eb3b4e651330fd2b397bfa80fd077811a86ad74ffeed1",
    ),
    ("PHILOSOPHIES.md", "sqlite3.c"): (
        "2a90d03b4f22054b6163b626a3214d87b29015f8ad7acd4c335ac7cd6efc961c",
    ),
    ("THIRD_PARTY_LICENSES.md", "miniz-3.0.2.zip"): (
        "311d4124a6d2a5d973a937b4b5f2598aa9358d52e100d9fc518fea7b412445ab",
        "583b67a24e9007378cb5a39dde6fb515369a5f86798b33be5bdaf214788248c9",
    ),
    ("apps/host/mdl/README.md", "robots.txt"): (
        "d5ff2637aa208a784cc116e766dd34d1b21a1df08726647f618dbe2259fcebd1",
        "fd54e89acf94b1c510036a5e47bfe0e834820ab78869bf5e1f53b3c2e27588e3",
    ),
    ("coprocessor/esp32c6/README.md", "wifi.env"): (
        "f75141d2b345f67b480e48cb15484247dc2359dbbe20aae28a05d577929e370b",
    ),
    ("docs/ARCHITECTURE.md", "crt0.o"): (
        "883484b3e1ba2990a65e62821f720868f377f0d749a2494c0124fa44b7a4a3b6",
        "989a280aa3d47196eaeb29ba04678f950c0eb417fbb79fe3fa4846ed99c103eb",
    ),
    ("docs/COVERAGE.md", "compile_commands.json"): (
        "9164e4797e1e2cf6ee410f257b49ec32bb0fb7acf89f5df4669e5be84c8cbc60",
    ),
    ("docs/COVERAGE.md", "summary.json"): (
        "85a783a432aee72a459669b7350679aaf89192f60b836604676898ae1c3efb8c",
    ),
    ("docs/COVERAGE.md", "summary.txt"): (
        "f7743a957bc25a7953b672f7b8bf4de3129a55f0954a507db7ee423a5854662e",
    ),
    ("docs/DOCS.md", "footer.html"): (
        "e5ff28637d3b499d8db299fb4417c78d486363c10c0ea503613f00dded8a0084",
    ),
    ("docs/HIL_SUITE.md", "dwf.h"): (
        "429e72c72cbf33e8b2eb17e1756485f6541f159b459688ba974415d9859d76f1",
    ),
    ("docs/IDE.md", "compile_commands.json"): (
        "abdd65fa156e20bf329e192103fb7eb420695ea91046790214f96f898931434a",
    ),
    ("docs/INFRASTRUCTURE.md", "k3s-runner-maintenance.sh"): (
        "fbb8cfd2fc7fa0c0961037ee1ce977e4a50c946dae52cc2038aa729c837face6",
    ),
    ("docs/RING_AND_WORLD.md", "my_driver.c"): (
        "f467c3e567486b0157a3a1aca74864b5db02a8749f78bddbe1ecbfd1689eacda",
    ),
    ("docs/ROADMAP.md", "ra8_xxx_regs.h"): (
        "bc10b0c18d4b000c52f44c14cf8c4b2cf8ff9a6ce005a68bd9c9b4bd56ff06fa",
    ),
    ("docs/ROOT_OF_TRUST.md", "history.json"): (
        "df7ec6d5427515f6b358ef674fe7e302fb6c6e3c077537624ac213a690302456",
    ),
    ("docs/SOUP/README.md", ".tar.xz"): (
        "4fb5b25547ce892e053cb081ce35f1f5fd75e7732a3fdf9e7139b57554298fcc",
    ),
    ("docs/SOUP/README.md", "miniz-3.0.2.zip"): (
        "3be044f943ab991e57bbbe81f4a65edc2012acc4afd01b14c75edb0cc73acbbd",
    ),
    ("docs/SOUP/esp-hosted-host.md", "esp_timer.h"): (
        "16e8a3528751960b19685b63f95d14a48e6ab19f52c395f294a975e6f075530a",
    ),
    ("docs/SOUP/esp-hosted-host.md", "esp_wifi_types.h"): (
        "16e8a3528751960b19685b63f95d14a48e6ab19f52c395f294a975e6f075530a",
    ),
    ("docs/SOUP/esp-hosted-host.md", "portmacro.h"): (
        "d5b3ca320e35ceb5af213c3de985e4cc621dcb7edb17dc3c7b089b58ace168b4",
    ),
    ("docs/SOUP/miniz.md", "miniz-3.0.2.zip"): (
        "c53637b11351d9ffa5abe88209834b3f61f96a9e86dce13f68416311bea9637e",
        "e86e59814206706f14b64ba5684599103176ed5c5a3640e0fbcd13a61b631caa",
        "f2d69043220ddca4131c093489b4c14a1316e56866a3b76d689db8876df2aaba",
    ),
    ("docs/SOUP/tf-psa-crypto.md", "generate_driver_wrappers.py"): (
        "8c6f5aa29ef64f51f237391f0daf8437afaf256a92417da46dd78f580c0a8cb9",
    ),
    ("docs/SOUP/tflite-micro.md", "*_test.cc"): (
        "84f4401aa0e1d289e61cea7d7e5ba1e130004ab15496847fa9818957e8c547a5",
    ),
    ("docs/SOUP/xz_embedded.md", ".tar.xz"): (
        "fc674ab41f256e3dafe16ad4073c4e09b6aafb89e73ef7bedce0b151d10549cc",
    ),
    ("docs/STACK_USAGE.md", "${RA8_APP_NAME}.elf"): (
        "f56770aa18eb092d118e8a620f71ebbda9340c89cd2a2a20274384b873c7f29e",
    ),
    ("docs/VENDOR_BLOBS.md", "hw_sce_*.c"): (
        "d0df5f74c37dd6fb765b34c3bdb1525d3b12b59703a78c98b3c155a9d60b83ee",
    ),
    ("docs/VENDOR_BLOBS.md", "r_rsip_*.c"): (
        "08f1a6e06ee2e148ba5ee7116e1011011c2d7a78b7139cfe2381175bd3307955",
    ),
    ("docs/VENDOR_BLOBS.md", "r_rsip_reg.h"): (
        "ab6d4011ca5765b444f7387fb90eb532cdcb936c2ec9bcde3492a441de2317dd",
    ),
    ("docs/VENDOR_BLOBS.md", "r_rsip_util.h"): (
        "61e9718e928b6b8ef4d0693ac6c0efbf7721a58f12386e5040bd3f1247571fb6",
    ),
    ("docs/formats/BINARY_FORMATS.md", "foo.bin"): (
        "44f15ea667e9cee7be7419586bcd8781e31757cfebd901f448f35e604bd9c7e8",
    ),
    ("docs/formats/JOF.md", "sample.jof"): (
        "63cb04d26b9564158d5e9169e58a41cfc6391d494a6ceba960406f9e319c3c58",
        "7d0b721b40d37569c9bbf89374e078b54e01a8036c65b928c52ba50e2adb85df",
        "f471bc2375814e128b93f7ebab5228b16ad588d36903bcaa8951cd4476c16c1e",
    ),
    ("docs/formats/JOF.md", "sample.png"): (
        "7d0b721b40d37569c9bbf89374e078b54e01a8036c65b928c52ba50e2adb85df",
        "b675c478bc3bdc214d42957a802fad4dc5b843f7ec3b4e01455ddbde9674a95b",
        "fa5b6cad5d44c3641ea1f08c311a8bcbb8ff9e6667da909c879e0afda5fb6fcc",
    ),
    ("docs/formats/ROT1.md", "body.bin"): (
        "6a27096b8247e6f5075e2e8459fff0c58ecdeaca405536caeb217327c3b6a0b9",
        "6a27096b8247e6f5075e2e8459fff0c58ecdeaca405536caeb217327c3b6a0b9",
        "cefc378262d698188515df915eed468c37349599e7a3bfb923aa4b893de7f9e8",
    ),
    ("docs/formats/ROT1.md", "signed.bin"): (
        "b8884236ed0c0ab543685ec3c13c2bb5048772a6a0a58ba3ba529748fb64d1ce",
    ),
    ("docs/qualification/HW_IN_LOOP_RUNNER.md", "config.sh"): (
        "f65ee40de147311f4900dba8579bf50fa96006095f8e0069c4d21d4267f46d66",
    ),
    ("docs/qualification/HW_IN_LOOP_RUNNER.md", "svc.sh"): (
        "556c565bd251c502ea76fb7d915ebd2e5e99c50fb8d6710a67f631c7332e63c2",
    ),
    ("docs/qualification/MISRA_DEVIATIONS.md", "misra.py"): (
        "63f20d0743870e3c91989336c37c668f679ff3cf72d910596bd15f799b003f3c",
        "7e6022a581c7e6db64483412fc2dab3ed2fdbe179a3c0b0d561f80e6f075707c",
        "89a9b6a12528c68b61874a6d1a52a71013ae92246a102ab85859fe74648c7345",
    ),
    ("docs/qualification/SDD.md", "_internal.h"): (
        "db14b03826b487de50fa02785aa4cad0fc396081d746bc914abad51bf514b2c7",
    ),
    ("docs/qualification/SRS.md", "_regs.h"): (
        "b55d9b2d96b21b0fd29c92e330537699f785fc01a8f6e1b2f8012046cb642d62",
    ),
    ("docs/qualification/SVCP.md", "mcdc.txt"): (
        "f769efea027818f81917bf2fef9f993d96a548ff85bba8c882064d938b1f90d6",
    ),
    ("docs/qualification/SVCP.md", "summary.txt"): (
        "3e6b72d9a536e5ec2bd4e3ae94f15e673361bd2eee71148208cef84c96d44bd7",
    ),
    ("docs/qualification/SVR.md", "r_usb_pdriver.c"): (
        "ff3a66bfe8bf027bb3c81c94b68095142e954df77903ae2ae2ff5790d758841e",
    ),
    ("docs/reference/ra8p1_vs_ra8d2.md", "R7KA8{P1,D2}KF_core0.h"): (
        "d984700bdee5ea78d5db166186850fdc22bfe1704fe0f2aa92d08f25833c9717",
    ),
    ("docs/reference/ra8p1_vs_ra8d2.md", "bsp_linker.c"): (
        "d66042eb9df30051ded10cf042539f9b74864eec10168a00a6968585388f1efc",
    ),
    ("docs/sbom/patches/README.md", "0001-description.patch"): (
        "ca0d5e6e847a06cfceb472533df0cc4026eddd493a98f00993827d5dd6ac8a0a",
    ),
    ("docs/sbom/patches/README.md", "0002-description.patch"): (
        "ca0d5e6e847a06cfceb472533df0cc4026eddd493a98f00993827d5dd6ac8a0a",
    ),
    ("examples/ek_ra8d2/hw_pending/ereader_zoom/README.md", "dst.h"): (
        "0f8fdbd61f30ee429390eb462edcdf079680a34738e33472e3442da545c997dd",
    ),
    ("examples/ek_ra8d2/hw_pending/i2c_peripheral_responder/README.md", "bsp_elc.h"): (
        "005183fe4fb539f8213bff29ac24a236862e8b818739605acd5a28e8d006640d",
    ),
    ("infra/network/PI_PROVISIONING.md", "cmdline.txt"): (
        "cb9037f30531a3de25e6711928b09c032d76c049419e194c9d4b8260d4e7c72b",
    ),
    ("infra/network/PI_PROVISIONING.md", "config.txt"): (
        "1fd50968f05c27859371f1f3eb9c32cdd648d9e20e06542c541eb10e17730b59",
        "25d9682c48bceb005649643c2f6ad9a662bc49c20af9ea8d19c8aa43f64db9d5",
        "62b444c19bfddd6a958d1507cf13cc7c338150e6cf2a34f5db23ea4fb2d2c9fa",
    ),
    ("infra/network/PI_PROVISIONING.md", "custom.toml"): (
        "6bed3284d29d8a632bcd915b0c490c299f87c9ca9be6535244999b353f50aea9",
        "b18b03a38981e9d6e99770e861b0cad7bd325e5089ac99827883db95ac64c2ac",
        "fe55427a039ee5c49204c72e37a09de5ee8bbfcd903e4b5c7582c44fc57fad8b",
    ),
    ("infra/network/PI_PROVISIONING.md", "userconf.txt"): (
        "610e9bfd35b731f3877431c2218f52a59a2a6eb717f9575fbe52145ea742a6ac",
        "71b05004f1e2a83c58de5431019be9b09413ca261a671ec61cd37ba9a1dc43c0",
        "ebd8f286df9340e424d154a58d7c1803158f1ef227d8dadc447c74aac86b178f",
    ),
    ("infra/network/README.md", "userconf.txt"): (
        "60c475f246ca346f4adbd24f980f314cb0fff073416707aea40607606435b486",
    ),
    ("scripts/emu/README.md", "eil.conf"): (
        "08c3c36c3dbe49e3a52fc345986238560cd81ba66c83f9ad540cffaa8a11a14c",
    ),
    ("scripts/secrets/README.md", "approle.env"): (
        "497d4653c87cac05a67b18d4adb9458dde7152c4690fa0fde79f5a95e2cbf824",
    ),
    ("scripts/secrets/README.md", "init.json"): (
        "6388a4c6a0a5069bf77c99e5200330936b08d046f5f3eb741f8a956e22ad39eb",
    ),
    ("scripts/secrets/README.md", "values.env"): (
        "497d4653c87cac05a67b18d4adb9458dde7152c4690fa0fde79f5a95e2cbf824",
    ),
    ("tests/fixtures/webp/README.md", "pattern.png"): (
        "831ccfeee9be6bda44897687af0c3ef471a4f4867089578a2c96956408697b93",
        "9f370e37f9651bc6e7cdb6da8fedfcaca63a1190e9fa35423bd89350ae4d44e6",
        "e88a5a6934d6641646e8c31ccd5fe99394a4b90184ec7d0cb1ed122a0107d093",
    ),
    ("tests/fixtures/webp/README.md", "tall.png"): (
        "3b7308e8cdcb6d13d5d824eb4acf0a9cc8c294456e08930ed068d86d671a7a7f",
        "b896607fcbf5c4f574da907196b6fdb32af33af263bbfd0c94818c6ceb73681f",
    ),
    ("tests/fixtures/webp/README.md", "wide.png"): (
        "2a4998203c1e8c307c33ecda24d3edee42286d550af044801ada385737762663",
        "9a2a452b55ce33c4f0f3bf893ca88a0fabf482c03b9d4114523cff4574f408c1",
    ),
    ("tests/golden/ereader_chrome/README.md", "ereader_ui.elf"): (
        "58ab9fdb7f029d4ff56f95f5000680b2486823cc8923eca8742f521314c29f37",
    ),
}
