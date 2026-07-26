# ec-systemcore robot-code clients

The daemon exposes bounded ECSC protocol version 1 on
`/run/ec-systemcore/ec-systemcore.sock`. Java is the supported robot-code
integration. It owns framing, heartbeats, bounded queues, epochs, disconnect
detection, and reconnection so robot code does not duplicate the wire protocol.

## Linux authorization

Opening the socket and claiming the single controller lease are separate
checks:

- `ec-systemcore-observer` grants access to the mode-0660 socket and its
  mode-0750 parent directory.
- UID 0, `controller_uids`, `controller_gids`, or membership in
  `controller_group` can authorize control.
- `controller_group` defaults to `ec-systemcore-controller`, and Linux
  supplementary peer groups are honored through `SO_PEERGROUPS`.

A non-root robot service therefore needs both dedicated groups:

```ini
[Service]
User=robot-program
Group=robot-program
SupplementaryGroups=ec-systemcore-observer ec-systemcore-controller
ExecStart=/usr/bin/robot-program
NoNewPrivileges=true
```

Do not make the socket world-accessible, authorize a shared interactive group,
or add the configuration service account to the controller group.

## Java (primary)

The installed JAR is `/usr/share/java/ec-systemcore-client.jar`; its module and
package are both `ec_systemcore`. WPILib 2027 uses JDK 25. The library is built
and tested on that runtime with Java 17 bytecode and has no WPILib or JNI
dependency.

```java
import ec_systemcore.SystemCoreClient;
import ec_systemcore.SystemCoreConfig;
import ec_systemcore.SystemCoreTypes.ClientRole;

SystemCoreConfig config =
    SystemCoreConfig.builder()
        .requestedRole(ClientRole.CONTROLLER)
        .subscribeInputs(true)
        .inputPeriodMillis(20)
        .build();
SystemCoreClient ethercat = SystemCoreClient.connect(config);

// Disabled code path:
ethercat.disableOutputs(); // asynchronous; do not join/get in the robot loop

// After a fresh healthy controller session:
ethercat.enableOutputs().thenAccept(ack -> {
  if (!ack.ok()) {
    System.err.println("EtherCAT enable rejected: " + ack.status());
  }
});

// Every enabled periodic iteration:
if (ethercat.outputsEnabled()) {
  if (!ethercat.tryWriteOutput(0, 1, 0, completeSubDeviceImage)) {
    // The bounded queue did not accept this cycle. Do not block; the daemon's
    // output watchdog will fail closed if fresh complete images stop arriving.
  }
}
ethercat.latestInput(0, 1).ifPresent(
    image -> image.copyDataTo(reusedInputBuffer));
```

`connect()` returns immediately. One bounded worker owns the socket, reconnect,
framing, heartbeat, and request timeouts. Public commands return
`CompletableFuture<Ack>` and never wait for I/O. Listener callbacks and future
completions use a separate bounded executor so slow user code cannot stop
heartbeats.

## C++

The C++ IPC library is not a supported robot-code integration for this release.

```cmake
find_package(ec-systemcore-client CONFIG REQUIRED)
target_link_libraries(robot-program PRIVATE ec-systemcore::client)
```

The public header is `<ec_systemcore/client.hpp>`. `ec_systemcore::Client` is a
thread-safe, no-throw client. With background reconnect enabled, its worker
polls and heartbeats while the robot loop drains bounded input and bus-info
queues. No user callback runs on the socket worker.

## Mandatory reconnect/output contract

Every new connection generation starts with outputs disabled. Clients never
replay an enable or output write after a disconnect, daemon restart, epoch
change, topology/adapter fault, heartbeat timeout, explicit disable, or output
watchdog expiration.

Robot code must:

1. treat disconnected, stale, reinitializing, degraded, or faulted status as
   disabled;
2. wait for a fresh controller grant and healthy operational buses;
3. explicitly enable once for that generation;
4. send a complete output image for every output-capable SubDevice at least
   once per `output_command_timeout_ms`;
5. immediately return to the disabled flow after any rejection or epoch
   change.

Input PDO fragments are reassembled with cycle/epoch/order/bounds checks.
Partial images are never exposed as coherent sensor data, stale fragments are
dropped, and any bounded-memory violation invalidates the connection.
