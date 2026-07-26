#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly ROOT_DIR

fail() {
  echo "error: $*" >&2
  exit 1
}

command -v rg >/dev/null 2>&1 || fail "ripgrep (rg) is required"

# Keep the rejected legacy identifiers out of the repository itself while
# still assembling exact case-insensitive validation expressions at runtime.
readonly LEGACY_VENDOR_PATTERN='lime''light'
readonly LEGACY_VENDOR_ID_PATTERN='LIME''LIGHT_EC'
readonly LEGACY_MAIN_DEVICE_PATTERN='ethercat[_-]main''device'
readonly LEGACY_WEB_UI_PATTERN='ethercat[_-]dash''board'
readonly LEGACY_SHORT_UI_PATTERN='ec_dash''board'
readonly LEGACY_EC_MAIN_DEVICE_PATTERN='ec_main''device'

mapfile -t FIRST_PARTY_FILES < <(
  rg --files "${ROOT_DIR}" \
    -g '!third_party/SOEM/**' \
    -g '!build/**' \
    -g '!dist/**' \
    -g '!configuration/node_modules/**' \
    -g '!configuration/dist/**' \
    -g '!docs/**' \
    -g '!scripts/validate_branding.sh' \
    -g '!.git/**'
)

[[ "${#FIRST_PARTY_FILES[@]}" -gt 0 ]] ||
  fail "no first-party files found beneath ${ROOT_DIR}"

branding_failure=0
for path in "${FIRST_PARTY_FILES[@]}"; do
  relative_path="${path#"${ROOT_DIR}/"}"
  normalized_path="${relative_path//\\//}"
  if grep -Eqi "${LEGACY_VENDOR_PATTERN}" <<<"${normalized_path}"; then
    echo "non-canonical path: ${normalized_path}" >&2
    branding_failure=1
  fi
done

if rg -n -i \
  -e "${LEGACY_VENDOR_PATTERN}" \
  -e "${LEGACY_VENDOR_ID_PATTERN}" \
  -e "${LEGACY_MAIN_DEVICE_PATTERN}" \
  -e "${LEGACY_WEB_UI_PATTERN}" \
  -e "${LEGACY_SHORT_UI_PATTERN}" \
  -e "${LEGACY_EC_MAIN_DEVICE_PATTERN}" \
  "${FIRST_PARTY_FILES[@]}"; then
  branding_failure=1
fi

if rg -n \
  -e '/etc/ethercat(/|$)' \
  -e '/var/log/ethercat(/|$)' \
  -e '/var/run/ec-systemcore\.sock' \
  -e '/run/ec-systemcore\.sock' \
  "${FIRST_PARTY_FILES[@]}"; then
  branding_failure=1
fi

[[ "${branding_failure}" -eq 0 ]] ||
  fail "first-party names and installed paths must use the ec-systemcore contract"

echo "Validated canonical ec-systemcore branding and installed paths."
