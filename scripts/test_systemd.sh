#!/usr/bin/env bash
set -euo pipefail

readonly ROOT_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)}"
if [[ -d "${ROOT_DIR}/daemon" ]]; then
  readonly UNIT_DIR="${ROOT_DIR}/daemon"
elif [[ -d "${ROOT_DIR}/lib/systemd/system" ]]; then
  readonly UNIT_DIR="${ROOT_DIR}/lib/systemd/system"
else
  echo "error: unable to locate source or staged systemd units beneath ${ROOT_DIR}" >&2
  exit 1
fi
readonly DAEMON_UNIT="${UNIT_DIR}/ec-systemcore.service"
readonly CONFIGURATION_UNIT="${UNIT_DIR}/ec-systemcore-configuration.service"

fail() {
  echo "error: $*" >&2
  exit 1
}

for unit in "${DAEMON_UNIT}" "${CONFIGURATION_UNIT}"; do
  [[ -f "${unit}" ]] || fail "missing unit ${unit}"
  grep -qx 'NoNewPrivileges=true' "${unit}" ||
    fail "${unit} must set NoNewPrivileges=true"
  grep -qx 'ProtectSystem=strict' "${unit}" ||
    fail "${unit} must set ProtectSystem=strict"
  grep -qx 'PrivateTmp=true' "${unit}" ||
    fail "${unit} must set PrivateTmp=true"
  grep -qx 'ProtectProc=invisible' "${unit}" ||
    fail "${unit} must hide unrelated process details"
  grep -qx 'RestrictSUIDSGID=true' "${unit}" ||
    fail "${unit} must block new setuid/setgid files"
  grep -qx 'RemoveIPC=true' "${unit}" ||
    fail "${unit} must clean up IPC objects"
  grep -qx 'StartLimitIntervalSec=0' "${unit}" ||
    fail "${unit} must never permanently suppress automatic recovery"
  grep -Eq '^ReadOnlyPaths=.*-/run/udev/data([[:space:]]|$)' "${unit}" ||
    fail "${unit} must access the udev database read-only"
  if grep -Eq '^User=(root)?$|^DynamicUser=false$' "${unit}"; then
    fail "${unit} must not run as root"
  fi
done

grep -qx 'User=ec-systemcore' "${DAEMON_UNIT}" ||
  fail "daemon unit must use the ec-systemcore account"
grep -qx 'Group=ec-systemcore-observer' "${DAEMON_UNIT}" ||
  fail "daemon socket must use the observer group"
grep -qx 'RuntimeDirectory=ec-systemcore' "${DAEMON_UNIT}" ||
  fail "daemon unit must create the canonical runtime directory"
grep -qx 'UMask=0027' "${DAEMON_UNIT}" ||
  fail "daemon-created logs must not be writable by observers"
grep -qx 'Nice=-5' "${DAEMON_UNIT}" ||
  fail "daemon must retain CPU scheduling preference under configuration load"
grep -qx 'CPUWeight=1000' "${DAEMON_UNIT}" ||
  fail "daemon must retain CPU share under configuration load"
grep -qx 'IOWeight=1000' "${DAEMON_UNIT}" ||
  fail "daemon must retain I/O share under diagnostic load"
grep -qx 'RuntimeDirectoryMode=0750' "${DAEMON_UNIT}" ||
  fail "daemon runtime directory must be private to the observer group"
grep -qx 'LogsDirectoryMode=0750' "${DAEMON_UNIT}" ||
  fail "daemon log directory must not be writable by observers"
grep -qx 'ProcSubset=pid' "${DAEMON_UNIT}" ||
  fail "daemon unit must expose only the process subset of procfs"
grep -Eq '^AmbientCapabilities=.*CAP_NET_RAW' "${DAEMON_UNIT}" ||
  fail "daemon requires CAP_NET_RAW"
if grep -Eq 'CAP_NET_ADMIN|0\.0\.0\.0|:::' "${DAEMON_UNIT}"; then
  fail "daemon unit grants an unnecessary capability or wildcard listener"
fi

grep -qx 'User=ec-systemcore-configuration' "${CONFIGURATION_UNIT}" ||
  fail "configuration unit must use its dedicated account"
grep -qx 'Group=ec-systemcore-observer' "${CONFIGURATION_UNIT}" ||
  fail "configuration must have observer-only primary credentials"
grep -qx \
  'ExecStart=/usr/lib/ec-systemcore/runtime/bun /usr/lib/ec-systemcore/configuration/server.js' \
  "${CONFIGURATION_UNIT}" ||
  fail "configuration must use the package-owned ARM64 Bun runtime"
grep -qx 'WorkingDirectory=/usr/lib/ec-systemcore/configuration' \
  "${CONFIGURATION_UNIT}" ||
  fail "configuration runtime must start from its read-only application directory"
grep -qx 'UMask=0027' "${CONFIGURATION_UNIT}" ||
  fail "configuration-created configuration files require private modes"
grep -qx 'Nice=10' "${CONFIGURATION_UNIT}" ||
  fail "configuration must yield CPU time to robot-control work"
grep -qx 'CPUWeight=25' "${CONFIGURATION_UNIT}" ||
  fail "configuration must yield CPU share to robot-control work"
grep -qx 'IOWeight=25' "${CONFIGURATION_UNIT}" ||
  fail "configuration must yield I/O share to robot-control work"
if grep -Eq 'Group(s)?=.*ec-systemcore-controller' "${CONFIGURATION_UNIT}"; then
  fail "configuration must never receive controller credentials"
fi
grep -qx 'Environment=EC_SYSTEMCORE_HOST=0.0.0.0' "${CONFIGURATION_UNIT}" ||
  fail "configuration must be reachable on the isolated robot LAN by default"
grep -qx 'Environment=EC_SYSTEMCORE_CONFIG=/etc/ec-systemcore/ec-systemcore.json' \
  "${CONFIGURATION_UNIT}" ||
  fail "configuration unit uses the wrong configuration path"
grep -qx 'Environment=EC_SYSTEMCORE_SOCKET=/run/ec-systemcore/ec-systemcore.sock' \
  "${CONFIGURATION_UNIT}" ||
  fail "configuration unit uses the wrong daemon socket"
grep -qx 'Environment=EC_SYSTEMCORE_LOG_ROOT=/var/log/ec-systemcore' \
  "${CONFIGURATION_UNIT}" ||
  fail "configuration unit uses the wrong log path or environment variable"
grep -qx 'CapabilityBoundingSet=' "${CONFIGURATION_UNIT}" ||
  fail "configuration capability bounding set must be empty"
grep -qx 'ReadWritePaths=/etc/ec-systemcore' "${CONFIGURATION_UNIT}" ||
  fail "configuration may write only its private configuration directory"
grep -Eq '^ReadOnlyPaths=.*([[:space:]]|^)/var/log/ec-systemcore([[:space:]]|$)' \
  "${CONFIGURATION_UNIT}" ||
  fail "configuration log access must be read-only"
grep -qx 'Wants=ec-systemcore.service' "${CONFIGURATION_UNIT}" ||
  fail "configuration must bring up the fail-safe daemon"
if grep -Eq 'CAP_[A-Z_]+|EC_SYSTEMCORE_ENABLE_RESTARTS=true|IPAddressDeny=' \
  "${CONFIGURATION_UNIT}"; then
  fail "configuration unit has an unexpected capability or network restriction"
fi
if grep -qx 'MemoryDenyWriteExecute=true' "${CONFIGURATION_UNIT}"; then
  fail "configuration enabled a sandbox flag incompatible with JavaScriptCore JIT"
fi

if command -v systemd-analyze >/dev/null 2>&1; then
  analyze_output="$(
    systemd-analyze verify "${DAEMON_UNIT}" "${CONFIGURATION_UNIT}" 2>&1 || true
  )"
  if grep -Evi \
    'Command .* is not executable: No such file or directory|Unit .* not found' \
    <<<"${analyze_output}" |
      grep -Eq ':[[:space:]]+(Failed|Unknown|Invalid|Missing|error)|Unknown key'; then
    echo "${analyze_output}" >&2
    fail "systemd-analyze found a unit error"
  fi
fi

echo "Validated systemd unit syntax and safe defaults."
