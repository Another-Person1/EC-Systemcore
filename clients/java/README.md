# ec-systemcore Java client

The client is built and tested on the Java 25 robot runtime while emitting
Java 17-compatible bytecode with `javac --release 17`. It talks to the local
daemon over the ECSC v1 Unix-domain socket. It does not require WPILib, JNI, a
network port, TLS, or credentials.
Unix socket permissions determine whether a process can observe or control.

`SystemCoreClient.connect(...)` returns immediately. One bounded background
thread owns connection, framing, heartbeats, and reconnects. Robot-loop calls
only enqueue bounded commands and return `CompletableFuture<Ack>` values; they
do not wait for the daemon.

Every connection generation starts with outputs disabled. The client never
replays output writes and never automatically re-enables after a disconnect,
epoch change, explicit disable, output-command timeout, or daemon restart.
Application code must observe a fresh controller session, explicitly enable,
and then send a complete output image for every output-capable SubDevice at
least once per configured `output_command_timeout_ms`.

For a periodic robot loop, poll immutable snapshots:

```java
SystemCoreConfig config =
    SystemCoreConfig.builder()
        .requestedRole(SystemCoreTypes.ClientRole.CONTROLLER)
        .subscribeInputs(true)
        .inputPeriodMillis(20)
        .build();

SystemCoreClient ethercat = SystemCoreClient.connect(config);

// In disabledInit/disabledPeriodic:
if (ethercat.outputsEnabled()) {
  ethercat.disableOutputs(); // never block with get()/join() in the robot loop
}

// After robot enable, once health reports CONTROLLING_DISABLED:
ethercat.enableOutputs().thenAccept(
    ack -> {
      if (!ack.ok()) {
        System.err.println("EtherCAT enable rejected: " + ack.status());
      }
    });

// In each enabled periodic iteration:
if (ethercat.outputsEnabled()) {
  ethercat.tryWriteOutput(0, 1, 0, completeSubDeviceImage);
}
ethercat.latestInput(0, 1).ifPresent(input -> input.copyDataTo(reusedInputBuffer));
```

Use asynchronous future continuations for logging or state transitions. A
blocking listener or continuation is isolated from socket heartbeats, but it
can delay later notifications or command completions on its own bounded
executor.
