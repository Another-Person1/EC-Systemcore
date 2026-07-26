# ec-systemcore

`ec-systemcore` is an experimental EtherCAT MainDevice service for the 2027
FIRST FRC/FTC ARM64 control platform based on Raspberry Pi Compute Module 5. It
contains a C++23 daemon, a required browser-based Configuration & Diagnostics
application, and a reconnect-safe local IPC client for Java.

This project is unofficial and alpha quality. It is not a safety controller,
safety PLC, FSoE implementation, or substitute for certified safety hardware.
Treat every actuator as capable of unexpected motion and retain an independent,
verified way to remove power.

## Installed contract

| Purpose | Installed path or unit |
| --- | --- |
| Daemon | `/usr/bin/ec-systemcore-daemon` |
| Configuration | `/etc/ec-systemcore/ec-systemcore.json` |
| IPC socket | `/run/ec-systemcore/ec-systemcore.sock` |
| Logs | `/var/log/ec-systemcore` |
| Daemon unit | `ec-systemcore.service` |
| Configuration unit | `ec-systemcore-configuration.service` |
| Configuration bundle | `/usr/lib/ec-systemcore/configuration/server.js` |
| Configuration runtime | `/usr/lib/ec-systemcore/runtime/bun` |
| Java client | `/usr/share/java/ec-systemcore-client.jar` |
| C++ client | `/usr/lib/libec-systemcore-client.a` |

Both services run under distinct unprivileged accounts. The daemon receives
`CAP_NET_RAW` for EtherCAT frames plus only the capabilities needed to attempt
optional FIFO scheduling and memory locking. The configuration service has no
Linux capabilities and is never controller-authorized. Resource weights and
nice levels favor daemon cycle, heartbeat, and fail-safe work over browser and
log-archive load.

A fresh package installation enables and starts both services with zero
configured EtherCAT buses. That empty-bus configuration is fail-safe. Upgrades
use `try-restart` so an administrator's stopped/disabled state is preserved.
Removal stops and disables the units but retains administrator configuration
and logs.

## Configuration & Diagnostics

Open `http://<systemcore-address>:8080/` from the isolated robot LAN. The
packaged service binds `0.0.0.0:8080` and intentionally has no accounts,
passwords, cookies, login flow, or TLS. The isolated robot network is therefore
the trust boundary; never expose this port to the public internet or an
untrusted venue/school network.

The application uses a transparent short-lived same-origin nonce and strict
Host/Origin checks to prevent cross-site browser requests. This is request
integrity, not user authentication. DNS, mDNS, team hostnames, and direct IP
addresses work without preconfiguration when Host and Origin agree.

Optional comma-separated environment settings can narrow a deployment:

```ini
[Service]
Environment=EC_SYSTEMCORE_ALLOWED_CLIENT_SUBNETS=10.20.27.0/24
Environment=EC_SYSTEMCORE_ALLOWED_HOSTS=10.20.27.2:8080,ec-systemcore.local:8080
Environment=EC_SYSTEMCORE_ALLOWED_ORIGINS=http://10.20.27.2:8080,http://ec-systemcore.local:8080
```

Create a drop-in with
`sudo systemctl edit ec-systemcore-configuration.service`, then restart that
unit. Leaving these variables unset provides the intended zero-configuration
LAN behavior.

The application exposes status, aggregate bus counts, jitter/fault counters,
read-only logs, adapter identity locking, and the full JSON schema through its
advanced editor. It deliberately does not restart services, delete logs, send
raw outputs, assign EtherCAT station IDs, or upload ESI files. Individual
SubDevice topology is not auto-adopted from IPC; review the daemon's discovered
topology log and enter `expected_subdevices` explicitly before enabling
output-capable hardware.

## Fail-safe runtime behavior

The daemon supports a normal Linux kernel. PREEMPT_RT, FIFO scheduling, CPU
affinity, and memory locking are optional latency improvements, never startup
requirements. Applied scheduling mode, unavailable features, current/max
jitter, overruns, and timing degradation are reported over IPC and in the
configuration application.

Each enabled bus is isolated in its own lifecycle worker. Link loss, three
consecutive working-counter failures, distributed-clock failure, adapter
identity mismatch, topology mismatch, controller disconnect, heartbeat
timeout, stale output commands, configuration change, or daemon shutdown
advances a safety epoch and forces output bytes to zero. The bus closes its
SOEM context and automatically retries initialization with bounded delay while
the daemon and robot process stay alive.

No controller lease, output enable, or output write survives a reconnect or
epoch change. Robot code must observe a fresh healthy session, explicitly
enable outputs, and continuously submit complete output images within
`output_command_timeout_ms`.

## Stable USB Ethernet identity

Linux interface names can change after unplug/replug or discovery-order
changes. A bus can be locked to read-only sysfs/udev identity:

- required physical connection path (`id_path`);
- optional permanent MAC (`permanent_mac`);
- optional USB serial, vendor ID, and product ID.

The application can persistently lock or unlock an adapter-to-port mapping.
The daemon never writes udev rules, changes a MAC address, or renames a kernel
interface. A missing or mismatched locked adapter fails closed. Reconnecting
the same physical identity triggers automatic rescan/reinitialization; a
replacement remains blocked until an operator deliberately updates or removes
the lock.

## Robot-code IPC

ECSC protocol version 1 is a bounded binary protocol on the local Unix socket.
It has no network listener, JNI bridge, TLS, or credentials. Socket access and
controller authorization are both enforced by Linux peer credentials.

A non-root robot account should belong to both groups:

```sh
sudo usermod -aG ec-systemcore-observer,ec-systemcore-controller robot-program
```

`ec-systemcore-observer` permits traversal/opening of the private runtime
directory and mode-0660 socket. `ec-systemcore-controller` permits the single
controller lease; supplementary group membership is honored. Log out/restart
the service after changing memberships. Never add
`ec-systemcore-configuration` to the controller group.

Java is the primary robot integration. CI uses the official WPILib 2027 JDK 25
runtime while intentionally compiling the dependency-free client with
`--release 17` bytecode for forward compatibility:

```sh
javac -cp /usr/share/java/ec-systemcore-client.jar Robot.java
java -cp /usr/share/java/ec-systemcore-client.jar:. Robot
```

Java is the supported robot-code integration for the 2027 runtime. See [the
client guide](docs/client-api.md) for the nonblocking/reconnect contract and
example.

## Reproducible ARM64 build

The canonical build runs in the pinned
[Bookworm toolchain image](.docker/linux-arm64.Dockerfile). It uses a Debian 12
ARM64 sysroot with a GLIBC 2.36 ceiling, C++23, pinned Eclipse Temurin JDK 25,
pinned Bun, a dated Debian snapshot, verified release archives, and an
installed Bun archive-to-executable provenance manifest.

```sh
docker build -f .docker/linux-arm64.Dockerfile -t ec-systemcore-ci .
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp/ec-systemcore-ci-home \
  -e GIT_CONFIG_COUNT=1 \
  -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/workspace \
  -e SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)" \
  -v "$PWD:/workspace" -w /workspace \
  ec-systemcore-ci bash scripts/ci.sh development
```

The pipeline type-checks/tests/bundles Configuration & Diagnostics; compiles
and tests the Java client; runs daemon unit, reconnect, and
adapter lifecycle tests under AArch64 emulation; validates systemd and package
lifecycle behavior; checks ELF ABI/hardening; builds a reproducible IPK; and
emits an SPDX SBOM plus SHA-256 checksums.

## First operation

1. Install the ARM64 IPK. Both services start with no enabled buses.
2. Open Configuration & Diagnostics on port 8080.
3. Connect an EtherCAT-only adapter; do not share it with robot-management
   traffic.
4. Add the mapping disabled, lock its stable identity, and review logged
   discovered topology.
5. Enter and verify every expected vendor/product/revision/PDO size.
6. Enable the mapping and verify operational, lock, topology, timing, and
   distributed-clock status before authorizing robot outputs.

## Licensing

First-party code is GPL-3.0-only; see [LICENSE](LICENSE). SOEM and the bundled
Bun runtime retain their upstream licenses and notices. Exact source/runtime
provenance is recorded in [THIRD_PARTY.md](THIRD_PARTY.md).
