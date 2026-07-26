#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly ROOT_DIR
readonly STAGE_DIR="${1:-${ROOT_DIR}/build/package/stage}"

fail() {
  echo "error: $*" >&2
  exit 1
}

[[ -d "${STAGE_DIR}" ]] || fail "package stage is missing: ${STAGE_DIR}"
STAGE_REAL="$(cd "${STAGE_DIR}" && pwd -P)"
readonly STAGE_REAL
[[ "${STAGE_REAL}" != "/" && "${STAGE_REAL}" != "${ROOT_DIR}" ]] ||
  fail "refusing to test an unsafe package root"

assert_file() {
  [[ -f "${STAGE_REAL}$1" && ! -L "${STAGE_REAL}$1" ]] ||
    fail "required regular file is missing: $1"
}

assert_file /usr/bin/ec-systemcore-daemon
assert_file /usr/lib/ec-systemcore/configuration/server.js
assert_file /usr/lib/ec-systemcore/runtime/bun
assert_file /usr/lib/systemd/system/ec-systemcore.service
assert_file /usr/lib/systemd/system/ec-systemcore-configuration.service
assert_file /etc/ec-systemcore/ec-systemcore.json
assert_file /usr/share/ec-systemcore/ec-systemcore-build-metadata.json
assert_file /usr/share/ec-systemcore/bun-runtime.sha256
assert_file /usr/share/ec-systemcore/bun-runtime-provenance.json
assert_file /usr/share/java/ec-systemcore-client.jar
assert_file /usr/lib/libec-systemcore-client.a
assert_file /usr/include/ec_systemcore/client.hpp
assert_file /usr/lib/cmake/ec-systemcore-client/ec-systemcore-clientConfig.cmake
assert_file /usr/share/doc/ec-systemcore/client-api.md
assert_file /usr/share/doc/ec-systemcore/BUN-LICENSE.md
assert_file /usr/share/doc/ec-systemcore/THIRD_PARTY.md
assert_file /CONTROL/control
assert_file /CONTROL/postinst
assert_file /CONTROL/prerm
assert_file /CONTROL/postrm
assert_file /CONTROL/conffiles

[[ ! -e "${STAGE_REAL}/usr/etc/ec-systemcore" ]] ||
  fail "configuration was incorrectly installed below /usr/etc"
grep -q '"controller_group":[[:space:]]*"ec-systemcore-controller"' \
  "${STAGE_REAL}/etc/ec-systemcore/ec-systemcore.json" ||
  fail "safe default controller group is missing"
grep -q 'ensure_group ec-systemcore-observer' \
  "${STAGE_REAL}/CONTROL/postinst" ||
  fail "observer group is not created at installation"
grep -q 'ensure_group ec-systemcore-controller' \
  "${STAGE_REAL}/CONTROL/postinst" ||
  fail "controller group is not created at installation"
grep -q 'repair_service_user_groups' \
  "${STAGE_REAL}/CONTROL/postinst" ||
  fail "upgrades do not repair existing service-account groups"
if grep -Eq 'ec-systemcore-configuration.*ec-systemcore-controller' \
  "${STAGE_REAL}/usr/lib/systemd/system/ec-systemcore-configuration.service"; then
  fail "configuration service received controller authorization"
fi

grep -qx 'Package: ec-systemcore' "${STAGE_REAL}/CONTROL/control" ||
  fail "package name is not canonical"
grep -qx 'Architecture: arm64' "${STAGE_REAL}/CONTROL/control" ||
  fail "package architecture is not arm64"
grep -qx \
  'License: GPL-3.0-only AND LicenseRef-Bun-1.3.14-Binary' \
  "${STAGE_REAL}/CONTROL/control" ||
  fail "package metadata does not disclose the bundled runtime license"
grep -Eq '^Version: [0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$' \
  "${STAGE_REAL}/CONTROL/control" ||
  fail "package version is not valid"
grep -Eq '^Depends: .*\bsystemd\b' "${STAGE_REAL}/CONTROL/control" ||
  fail "systemd dependency is missing"
if grep -Eq '^Depends: .*\bbun\b' "${STAGE_REAL}/CONTROL/control"; then
  fail "package must use its verified bundled Bun runtime"
fi

readonly BUN_RUNTIME="${STAGE_REAL}/usr/lib/ec-systemcore/runtime/bun"
[[ "$(stat -c '%a' "${BUN_RUNTIME}")" == "755" ]] ||
  fail "bundled Bun runtime mode must be 0755"
EXPECTED_BUN_SHA256="$(
  tr -d '\r\n' < "${STAGE_REAL}/usr/share/ec-systemcore/bun-runtime.sha256"
)"
readonly EXPECTED_BUN_SHA256
[[ "${EXPECTED_BUN_SHA256}" =~ ^[0-9a-f]{64}$ ]] ||
  fail "bundled Bun executable digest is invalid"
[[ "$(jq -r '.bun_runtime_sha256' \
      "${STAGE_REAL}/usr/share/ec-systemcore/ec-systemcore-build-metadata.json")" == "${EXPECTED_BUN_SHA256}" ]] ||
  fail "bundled Bun digest does not match build metadata"
[[ "$(jq -r '.bun_runtime_asset_sha256' \
      "${STAGE_REAL}/usr/share/ec-systemcore/ec-systemcore-build-metadata.json")" == "$(< "${ROOT_DIR}/.bun-arm64-zip.sha256")" ]] ||
  fail "Bun release-archive digest does not match build metadata"
readonly BUN_PROVENANCE="${STAGE_REAL}/usr/share/ec-systemcore/bun-runtime-provenance.json"
[[ "$(jq -r '.version' "${BUN_PROVENANCE}")" == "$(< "${ROOT_DIR}/.bun-version")" &&
   "$(jq -r '.asset' "${BUN_PROVENANCE}")" == "bun-linux-aarch64.zip" &&
   "$(jq -r '.archive_sha256' "${BUN_PROVENANCE}")" == "$(< "${ROOT_DIR}/.bun-arm64-zip.sha256")" &&
   "$(jq -r '.executable_sha256' "${BUN_PROVENANCE}")" == "${EXPECTED_BUN_SHA256}" ]] ||
  fail "Bun provenance does not tie the official archive to the installed runtime"
[[ "$(sha256sum "${BUN_RUNTIME}" | awk '{ print $1 }')" == "${EXPECTED_BUN_SHA256}" ]] ||
  fail "bundled Bun runtime does not match its pinned release digest"
grep -Eq 'Machine:[[:space:]]+AArch64$' <(
  aarch64-linux-gnu-readelf -W -h "${BUN_RUNTIME}"
) || fail "bundled Bun runtime is not AArch64"
[[ "$(qemu-aarch64 -L /usr/aarch64-linux-gnu \
      "${BUN_RUNTIME}" --version)" == "$(< "${ROOT_DIR}/.bun-version")" ]] ||
  fail "bundled Bun runtime cannot execute as the pinned version"
jar tf "${STAGE_REAL}/usr/share/java/ec-systemcore-client.jar" |
  grep -qx 'ec_systemcore/SystemCoreClient.class' ||
  fail "Java client JAR is missing SystemCoreClient"

for script_name in postinst prerm postrm; do
  [[ "$(stat -c '%a' "${STAGE_REAL}/CONTROL/${script_name}")" == "755" ]] ||
    fail "${script_name} mode must be 0755"
  sh -n "${STAGE_REAL}/CONTROL/${script_name}"
done
[[ "$(stat -c '%a' "${STAGE_REAL}/CONTROL/control")" == "644" ]] ||
  fail "CONTROL/control mode must be 0644"

if find "${STAGE_REAL}" -xdev -type f -perm /022 -print -quit |
    grep -q .; then
  fail "package contains a group- or world-writable regular file"
fi
if find "${STAGE_REAL}" -xdev -path '*/var/lib/*' -print -quit |
    grep -q .; then
  fail "package payload must not pre-create mutable runtime state"
fi
if grep -ERn -i \
  '^[[:space:]]*User=root$|^[[:space:]]*(AmbientCapabilities|CapabilityBoundingSet)=.*CAP_NET_ADMIN|^[[:space:]]*Environment=.*EC_SYSTEMCORE_(AUTH|TLS)=' \
  "${STAGE_REAL}/CONTROL" "${STAGE_REAL}/usr/lib/systemd/system"; then
  fail "package contains an unsafe privilege or authentication default"
fi

bash "${ROOT_DIR}/scripts/validate_elf.sh" \
  "${STAGE_REAL}/usr/bin/ec-systemcore-daemon"
bash "${ROOT_DIR}/scripts/test_systemd.sh" "${STAGE_REAL}/usr"

TEST_ROOT="$(mktemp -d)"
readonly TEST_ROOT
cleanup() {
  case "${TEST_ROOT}" in
    /tmp/*|/var/tmp/*) rm -rf -- "${TEST_ROOT}" ;;
    *) echo "warning: refusing to remove unexpected test path ${TEST_ROOT}" >&2 ;;
  esac
}
trap cleanup EXIT

mkdir -p \
  "${TEST_ROOT}/root/etc/ec-systemcore" \
  "${TEST_ROOT}/bin"
cp "${STAGE_REAL}/etc/ec-systemcore/ec-systemcore.json" \
  "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json"
cp "${ROOT_DIR}/tests/fixtures/fake-systemctl.sh" \
  "${TEST_ROOT}/bin/systemctl"
chmod 0755 "${TEST_ROOT}/bin/systemctl"
readonly SYSTEMCTL_LOG="${TEST_ROOT}/systemctl.log"
: > "${SYSTEMCTL_LOG}"

export EC_SYSTEMCORE_MAINTAINER_TEST_ROOT="${TEST_ROOT}/root"
export EC_SYSTEMCORE_MAINTAINER_TEST_SYSTEMD=1
export EC_SYSTEMCORE_TEST_SYSTEMCTL_LOG="${SYSTEMCTL_LOG}"
export PATH="${TEST_ROOT}/bin:${PATH}"

"${STAGE_REAL}/CONTROL/postinst"
grep -qx 'daemon-reload' "${SYSTEMCTL_LOG}" ||
  fail "postinst did not reload systemd"
grep -qx \
  'enable ec-systemcore.service ec-systemcore-configuration.service' \
  "${SYSTEMCTL_LOG}" ||
  fail "fresh install did not enable required services"
grep -qx \
  'start ec-systemcore.service ec-systemcore-configuration.service' \
  "${SYSTEMCTL_LOG}" ||
  fail "fresh install did not start required services"
[[ "$(stat -c '%a' \
  "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json")" == "660" ]] ||
  fail "postinst did not protect the configuration"
[[ "$(stat -c '%a' "${TEST_ROOT}/root/etc/ec-systemcore")" == "2770" ]] ||
  fail "configuration directory must preserve its private group on replace"
[[ "$(stat -c '%a' "${TEST_ROOT}/root/var/log/ec-systemcore")" == "750" ]] ||
  fail "observer accounts must not be able to modify logs"
[[ "$(stat -c '%a' "${TEST_ROOT}/root/run/ec-systemcore")" == "750" ]] ||
  fail "observer accounts must not be able to replace the IPC socket"
[[ "$(stat -c '%a' "${TEST_ROOT}/root/var/lib/ec-systemcore")" == "750" ]] ||
  fail "package lifecycle state directory must be root-private"
[[ "$(stat -c '%a' \
  "${TEST_ROOT}/root/var/lib/ec-systemcore/installed")" == "600" ]] ||
  fail "package lifecycle marker must be root-private"

printf '%s\n' untouched > "${TEST_ROOT}/sentinel"
mv "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json" \
  "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json.saved"
ln -s "${TEST_ROOT}/sentinel" \
  "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json"
if "${STAGE_REAL}/CONTROL/postinst" >/dev/null 2>&1; then
  fail "postinst followed a symbolic-link configuration path"
fi
grep -qx untouched "${TEST_ROOT}/sentinel" ||
  fail "postinst modified a symbolic-link target"
rm -f "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json"
mv "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json.saved" \
  "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json"

: > "${SYSTEMCTL_LOG}"
"${STAGE_REAL}/CONTROL/postinst" configure 2026.0.0
grep -qx 'daemon-reload' "${SYSTEMCTL_LOG}" ||
  fail "upgrade postinst did not reload systemd"
grep -qx \
  'try-restart ec-systemcore.service ec-systemcore-configuration.service' \
  "${SYSTEMCTL_LOG}" ||
  fail "upgrade did not preserve stopped/disabled service state"
if grep -Eq '(^| )(enable|start)( |$)' "${SYSTEMCTL_LOG}"; then
  fail "upgrade enabled or started an intentionally stopped service"
fi

: > "${SYSTEMCTL_LOG}"
"${STAGE_REAL}/CONTROL/prerm" upgrade
[[ ! -s "${SYSTEMCTL_LOG}" ]] ||
  fail "prerm upgrade must leave running services untouched"

printf '%s\n' preserve > "${TEST_ROOT}/root/var/log/ec-systemcore/state"
"${STAGE_REAL}/CONTROL/prerm" remove
grep -qx 'stop ec-systemcore-configuration.service ec-systemcore.service' \
  "${SYSTEMCTL_LOG}" ||
  fail "prerm remove did not stop services in dependency order"
grep -qx 'disable ec-systemcore-configuration.service ec-systemcore.service' \
  "${SYSTEMCTL_LOG}" ||
  fail "prerm remove did not disable services"
"${STAGE_REAL}/CONTROL/postrm" remove
[[ ! -e "${TEST_ROOT}/root/var/lib/ec-systemcore/installed" ]] ||
  fail "package removal retained the fresh-install lifecycle marker"
[[ -f "${TEST_ROOT}/root/etc/ec-systemcore/ec-systemcore.json" ]] ||
  fail "maintainer scripts removed administrator configuration"
[[ -f "${TEST_ROOT}/root/var/log/ec-systemcore/state" ]] ||
  fail "maintainer scripts removed logs/state"

if "${STAGE_REAL}/CONTROL/prerm" unexpected >/dev/null 2>&1; then
  fail "prerm accepted an unknown package-manager action"
fi

echo "Validated package contents and install/upgrade/remove lifecycle."
