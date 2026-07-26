#!/usr/bin/env bash
set -euo pipefail

fail() {
  echo "error: $*" >&2
  exit 1
}

[[ "$#" -eq 1 ]] || fail "usage: $0 /path/to/ec-systemcore-daemon"
readonly BINARY="$1"
[[ -f "${BINARY}" && ! -L "${BINARY}" ]] ||
  fail "${BINARY} must be a regular, non-symlink file"

READELF="${EC_SYSTEMCORE_READELF:-aarch64-linux-gnu-readelf}"
command -v "${READELF}" >/dev/null 2>&1 ||
  fail "${READELF} is required for ARM64 ELF validation"

ELF_HEADER="$("${READELF}" -W -h "${BINARY}")"
readonly ELF_HEADER
PROGRAM_HEADERS="$("${READELF}" -W -l "${BINARY}")"
readonly PROGRAM_HEADERS
DYNAMIC_SECTION="$("${READELF}" -W -d "${BINARY}")"
readonly DYNAMIC_SECTION
VERSION_INFO="$("${READELF}" -W --version-info "${BINARY}")"
readonly VERSION_INFO

grep -Eq 'Class:[[:space:]]+ELF64$' <<<"${ELF_HEADER}" ||
  fail "daemon is not an ELF64 binary"
grep -Eq 'Data:[[:space:]]+2.s complement, little endian$' <<<"${ELF_HEADER}" ||
  fail "daemon is not little-endian"
grep -Eq 'Machine:[[:space:]]+AArch64$' <<<"${ELF_HEADER}" ||
  fail "daemon is not built for AArch64"
grep -Eq 'Type:[[:space:]]+DYN .*Position-Independent Executable' \
  <<<"${ELF_HEADER}" ||
  fail "daemon is not a PIE executable"

grep -Fq 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' \
  <<<"${PROGRAM_HEADERS}" ||
  fail "daemon has an unexpected or missing ARM64 dynamic loader"
grep -Eq 'GNU_RELRO[[:space:]]' <<<"${PROGRAM_HEADERS}" ||
  fail "daemon is missing GNU_RELRO"
grep -Eq 'GNU_STACK[[:space:]].*[[:space:]]RW[[:space:]]' \
  <<<"${PROGRAM_HEADERS}" ||
  fail "daemon GNU_STACK flags are unexpected"
if grep -Eq 'GNU_STACK[[:space:]].*[[:space:]]RWE([[:space:]]|$)' \
  <<<"${PROGRAM_HEADERS}"; then
  fail "daemon requests an executable stack"
fi

if ! grep -Eq '\(BIND_NOW\)|FLAGS.*NOW' <<<"${DYNAMIC_SECTION}"; then
  fail "daemon is missing immediate binding (-z now)"
fi
if grep -Eq '\((RPATH|RUNPATH|TEXTREL)\)' <<<"${DYNAMIC_SECTION}"; then
  fail "daemon contains RPATH, RUNPATH, or text relocations"
fi

MAX_GLIBC_VERSION="$(
  grep -Eo 'GLIBC_[0-9]+\.[0-9]+' <<<"${VERSION_INFO}" |
    sed 's/^GLIBC_//' |
    sort -Vu |
    tail -n 1
)"
[[ -n "${MAX_GLIBC_VERSION}" ]] ||
  fail "daemon does not declare a GLIBC symbol-version dependency"
if [[ "$(printf '%s\n' "${MAX_GLIBC_VERSION}" 2.36 | sort -V | tail -n 1)" != "2.36" ]]; then
  fail "daemon requires GLIBC_${MAX_GLIBC_VERSION}; target maximum is GLIBC_2.36"
fi

echo "Validated ARM64 PIE/RELRO/NOW/no-exec-stack ABI: ${BINARY}"
