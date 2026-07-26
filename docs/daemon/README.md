# ec-systemcore daemon configuration reference

The C++23 ARM64 daemon runs one independent SOEM MainDevice lifecycle per
enabled interface mapping and exposes ECSC v1 only through the local Unix
socket. It has no NetworkTables, HTTP, or robot-language runtime dependency.

Build and validate it with the repository's canonical pinned toolchain:

```sh
cmake --preset linux-arm64
cmake --build --preset linux-arm64
ctest --preset linux-arm64
```

## Configuration lifecycle

The strict JSON file is `/etc/ec-systemcore/ec-systemcore.json`. It is bounded
to 1 MiB, nesting depth 32, and 8192 nodes. Duplicate, unknown, mistyped,
out-of-range, and obsolete keys are rejected. A change advances the safety
epoch, zeros outputs, and exits with the temporary-failure code so systemd
restarts the daemon into the new configuration.

The complete root schema is:

| Field | Contract |
| --- | --- |
| `allow_restricted_interfaces` | Boolean; required opt-in before using `eth0`, `wlan0`, `usb0`, or `lo`. |
| `cycle_period_us` | Integer 5,000–1,000,000. |
| `heartbeat_timeout_ms` | Integer 100–5,000. |
| `output_command_timeout_ms` | Integer 20–heartbeat timeout and at least two cycle periods. |
| `log_directory` | Fixed at `/var/log/ec-systemcore`. |
| `log_count_limit` | Integer 1–1,000. |
| `free_space_threshold_mb` | Integer 0–1,048,576. |
| `realtime_memory_lock` | Optional boolean request; failure is reported, not fatal. |
| `realtime_fifo` | Optional boolean request; failure falls back to `SCHED_OTHER`. |
| `realtime_priority` | Optional signed 32-bit FIFO priority request. |
| `realtime_cpu` | Optional unsigned CPU index request. |
| `controller_group` | Linux group name, default `ec-systemcore-controller`. |
| `controller_uids` / `controller_gids` | Up to 64 unique numeric peer IDs each. |
| `interface_mappings` | Required array of at most eight mappings. |

An interface mapping contains:

| Field | Contract |
| --- | --- |
| `logical_name` | Unique 1–32 character `[A-Za-z0-9_-]+` name. |
| `physical_interface` | Unique Linux interface name, at most 15 characters. |
| `enabled` | Boolean; defaults true. Disabled entries are retained but not opened. |
| `maximum_io_map_bytes` | 1,024–1,048,576; defaults 262,144. |
| `distributed_clock` | Request EtherCAT distributed-clock synchronization. |
| `distributed_clock_shift_ns` | Signed 32-bit phase shift. |
| `allow_unverified_topology` | Allows input-only discovery; all outputs remain disabled. |
| `lock` | Persistent adapter identity described below. |
| `expected_subdevices` | Ordered array of at most 199 topology entries. |

Each expected SubDevice requires unsigned 32-bit `vendor_id` and
`product_code`; `revision`, `input_bytes`, and `output_bytes` are optional
unsigned 32-bit values. Every mapped PDO direction must specify its exact byte
size. `output_bytes` is capped at 1,024, matching one atomic output-write
command.

For any output-capable bus, an exact ordered `expected_subdevices` topology is
required before output enable can succeed. The daemon logs discovered
vendor/product/revision/PDO values for operator review. It does not
automatically adopt those values. `allow_unverified_topology` is an input-only
commissioning mode and stores zero output sizes.

## Adapter lock schema

`lock.id_path` is required and is resolved from udev/sysfs physical path.
`permanent_mac`, `usb_serial`, `usb_vendor_id`, and `usb_product_id` are
optional additional checks. Path-only identities are valid for adapters that
do not expose a permanent MAC. Broadcast/multicast MACs, malformed USB IDs,
control characters, excessive lengths, and contradictory identities are
rejected.

A configured lock is independent of transient names such as `eth1`. Missing,
ambiguous, or mismatched identity keeps the bus out of operation and outputs
disabled. Reconnecting the matching adapter is detected by periodic scan and
automatically starts reinitialization. Persistent lock/unlock is performed in
Configuration & Diagnostics; the controller IPC also supports an explicit
runtime-only unlock for recovery.

## Standard-kernel and recovery behavior

PREEMPT_RT is optional. At startup each bus attempts requested memory lock,
affinity, and FIFO policy. Any unavailable feature is exposed through
scheduling mode/error bits, while the cycle continues with standard monotonic
absolute sleeps. The service-level CPU/IO weights and nice value continue to
favor safety work even without FIFO scheduling.

Link loss, process-data faults, distributed-clock unlock, adapter changes, and
initialization failure close the SOEM context and retry automatically. Output
permission is revoked before retry. After reinitialization, a new safety epoch
requires a fresh controller grant, explicit output enable, and fresh complete
output images.

Events are bounded, rotated `ec-systemcore-*.log` files under the fixed log
directory. Configuration & Diagnostics can view/download them read-only.
