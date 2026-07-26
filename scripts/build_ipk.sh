#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly ROOT_DIR
readonly PKG_NAME="ec-systemcore"
readonly PKG_ARCH="arm64"

fail() {
  echo "error: $*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "'$1' is required"
}

canonical_path() {
  python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

require_within() {
  candidate="$(canonical_path "$1")"
  parent="$(canonical_path "$2")"
  allow_parent="${3:-false}"
  if [[ "${candidate}" == "${parent}" && "${allow_parent}" == "true" ]]; then
    printf '%s\n' "${candidate}"
    return
  fi
  [[ "${candidate}" == "${parent}/"* ]] ||
    fail "$1 resolves outside the allowed directory $2"
  printf '%s\n' "${candidate}"
}

require_command python3
require_command cmake
require_command opkg-build
require_command aarch64-linux-gnu-readelf
require_command qemu-aarch64
require_command sha256sum
require_command jar

readonly VERSION_FILE="${ROOT_DIR}/VERSION"
[[ -f "${VERSION_FILE}" ]] || fail "VERSION is missing"
IFS= read -r BASE_VERSION < "${VERSION_FILE}"
[[ "${BASE_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
  fail "VERSION must be a stable semantic version without a leading 'v'"

BUILD_ROOT="$(require_within "${ROOT_DIR}/build" "${ROOT_DIR}")"
readonly BUILD_ROOT
DIST_ROOT="$(require_within "${ROOT_DIR}/dist" "${ROOT_DIR}")"
readonly DIST_ROOT
mkdir -p "${BUILD_ROOT}" "${DIST_ROOT}"

BUILD_DIR="$(require_within \
  "${BUILD_DIR:-${ROOT_DIR}/build/linux-arm64}" \
  "${BUILD_ROOT}")"
readonly BUILD_DIR
PACKAGE_WORK_ROOT="$(require_within \
  "${ROOT_DIR}/build/package" "${BUILD_ROOT}")"
readonly PACKAGE_WORK_ROOT
mkdir -p "${PACKAGE_WORK_ROOT}"
STAGE_DIR="$(require_within \
  "${STAGE_DIR:-${PACKAGE_WORK_ROOT}/stage}" \
  "${PACKAGE_WORK_ROOT}")"
readonly STAGE_DIR
DIST_DIR="$(require_within \
  "${DIST_DIR:-${DIST_ROOT}}" \
  "${DIST_ROOT}" true)"
readonly DIST_DIR

readonly PACKAGE_FLAVOR="${PACKAGE_FLAVOR:-development}"
case "${PACKAGE_FLAVOR}" in
  release)
    PACKAGE_VERSION="${BASE_VERSION}"
    ;;
  development)
    BUILD_ID="${EC_SYSTEMCORE_BUILD_ID:-$(git -C "${ROOT_DIR}" rev-parse --short=12 HEAD)}"
    [[ "${BUILD_ID}" =~ ^[0-9A-Za-z]+([.-][0-9A-Za-z]+)*$ ]] ||
      fail "EC_SYSTEMCORE_BUILD_ID contains characters invalid in a semantic prerelease"
    PACKAGE_VERSION="${BASE_VERSION}-development.${BUILD_ID}"
    ;;
  *)
    fail "PACKAGE_FLAVOR must be 'release' or 'development'"
    ;;
esac
readonly PACKAGE_VERSION

DEFAULT_IPK_NAME="${PKG_NAME}-${PACKAGE_VERSION}-${PKG_ARCH}.ipk"
readonly FINAL_IPK_NAME="${FINAL_IPK_NAME:-${DEFAULT_IPK_NAME}}"
[[ "${FINAL_IPK_NAME}" == "$(basename -- "${FINAL_IPK_NAME}")" ]] ||
  fail "FINAL_IPK_NAME must be a basename"
[[ "${FINAL_IPK_NAME}" =~ ^ec-systemcore-[0-9A-Za-z.+-]+-arm64\.ipk$ ]] ||
  fail "FINAL_IPK_NAME must use the canonical ec-systemcore ARM64 name"
FINAL_IPK_PATH="$(require_within \
  "${DIST_DIR}/${FINAL_IPK_NAME}" "${DIST_DIR}")"
readonly FINAL_IPK_PATH
[[ ! -e "${FINAL_IPK_PATH}" ]] ||
  fail "refusing to overwrite existing output ${FINAL_IPK_PATH}"

readonly BUILD_METADATA="${BUILD_METADATA:-${BUILD_DIR}/ec-systemcore-build-metadata.json}"
[[ -f "${BUILD_METADATA}" ]] || fail "build metadata is missing: ${BUILD_METADATA}"
readarray -t METADATA < <(
  python3 - "${BUILD_METADATA}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
print(data.get("name", ""))
print(data.get("version", ""))
print(data.get("target_system", ""))
print(data.get("target_processor", ""))
print(data.get("build_type", ""))
print(data.get("cxx_standard", ""))
print(data.get("cxx_compiler_id", ""))
print(data.get("warnings_as_errors", ""))
print(data.get("target_glibc_max", ""))
print(data.get("java_validation_runtime", ""))
print(data.get("java_bytecode_release", ""))
print(data.get("bun_runtime_version", ""))
print(data.get("bun_runtime_asset", ""))
print(data.get("bun_runtime_asset_sha256", ""))
print(data.get("bun_runtime_sha256", ""))
PY
)
[[ "${METADATA[0]}" == "${PKG_NAME}" ]] ||
  fail "build metadata has the wrong project name"
[[ "${METADATA[1]}" == "${BASE_VERSION}" ]] ||
  fail "build version ${METADATA[1]} does not match VERSION ${BASE_VERSION}"
[[ "${METADATA[2]}" == "Linux" ]] ||
  fail "package build must target Linux"
[[ "${METADATA[3]}" =~ ^(aarch64|arm64)$ ]] ||
  fail "package build must target AArch64"
[[ "${METADATA[4]}" == "Release" ]] ||
  fail "package build must use the Release configuration"
[[ "${METADATA[5]}" == "23" ]] ||
  fail "package build must use C++23"
[[ "${METADATA[6]}" == "GNU" ]] ||
  fail "package build must use the pinned GNU ARM64 compiler"
[[ "${METADATA[7]}" == "ON" ]] ||
  fail "package build must reject first-party compiler warnings"
[[ "${METADATA[8]}" == "2.36" ]] ||
  fail "package build has the wrong target glibc compatibility ceiling"
[[ "${METADATA[9]}" == "25.0.3+9" && "${METADATA[10]}" == "17" ]] ||
  fail "package build has the wrong Java validation/bytecode contract"
[[ "${METADATA[11]}" == "$(< "${ROOT_DIR}/.bun-version")" ]] ||
  fail "build metadata has the wrong Bun version"
[[ "${METADATA[12]}" == "bun-linux-aarch64.zip" ]] ||
  fail "build metadata has the wrong Bun release asset"
EXPECTED_BUN_ZIP_SHA256="$(< "${ROOT_DIR}/.bun-arm64-zip.sha256")"
readonly EXPECTED_BUN_ZIP_SHA256
[[ "${METADATA[13]}" == "${EXPECTED_BUN_ZIP_SHA256}" ]] ||
  fail "build metadata has the wrong Bun release-archive digest"
[[ "${METADATA[14]}" =~ ^[0-9a-f]{64}$ ]] ||
  fail "build metadata has an invalid Bun executable digest"

rm -rf -- "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/CONTROL" "${DIST_DIR}"

DESTDIR="${STAGE_DIR}" cmake --install "${BUILD_DIR}" --prefix /usr --strip

readonly DAEMON_BIN="${STAGE_DIR}/usr/bin/ec-systemcore-daemon"
readonly CONFIGURATION_BIN="${STAGE_DIR}/usr/lib/ec-systemcore/configuration/server.js"
readonly BUN_RUNTIME="${STAGE_DIR}/usr/lib/ec-systemcore/runtime/bun"
readonly CONFIG_FILE="${STAGE_DIR}/etc/ec-systemcore/ec-systemcore.json"
readonly SYSTEMD_DIR="${STAGE_DIR}/usr/lib/systemd/system"
readonly JAVA_CLIENT="${STAGE_DIR}/usr/share/java/ec-systemcore-client.jar"

[[ -f "${DAEMON_BIN}" && ! -L "${DAEMON_BIN}" ]] ||
  fail "CMake install did not produce ${DAEMON_BIN}"
[[ -f "${CONFIGURATION_BIN}" && ! -L "${CONFIGURATION_BIN}" ]] ||
  fail "CMake install did not produce ${CONFIGURATION_BIN}"
[[ -x "${BUN_RUNTIME}" && ! -L "${BUN_RUNTIME}" ]] ||
  fail "CMake install did not produce the bundled ARM64 Bun runtime"
[[ -f "${JAVA_CLIENT}" && ! -L "${JAVA_CLIENT}" ]] ||
  fail "CMake install did not produce the Java robot-code client"
[[ -f "${STAGE_DIR}/usr/lib/libec-systemcore-client.a" ]] ||
  fail "CMake install did not produce the C++ robot-code client"
[[ -f "${STAGE_DIR}/usr/include/ec_systemcore/client.hpp" ]] ||
  fail "CMake install did not produce the C++ client header"
[[ -f "${STAGE_DIR}/usr/lib/cmake/ec-systemcore-client/ec-systemcore-clientConfig.cmake" ]] ||
  fail "CMake install did not produce the C++ CMake package"
jar tf "${JAVA_CLIENT}" | grep -qx 'ec_systemcore/SystemCoreClient.class' ||
  fail "installed Java JAR is missing SystemCoreClient"
readonly BUN_DIGEST_FILE="${STAGE_DIR}/usr/share/ec-systemcore/bun-runtime.sha256"
readonly BUN_PROVENANCE_FILE="${STAGE_DIR}/usr/share/ec-systemcore/bun-runtime-provenance.json"
[[ -f "${BUN_DIGEST_FILE}" && ! -L "${BUN_DIGEST_FILE}" ]] ||
  fail "installed Bun executable digest is missing"
[[ -f "${BUN_PROVENANCE_FILE}" && ! -L "${BUN_PROVENANCE_FILE}" ]] ||
  fail "installed Bun archive provenance is missing"
EXPECTED_BUN_SHA256="$(< "${BUN_DIGEST_FILE}")"
readonly EXPECTED_BUN_SHA256
[[ "${EXPECTED_BUN_SHA256}" == "${METADATA[14]}" ]] ||
  fail "installed Bun digest does not match build metadata"
readarray -t BUN_PROVENANCE < <(
  python3 - "${BUN_PROVENANCE_FILE}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
print(data.get("version", ""))
print(data.get("asset", ""))
print(data.get("archive_sha256", ""))
print(data.get("executable_sha256", ""))
PY
)
[[ "${BUN_PROVENANCE[0]}" == "$(< "${ROOT_DIR}/.bun-version")" &&
   "${BUN_PROVENANCE[1]}" == "bun-linux-aarch64.zip" &&
   "${BUN_PROVENANCE[2]}" == "${EXPECTED_BUN_ZIP_SHA256}" &&
   "${BUN_PROVENANCE[3]}" == "${EXPECTED_BUN_SHA256}" ]] ||
  fail "installed Bun provenance does not tie the official archive to its executable"
[[ "$(sha256sum "${BUN_RUNTIME}" | awk '{ print $1 }')" == "${EXPECTED_BUN_SHA256}" ]] ||
  fail "installed Bun runtime does not match its pinned release digest"
grep -Eq 'Machine:[[:space:]]+AArch64$' <(
  aarch64-linux-gnu-readelf -W -h "${BUN_RUNTIME}"
) || fail "installed Bun runtime is not AArch64"
[[ "$(qemu-aarch64 -L /usr/aarch64-linux-gnu \
      "${BUN_RUNTIME}" --version)" == "$(< "${ROOT_DIR}/.bun-version")" ]] ||
  fail "installed Bun runtime cannot execute as the pinned version"
[[ -f "${CONFIG_FILE}" && ! -L "${CONFIG_FILE}" ]] ||
  fail "CMake install did not produce ${CONFIG_FILE}"
[[ -f "${SYSTEMD_DIR}/ec-systemcore.service" ]] ||
  fail "daemon systemd unit is missing"
[[ -f "${SYSTEMD_DIR}/ec-systemcore-configuration.service" ]] ||
  fail "configuration systemd unit is missing"

bash "${ROOT_DIR}/scripts/validate_elf.sh" "${DAEMON_BIN}"

install -m 0644 "${ROOT_DIR}/CONTROL/control" "${STAGE_DIR}/CONTROL/control"
sed -i \
  -e "s/@EC_SYSTEMCORE_VERSION@/${PACKAGE_VERSION}/g" \
  "${STAGE_DIR}/CONTROL/control"
grep -qx "Package: ${PKG_NAME}" "${STAGE_DIR}/CONTROL/control" ||
  fail "CONTROL/control has the wrong package name"
grep -qx "Version: ${PACKAGE_VERSION}" "${STAGE_DIR}/CONTROL/control" ||
  fail "CONTROL/control version substitution failed"
grep -qx "Architecture: ${PKG_ARCH}" "${STAGE_DIR}/CONTROL/control" ||
  fail "CONTROL/control has the wrong architecture"

for script_name in postinst prerm postrm; do
  install -m 0755 "${ROOT_DIR}/CONTROL/${script_name}" \
    "${STAGE_DIR}/CONTROL/${script_name}"
done
install -m 0644 "${ROOT_DIR}/CONTROL/conffiles" \
  "${STAGE_DIR}/CONTROL/conffiles"

SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${ROOT_DIR}" log -1 --format=%ct)}"
[[ "${SOURCE_DATE_EPOCH}" =~ ^[0-9]+$ ]] ||
  fail "SOURCE_DATE_EPOCH must be an integer Unix timestamp"
export SOURCE_DATE_EPOCH
find "${STAGE_DIR}" -exec touch -h -d "@${SOURCE_DATE_EPOCH}" {} +

readonly GENERATED_IPK="${DIST_DIR}/${PKG_NAME}_${PACKAGE_VERSION}_${PKG_ARCH}.ipk"
[[ ! -e "${GENERATED_IPK}" ]] ||
  fail "refusing to overwrite opkg output ${GENERATED_IPK}"
opkg-build "${STAGE_DIR}" "${DIST_DIR}"
[[ -f "${GENERATED_IPK}" ]] ||
  fail "opkg-build did not create ${GENERATED_IPK}"
mv -- "${GENERATED_IPK}" "${FINAL_IPK_PATH}"

echo "Created ${FINAL_IPK_PATH}"
echo "Staged package root retained at ${STAGE_DIR} for validation and SBOM generation."
