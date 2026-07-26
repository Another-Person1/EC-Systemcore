#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly ROOT_DIR
readonly MODE="${1:-development}"

fail() {
  echo "error: $*" >&2
  exit 1
}

case "${MODE}" in
  development)
    readonly PACKAGE_FLAVOR="development"
    ;;
  release)
    readonly PACKAGE_FLAVOR="release"
    ;;
  *)
    fail "usage: $0 development|release"
    ;;
esac
readonly BUILD_PRESET="linux-arm64"

cd "${ROOT_DIR}"

if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
  SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
fi
[[ "${SOURCE_DATE_EPOCH}" =~ ^[0-9]+$ ]] ||
  fail "SOURCE_DATE_EPOCH must be an integer Unix timestamp"
export SOURCE_DATE_EPOCH

for command_name in \
  actionlint \
  aarch64-linux-gnu-readelf \
  bash \
  bun \
  cmake \
  cmp \
  cppcheck \
  jar \
  java \
  javac \
  jq \
  opkg-build \
  python3 \
  qemu-aarch64 \
  sha256sum \
  shellcheck \
  syft; do
  command -v "${command_name}" >/dev/null 2>&1 ||
    fail "${command_name} is required in the CI image"
done

[[ "$(bun --version)" == "$(< .bun-version)" ]] ||
  fail "installed Bun version does not match .bun-version"
[[ "$(javac -version 2>&1)" =~ ^javac[[:space:]]+25([.]|$) ]] ||
  fail "CI requires JDK 25 while compiling the Java client with --release 17"
python3 -c \
  'import sys; raise SystemExit(sys.version_info < (3, 11))' ||
  fail "CI requires Python 3.11 or newer"

bash scripts/validate_branding.sh
bash scripts/validate_versions.sh
actionlint .github/workflows/*.yml
shellcheck scripts/*.sh CONTROL/postinst CONTROL/prerm CONTROL/postrm \
  tests/fixtures/fake-systemctl.sh

bun install --cwd configuration --frozen-lockfile --ignore-scripts
if jq -e '.scripts.check != null' configuration/package.json >/dev/null; then
  bun run --cwd configuration check
else
  for configuration_script in test check:bundle; do
    if jq -e --arg name "${configuration_script}" \
      '.scripts[$name] != null' configuration/package.json >/dev/null; then
      bun run --cwd configuration "${configuration_script}"
    fi
  done
fi

cppcheck \
  --std=c++23 \
  --enable=warning,performance,portability \
  --error-exitcode=1 \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  --suppress=unusedFunction \
  -Idaemon/include \
  -Ithird_party/SOEM \
  -Ithird_party/SOEM/soem \
  -Ithird_party/SOEM/osal \
  daemon/src/main.cpp \
  daemon/src/client.cpp

cmake --preset "${BUILD_PRESET}"
cmake --build --preset "${BUILD_PRESET}"
ctest --test-dir "build/${BUILD_PRESET}" --output-on-failure

export BUILD_DIR="${ROOT_DIR}/build/${BUILD_PRESET}"
export PACKAGE_FLAVOR
bash scripts/build_ipk.sh
bash scripts/test_package.sh "${ROOT_DIR}/build/package/stage"

mapfile -t BUILT_PACKAGES < <(
  find dist -maxdepth 1 -type f -name 'ec-systemcore-*-arm64.ipk' -print
)
[[ "${#BUILT_PACKAGES[@]}" -eq 1 ]] ||
  fail "expected exactly one canonical IPK in dist"
PACKAGE_VERSION="$(
  awk -F ': ' '$1 == "Version" { print $2 }' \
    build/package/stage/CONTROL/control
)"
readonly PACKAGE_VERSION
mkdir -p build/package/reproducibility
opkg-build "${ROOT_DIR}/build/package/stage" \
  "${ROOT_DIR}/build/package/reproducibility"
readonly REPRODUCED_IPK="${ROOT_DIR}/build/package/reproducibility/ec-systemcore_${PACKAGE_VERSION}_arm64.ipk"
[[ -f "${REPRODUCED_IPK}" ]] ||
  fail "reproducibility build did not produce the expected package"
cmp "${BUILT_PACKAGES[0]}" "${REPRODUCED_IPK}" ||
  fail "two packages built from the same staged tree are not byte-identical"

mkdir -p dist
VERSION="$(< VERSION)"
readonly VERSION
readonly SBOM_PATH="dist/ec-systemcore-${VERSION}-${MODE}-arm64.spdx.json"
syft "dir:${ROOT_DIR}/build/package/stage" \
  --output "spdx-json=${SBOM_PATH}"
sha256sum dist/*.ipk "${SBOM_PATH}" > dist/SHA256SUMS
sha256sum --check dist/SHA256SUMS

echo "CI validation completed for ${MODE}."
