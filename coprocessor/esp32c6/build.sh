#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# build.sh -- reproducibly build the ESP32-C6 wireless co-processor firmware.
#
# The C6 image is a pinned Espressif esp-hosted-mcu "network_adapter" plus the
# reviewed first-party ra8_mdl_service component. The checked-in patch exposes
# a bounded synchronous CustomRpc response hook; this script refuses to build
# if that patch no longer applies to the exact upstream pin.
#
# Three things are ASSERTED rather than assumed, because each can drift while
# the build still succeeds:
#   - the esp-idf release is exactly ESP_IDF_VERSION (not merely its series),
#   - sdkconfig.defaults still agrees with pins.env (check_c6_pin_config.py),
#   - the component set the registry resolved matches components-lock.txt.
#
# Usage:
#   ./coprocessor/esp32c6/build.sh
#
# Requires:
#   - exact esp-idf ESP_IDF_VERSION exported so idf.py is on PATH
#     (. "$IDF_PATH/export.sh"), on any adequately provisioned build host;
#   - network access for the initial clone and esp-idf component resolution.
# Building is hardware-free. Flashing and mixed-image qualification remain
# restricted to the Pi bench workflow.
#
# The pinned upstream base recipe was built, flashed, and booted on the bench
# with these pins before the media component was added. The component itself
# still requires an explicit mixed-image bench qualification.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=coprocessor/esp32c6/pins.env
source "${SCRIPT_DIR}/pins.env"

CLONE_DIR="${SCRIPT_DIR}/esp-hosted-mcu"
PERIPHERAL_DIR="${CLONE_DIR}/slave" # LEGACY-OK: esp-hosted-mcu upstream directory is named "slave"

# ---- 1. require idf.py and assert the pinned esp-idf major.minor ----
if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: idf.py not on PATH. Export esp-idf ${ESP_IDF_VERSION} first" >&2
  echo "       (get_idf, or . \"\${IDF_PATH}/export.sh\"). The dev box has no" >&2
  echo "       esp-idf -- build on the Pi bench host." >&2
  exit 1
fi

idf_version="$(idf.py --version 2>/dev/null || true)"

# Assert the EXACT pin, not the minor series. This test used to be `*v5.5.*`,
# which accepted every v5.5.x -- so ESP_IDF_VERSION was decorative here, and the
# c6_toolchain role's claim that this script "refuses to run under any other
# v5.5.x" was simply not true. The pin is derived from pins.env; there is no
# second copy of the version in this file.
#
# The comparison is against a whitespace-delimited TOKEN ("ESP-IDF v5.5.4"), so
# v5.5.41 and a v5.5.4-268-g<sha> development checkout are both rejected:
# neither is the release the bench proved end to end.
idf_pinned=0
if [[ -n "${idf_version}" ]]; then
  read -ra idf_words <<<"${idf_version}"
  for word in "${idf_words[@]}"; do
    if [[ "${word}" == "${ESP_IDF_VERSION}" ]]; then
      idf_pinned=1
      break
    fi
  done
fi
if ((idf_pinned == 0)); then
  echo "ERROR: esp-idf ${ESP_IDF_VERSION} required (pinned in coprocessor/esp32c6/pins.env)." >&2
  echo "       idf.py reports: ${idf_version:-unknown}" >&2
  echo "       Fix: check out ${ESP_IDF_VERSION} in \${IDF_PATH}, re-run its install.sh," >&2
  echo "       then re-export (get_idf, or . \"\${IDF_PATH}/export.sh\")." >&2
  echo "       Provisioning is declared in infra/ansible/roles/c6_toolchain/." >&2
  echo "       NOTE: v5.4.1 does NOT build esp-hosted-mcu -- the component-manager" >&2
  echo "       pull of tf-psa-crypto fails to compile p256-m under that release." >&2
  exit 1
fi
echo "==> idf.py: ${idf_version}"

# ---- 1b. assert sdkconfig.defaults still agrees with pins.env ----
# pins.env is the source of truth; sdkconfig.defaults restates every pin in the
# only syntax esp-idf reads. Two files holding one fact drift silently, and a
# drift here is invisible downstream: the build succeeds, the image flashes, and
# the SPI link just never comes up because the C6 drives a different pin than
# the RA8 does. The comparison lives in ONE place (the checker below, which CI
# also runs), never copied into this script.
if ! command -v python3 >/dev/null 2>&1; then
  echo "ERROR: python3 not on PATH; cannot verify the C6 pin config." >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/../../scripts/checks/check_c6_pin_config.py"

# ---- 2. fetch esp-hosted-mcu at the pinned commit ----
if [[ ! -d "${CLONE_DIR}/.git" ]]; then
  echo "==> cloning ${ESP_HOSTED_MCU_URL}"
  git clone "${ESP_HOSTED_MCU_URL}" "${CLONE_DIR}"
fi
echo "==> checking out ${ESP_HOSTED_MCU_SHORT} (${ESP_HOSTED_MCU_COMMIT})"
git -C "${CLONE_DIR}" fetch origin
git -C "${CLONE_DIR}" -c advice.detachedHead=false checkout --detach "${ESP_HOSTED_MCU_COMMIT}"
git -C "${CLONE_DIR}" reset --hard "${ESP_HOSTED_MCU_COMMIT}"
git -C "${CLONE_DIR}" submodule update --init --recursive

if [[ ! -d "${PERIPHERAL_DIR}" ]]; then
  echo "ERROR: ${PERIPHERAL_DIR} missing -- upstream layout changed at this commit" >&2
  exit 1
fi

# ---- 2b. apply the reviewed extension and stage the first-party component ----
PATCH_FILE="${SCRIPT_DIR}/patches/0001-custom-rpc-sync-response-hook.patch"
COMPONENT_DIR="${PERIPHERAL_DIR}/components/ra8_mdl_service"
if [[ ! -f "${PATCH_FILE}" ]]; then
  echo "ERROR: required CustomRpc patch is missing: ${PATCH_FILE}" >&2
  exit 1
fi
echo "==> applying checked-in CustomRpc response hook"
git -C "${CLONE_DIR}" apply --unidiff-zero --check "${PATCH_FILE}"
git -C "${CLONE_DIR}" apply --unidiff-zero "${PATCH_FILE}"

echo "==> staging first-party ra8_mdl_service component"
rm -rf "${COMPONENT_DIR}"
mkdir -p "${COMPONENT_DIR}/include" "${COMPONENT_DIR}/src"
cp "${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt" "${COMPONENT_DIR}/CMakeLists.txt"
cp "${SCRIPT_DIR}/../../port/esp32_c6/src/mdl_service.c" "${COMPONENT_DIR}/src/mdl_service.c"
cp "${SCRIPT_DIR}/../../port/esp32_c6/src/esp_idf_mdl_compat_internal.h" \
  "${COMPONENT_DIR}/src/esp_idf_mdl_compat_internal.h"
cp "${SCRIPT_DIR}/../../port/esp32_c6/inc/ra8_mdl_service.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_service.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/src/ra8_c6link_mdl_service.c" \
  "${COMPONENT_DIR}/src/ra8_c6link_mdl_service.c"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/src/ra8_c6link_mdl_service_internal.h" \
  "${COMPONENT_DIR}/src/ra8_c6link_mdl_service_internal.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/src/ra8_media_download.pb-c.c" \
  "${COMPONENT_DIR}/src/ra8_media_download.pb-c.c"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_c6link_mdl_msg.h" \
  "${COMPONENT_DIR}/include/ra8_c6link_mdl_msg.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_protocol.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_protocol.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_http.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_http.h"
cp "${SCRIPT_DIR}/../../libs/ra8_mdl/inc/ra8_mdl_format.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_format.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_media_download.pb-c.h" \
  "${COMPONENT_DIR}/include/ra8_media_download.pb-c.h"
cp "${SCRIPT_DIR}/../../libs/ra8_core/inc/ra8_err.h" "${COMPONENT_DIR}/include/ra8_err.h"
cp "${SCRIPT_DIR}/../../libs/ra8_core/inc/ra8_attributes.h" \
  "${COMPONENT_DIR}/include/ra8_attributes.h"

# ---- 3. drop in the proven sdkconfig.defaults ----
echo "==> installing sdkconfig.defaults"
cp "${SCRIPT_DIR}/sdkconfig.defaults" "${PERIPHERAL_DIR}/sdkconfig.defaults"

# ---- 4. clean, set target, build ----
echo "==> cleaning previous build state"
rm -rf \
  "${PERIPHERAL_DIR}/build" \
  "${PERIPHERAL_DIR}/sdkconfig" \
  "${PERIPHERAL_DIR}/dependencies.lock" \
  "${PERIPHERAL_DIR}/managed_components"

echo "==> idf.py set-target ${ESP_TARGET} && idf.py build"
(
  cd "${PERIPHERAL_DIR}"
  idf.py set-target "${ESP_TARGET}"
  idf.py build
)

if ! command -v riscv32-esp-elf-nm >/dev/null 2>&1; then
  echo "ERROR: riscv32-esp-elf-nm missing; cannot assert media service linkage" >&2
  exit 1
fi
C6_ELF="${PERIPHERAL_DIR}/build/network_adapter.elf"
C6_SYMBOLS="$(riscv32-esp-elf-nm --defined-only "${C6_ELF}")"
if ! grep -Eq '^[[:xdigit:]]+[[:space:]]+T[[:space:]]+esp_hosted_custom_rpc_sync_handler$' \
  <<<"${C6_SYMBOLS}"; then
  echo "ERROR: built C6 image does not contain the strong media CustomRpc handler" >&2
  echo "       (a weak W fallback does not satisfy this check)" >&2
  exit 1
fi
if ! grep -Eq '^[[:xdigit:]]+[[:space:]]+T[[:space:]]+ra8_mdl_service_component_abi$' \
  <<<"${C6_SYMBOLS}"; then
  echo "ERROR: built C6 image lacks the ra8_mdl_service component ABI marker" >&2
  exit 1
fi
echo "==> verified strong ra8 media handler and component ABI in network_adapter.elf"

# ---- 5. verify the component set that actually resolved ----
# Step 4 deletes dependencies.lock, so the esp-idf component manager re-resolves
# from the registry on every run. That makes this build RECIPE-reproducible but
# not BIT-reproducible: the registry can hand back a different component version
# tomorrow, from an unchanged recipe, and nothing would say so.
#
# Committing the generated lock is not available to us. It records a
# HOST-SPECIFIC ABSOLUTE PATH for the local cmd_system component
# (${IDF_PATH}/examples/system/console/advanced/components/cmd_system), so a
# committed copy would be wrong on every machine except the one that produced
# it. What IS portable is the resolved SET -- kinds, names, versions and the
# registry component hashes -- and that is what components-lock.txt records,
# taken from the bench build whose network_adapter.bin is the flashed, proven
# image. The path is deliberately not compared; the local component is checked
# on its kind and version only.
COMPONENT_RECORD="${SCRIPT_DIR}/components-lock.txt"
GENERATED_LOCK="${PERIPHERAL_DIR}/dependencies.lock"

# Normalise dependencies.lock (YAML) to "kind<TAB>name<TAB>version<TAB>hash".
# INDENTATION is the discriminator and the reason this is not a grep: a
# component is a 2-space key under "dependencies:", its own version and
# component_hash sit at 4 spaces, and the version CONSTRAINTS of its
# requirements sit at 6 -- so a plain search for "version:" collects both and
# compares the wrong number.
lock_to_records() {
  awk '
    function emit() {
      if (name == "") { return }
      printf "%s\t%s\t%s\t%s\n", (kind == "" ? "?" : kind), name,
                                 (ver  == "" ? "?" : ver),
                                 (hash == "" ? "-" : hash)
    }
    /^dependencies:[[:space:]]*$/ { in_deps = 1; next }
    /^[A-Za-z_]/                  { emit(); name = ""; in_deps = 0; next }
    in_deps == 0                  { next }
    /^  [^ ].*:[[:space:]]*$/ {
      emit()
      name = $1; sub(/:$/, "", name)
      ver = ""; hash = ""; kind = ""
      next
    }
    /^    component_hash:[[:space:]]/ { hash = $2; next }
    # Keep only the characters a version can legitimately contain, which drops
    # the quoting the emitter puts around a bare wildcard version.
    /^    version:[[:space:]]/ { ver = $2; gsub(/[^A-Za-z0-9._*+-]/, "", ver); next }
    /^      type:[[:space:]]/  { kind = $2; next }
    END { emit() }
  ' "$1" | LC_ALL=C sort
}

# The committed record: four whitespace-separated columns, '#' comments.
record_to_records() {
  awk '!/^[[:space:]]*#/ && NF >= 4 { printf "%s\t%s\t%s\t%s\n", $1, $2, $3, $4 }' "$1" |
    LC_ALL=C sort
}

if [[ ! -f "${COMPONENT_RECORD}" ]]; then
  echo "ERROR: ${COMPONENT_RECORD} missing -- the committed component record is gone," >&2
  echo "       so nothing can be said about which component versions were used." >&2
  exit 1
fi
if [[ ! -f "${GENERATED_LOCK}" ]]; then
  echo "ERROR: the build produced no ${GENERATED_LOCK}." >&2
  echo "       Either the component manager did not run or upstream changed shape;" >&2
  echo "       either way the resolved component set cannot be verified." >&2
  exit 1
fi

echo "==> verifying resolved components against components-lock.txt"

# Each normalisation is its own statement so it runs under a normal errexit
# context: masking a first-party function's status with `||` swallows a failure
# part-way through its body and leaves the caller reading only the last
# command's status. Only the diff -- an external command whose exit 1 is the
# expected "they differ" answer -- is allowed to fail here.
recorded_components="$(record_to_records "${COMPONENT_RECORD}")"
resolved_components="$(lock_to_records "${GENERATED_LOCK}")"

if [[ "${recorded_components}" != "${resolved_components}" ]]; then
  echo "ERROR: the esp-idf component manager resolved a DIFFERENT component set" >&2
  echo "       than the one this firmware was proven with. '-' lines are what" >&2
  echo "       coprocessor/esp32c6/components-lock.txt records; '+' lines are what" >&2
  echo "       this build actually resolved." >&2
  diff -u --label committed --label resolved \
    <(printf '%s\n' "${recorded_components}") \
    <(printf '%s\n' "${resolved_components}") >&2 || true
  echo >&2
  echo "       This is not a warning to click past: the recipe is unchanged, so a" >&2
  echo "       difference here means the registry handed back something else. Either" >&2
  echo "       re-pin deliberately (update components-lock.txt AND reflash and" >&2
  echo "       re-qualify the C6), or find out why the resolution moved." >&2
  exit 1
fi

# ---- 6. print artifact paths ----
echo "build.sh: OK -- flash these with ./coprocessor/esp32c6/flash.sh:"
for artifact in \
  "bootloader/bootloader.bin" \
  "partition_table/partition-table.bin" \
  "ota_data_initial.bin" \
  "network_adapter.bin"; do
  echo "  ${PERIPHERAL_DIR}/build/${artifact}"
done
