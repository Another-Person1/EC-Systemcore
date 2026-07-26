#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly ROOT_DIR
cd "${ROOT_DIR}"

fail() {
  echo "error: $*" >&2
  exit 1
}

command -v jq >/dev/null 2>&1 || fail "jq is required"

VERSION="$(< VERSION)"
readonly VERSION
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
  fail "VERSION must be a stable numeric semantic version"
[[ "$(jq -r '.version' configuration/package.json)" == "${VERSION}" ]] ||
  fail "configuration/package.json version does not match VERSION"

BUN_VERSION="$(< .bun-version)"
readonly BUN_VERSION
[[ "$(jq -r '.packageManager' configuration/package.json)" == "bun@${BUN_VERSION}" ]] ||
  fail "configuration packageManager does not match .bun-version"
[[ "$(jq -r '.engines.bun' configuration/package.json)" == "${BUN_VERSION}" ]] ||
  fail "configuration Bun engine does not match .bun-version"
grep -qx "ENV BUN_VERSION=${BUN_VERSION}" .docker/linux-arm64.Dockerfile ||
  fail "Docker Bun version does not match .bun-version"
grep -qx 'ENV JAVA_VERSION=25.0.3+9' .docker/linux-arm64.Dockerfile ||
  fail "Docker Java version must match the pinned WPILib 2027 JDK"
ARM64_BUN_ZIP_SHA256="$(< .bun-arm64-zip.sha256)"
readonly ARM64_BUN_ZIP_SHA256
[[ "${ARM64_BUN_ZIP_SHA256}" =~ ^[0-9a-f]{64}$ ]] ||
  fail ".bun-arm64-zip.sha256 is invalid"
grep -Fq 'bun-linux-aarch64.zip' .docker/linux-arm64.Dockerfile ||
  fail "Docker image does not fetch the official ARM64 Bun runtime"
if grep -Eq '^Depends: .*\bbun\b' CONTROL/control; then
  fail "the target package must not depend on an unverified external Bun package"
fi

grep -qx 'Version: @EC_SYSTEMCORE_VERSION@' CONTROL/control ||
  fail "CONTROL version must be substituted from the authoritative VERSION"

[[ "$(jq -r '.configurePresets | length' CMakePresets.json)" == "1" ]] ||
  fail "there must be one canonical ARM64 configure preset"
[[ "$(jq -r '.configurePresets[0].name' CMakePresets.json)" == "linux-arm64" ]] ||
  fail "the canonical build preset must be named linux-arm64"

echo "Validated authoritative project, Bun, package, and build versions."
