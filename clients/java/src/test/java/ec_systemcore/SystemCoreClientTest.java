package ec_systemcore;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Properties;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import ec_systemcore.SystemCoreClient.PdoAssembler;
import ec_systemcore.SystemCoreTypes.Ack;
import ec_systemcore.SystemCoreTypes.AckStatus;
import ec_systemcore.SystemCoreTypes.AdapterIdentity;
import ec_systemcore.SystemCoreTypes.AdapterLockReason;
import ec_systemcore.SystemCoreTypes.AdapterLockState;
import ec_systemcore.SystemCoreTypes.BusState;
import ec_systemcore.SystemCoreTypes.ClientRole;
import ec_systemcore.SystemCoreTypes.HealthState;
import ec_systemcore.SystemCoreTypes.PdoInput;
import ec_systemcore.SystemCoreTypes.SchedulingMode;
import ec_systemcore.SystemCoreTypes.Status;

/** Dependency-free Java 17 protocol and fake-daemon tests. */
public final class SystemCoreClientTest {
  private static final HexFormat HEX = HexFormat.of();
  private static final Duration TEST_TIMEOUT = Duration.ofSeconds(5);

  private SystemCoreClientTest() {}

  public static void main(String[] arguments) throws Exception {
    Path vectors =
        arguments.length == 0
            ? Path.of("clients/protocol-v1-golden.properties")
            : Path.of(arguments[0]);
    testGoldenVectors(vectors);
    testFragmentedAndCoalescedFrames();
    testMalformedFramesAndPayloads();
    testPdoAssemblyBoundsAndOrdering();
    testConfigurationBounds();
    if (supportsUnixSockets()) {
      testReconnectNoReplayAndCallbackIsolation();
      testBoundedBackpressure();
      testDisableRejectionForcesReconnect();
    } else {
      System.out.println("SKIP: Unix-domain sockets are unavailable on this host");
    }
    System.out.println("SystemCoreClientTest: all checks passed");
  }

  private static void testGoldenVectors(Path path) throws Exception {
    Properties vectors = new Properties();
    try (InputStream input = Files.newInputStream(path)) {
      vectors.load(input);
    }
    long epoch = 0x1122_3344_5566_7788L;
    assertHex(
        vectors,
        "hello.controller.payload",
        EcscProtocol.encodeHello(ClientRole.CONTROLLER, true, 20, 0x0102_0304_0506_0708L));
    assertHex(
        vectors,
        "hello.controller.frame",
        EcscProtocol.bytes(
            EcscProtocol.encodeFrame(
                EcscProtocol.HELLO,
                0,
                EcscProtocol.encodeHello(
                    ClientRole.CONTROLLER, true, 20, 0x0102_0304_0506_0708L))));
    assertHex(vectors, "heartbeat.payload", EcscProtocol.encodeEpoch(epoch));
    assertHex(
        vectors,
        "heartbeat.frame",
        EcscProtocol.bytes(
            EcscProtocol.encodeFrame(
                EcscProtocol.HEARTBEAT, 0xa1b2_c3d4L, EcscProtocol.encodeEpoch(epoch))));
    assertHex(
        vectors,
        "output_enable.payload",
        EcscProtocol.encodeOutputEnable(epoch, true));
    assertHex(
        vectors,
        "output_write.payload",
        EcscProtocol.encodeOutputWrite(
            epoch, 2, 3, 0, HEX.parseHex("deadbeef")));
    assertHex(
        vectors,
        "ack.ok.payload",
        EcscProtocol.encodeAckForTest(AckStatus.OK, 0x1234_5678L));
    assertHex(
        vectors,
        "outputs_disabled.timeout.payload",
        EcscProtocol.encodeOutputsDisabledForTest(
            SystemCoreTypes.DisableReason.OUTPUT_COMMAND_TIMEOUT, 0xffff, epoch));
    assertHex(
        vectors,
        "pdo_input.payload",
        EcscProtocol.encodePdoForTest(
            2, 3, 0, 4, epoch, 1, HEX.parseHex("deadbeef")));
  }

  private static void testFragmentedAndCoalescedFrames() throws Exception {
    byte[] first =
        EcscProtocol.bytes(
            EcscProtocol.encodeFrame(
                EcscProtocol.ACK,
                7,
                EcscProtocol.encodeAckForTest(AckStatus.OK, 9)));
    byte[] second =
        EcscProtocol.bytes(
            EcscProtocol.encodeFrame(
                EcscProtocol.OUTPUTS_DISABLED,
                0,
                EcscProtocol.encodeOutputsDisabledForTest(
                    SystemCoreTypes.DisableReason.LINK_LOSS, 2, 11)));

    EcscProtocol.FrameDecoder decoder = new EcscProtocol.FrameDecoder();
    List<EcscProtocol.Frame> decoded = new ArrayList<>();
    for (byte value : first) {
      decoded.addAll(decoder.feed(ByteBuffer.wrap(new byte[] {value})));
    }
    check(decoded.size() == 1 && decoded.get(0).requestId() == 7, "fragmented frame");
    check(decoder.bufferedBytes() == 0, "decoder drained fragmented frame");

    byte[] joined = new byte[first.length + second.length];
    System.arraycopy(first, 0, joined, 0, first.length);
    System.arraycopy(second, 0, joined, first.length, second.length);
    decoded = decoder.feed(ByteBuffer.wrap(joined));
    check(decoded.size() == 2, "coalesced frame count");
    check(decoded.get(1).type() == EcscProtocol.OUTPUTS_DISABLED, "coalesced type");
  }

  private static void testMalformedFramesAndPayloads() throws Exception {
    expectThrows(
        ProtocolException.class,
        () ->
            new EcscProtocol.FrameDecoder()
                .feed(ByteBuffer.wrap(HEX.parseHex("0b000000454353430181000000000000"))),
        "undersized frame");
    expectThrows(
        ProtocolException.class,
        () ->
            new EcscProtocol.FrameDecoder()
                .feed(ByteBuffer.wrap(HEX.parseHex("0c00000045435343017f000000000000"))),
        "unknown message type");

    byte[] badAck = EcscProtocol.encodeAckForTest(AckStatus.OK, 0);
    badAck[2] = 1;
    expectThrows(
        ProtocolException.class,
        () -> EcscProtocol.decodeAck(badAck),
        "ACK reserved field");

    byte[] busInfo =
        EcscProtocol.encodeBusInfoForTest(
            0,
            BusState.OPERATIONAL,
            true,
            AdapterLockState.MATCHED,
            AdapterLockReason.NONE,
            1,
            new AdapterIdentity("x", "", "", "", ""),
            "bus",
            "eth0",
            1);
    busInfo[30] = (byte) 0xc0;
    expectThrows(
        ProtocolException.class,
        () -> EcscProtocol.decodeBusInfo(busInfo),
        "strict UTF-8");

    byte[] tooLargePrefix = new byte[4];
    ByteBuffer.wrap(tooLargePrefix)
        .order(java.nio.ByteOrder.LITTLE_ENDIAN)
        .putInt(EcscProtocol.MAXIMUM_FRAME_BODY + 1);
    expectThrows(
        ProtocolException.class,
        () -> new EcscProtocol.FrameDecoder().feed(ByteBuffer.wrap(tooLargePrefix)),
        "maximum frame bound");
  }

  private static void testPdoAssemblyBoundsAndOrdering() throws Exception {
    PdoAssembler assembler = new PdoAssembler(2, 16);
    long epoch = 3;
    check(
        assembler
            .accept(
                EcscProtocol.decodePdoChunk(
                    EcscProtocol.encodePdoForTest(
                        0, 1, 2, 4, epoch, 2, new byte[] {3, 4}),
                    16),
                Instant.now())
            .isEmpty(),
        "partial PDO remains incomplete");
    PdoInput complete =
        assembler
            .accept(
                EcscProtocol.decodePdoChunk(
                    EcscProtocol.encodePdoForTest(
                        0, 1, 0, 4, epoch, 2, new byte[] {1, 2}),
                    16),
                Instant.now())
            .orElseThrow();
    check(HEX.formatHex(complete.data()).equals("01020304"), "out-of-order PDO chunks");

    check(
        assembler
            .accept(
                EcscProtocol.decodePdoChunk(
                    EcscProtocol.encodePdoForTest(
                        0, 1, 0, 4, epoch, 1, new byte[] {9, 9, 9, 9}),
                    16),
                Instant.now())
            .isEmpty(),
        "older completed PDO is discarded");

    PdoAssembler conflicting = new PdoAssembler(2, 16);
    conflicting.accept(
        EcscProtocol.decodePdoChunk(
            EcscProtocol.encodePdoForTest(
                0, 1, 0, 4, epoch, 4, new byte[] {1, 2}),
            16),
        Instant.now());
    expectThrows(
        ProtocolException.class,
        () ->
            conflicting.accept(
                EcscProtocol.decodePdoChunk(
                    EcscProtocol.encodePdoForTest(
                        0, 1, 1, 4, epoch, 4, new byte[] {7, 3}),
                    16),
                Instant.now()),
        "conflicting overlapping chunks");

    PdoAssembler bounded = new PdoAssembler(1, 4);
    bounded.accept(
        EcscProtocol.decodePdoChunk(
            EcscProtocol.encodePdoForTest(
                0, 1, 0, 4, epoch, 1, new byte[] {1}),
            4),
        Instant.now());
    expectThrows(
        ProtocolException.class,
        () ->
            bounded.accept(
                EcscProtocol.decodePdoChunk(
                    EcscProtocol.encodePdoForTest(
                        0, 2, 0, 4, epoch, 1, new byte[] {1}),
                    4),
                Instant.now()),
        "bounded concurrent assemblies");
  }

  private static void testConfigurationBounds() throws Exception {
    expectThrows(
        IllegalArgumentException.class,
        () ->
            SystemCoreConfig.builder()
                .requestTimeout(Duration.ofDays(1))
                .build(),
        "request timeout upper bound");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            SystemCoreConfig.builder()
                .reconnectDelays(Duration.ofMillis(1), Duration.ofMinutes(2))
                .build(),
        "reconnect delay upper bound");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            SystemCoreConfig.builder()
                .socketPath(Path.of("/" + "x".repeat(108)))
                .build(),
        "Unix socket path bound");
    SystemCoreConfig defaults = SystemCoreConfig.builder().build();
    check(
        defaults.maximumTrackedPdoInputs() >= 8 * 199,
        "default retained PDO keys cover the maximum valid topology");
    check(
        defaults.maximumRetainedPdoBytes() >= 8 * 128 * 1024,
        "default retained PDO bytes cover all valid buses");
  }

  private static void testReconnectNoReplayAndCallbackIsolation() throws Exception {
    try (FakeDaemon daemon = new FakeDaemon()) {
      CountDownLatch blockedStatus = new CountDownLatch(1);
      CountDownLatch releaseStatus = new CountDownLatch(1);
      AtomicBoolean statusBlockedOnce = new AtomicBoolean();
      SystemCoreListener listener =
          new SystemCoreListener() {
            @Override
            public void onStatus(Status status) {
              if (statusBlockedOnce.compareAndSet(false, true)) {
                blockedStatus.countDown();
                awaitLatchUnchecked(releaseStatus);
              }
            }
          };

      AtomicReference<Throwable> serverFailure = new AtomicReference<>();
      AtomicBoolean replaySeen = new AtomicBoolean();
      Thread serverThread =
          daemon.start(
              serverFailure,
              server -> {
                try (FakeConnection first = server.accept(TEST_TIMEOUT)) {
                  expectHello(first, ClientRole.CONTROLLER);
                  first.sendFragmented(
                      EcscProtocol.HELLO_ACK,
                      0,
                      EcscProtocol.encodeHelloAckForTest(
                          ClientRole.CONTROLLER, false, 250, 41, 123, 0xf),
                      3);
                  while (true) {
                    EcscProtocol.Frame frame = first.read(TEST_TIMEOUT);
                    if (frame.type() == EcscProtocol.HEARTBEAT) {
                      first.ack(frame, AckStatus.OK);
                    } else if (frame.type() == EcscProtocol.OUTPUT_ENABLE
                        && frame.payload()[8] == 1) {
                      first.sendTogether(
                          EcscProtocol.encodeFrame(
                              EcscProtocol.ACK,
                              frame.requestId(),
                              EcscProtocol.encodeAckForTest(AckStatus.OK, 0)),
                          EcscProtocol.encodeFrame(
                              EcscProtocol.STATUS, 0, statusPayload(41, false)));
                    } else if (frame.type() == EcscProtocol.OUTPUT_WRITE) {
                      first.ack(frame, AckStatus.OK);
                    } else if (frame.type() == EcscProtocol.OUTPUT_ENABLE
                        && frame.payload()[8] == 0) {
                      // Cancellation must close this generation even if the command crossed IPC.
                    }
                  }
                } catch (EOFException expected) {
                  // The cancelled disable closes generation one.
                }

                try (FakeConnection second = server.accept(TEST_TIMEOUT)) {
                  expectHello(second, ClientRole.CONTROLLER);
                  second.send(
                      EcscProtocol.HELLO_ACK,
                      0,
                      EcscProtocol.encodeHelloAckForTest(
                          ClientRole.CONTROLLER, false, 250, 42, 124, 0xf));
                  long deadline = System.nanoTime() + Duration.ofMillis(400).toNanos();
                  while (System.nanoTime() - deadline < 0) {
                    EcscProtocol.Frame frame = second.tryRead(Duration.ofMillis(25));
                    if (frame == null) {
                      continue;
                    }
                    if (frame.type() == EcscProtocol.HEARTBEAT) {
                      second.ack(frame, AckStatus.OK);
                    } else if (frame.type() == EcscProtocol.OUTPUT_ENABLE
                        || frame.type() == EcscProtocol.OUTPUT_WRITE) {
                      replaySeen.set(true);
                    } else {
                      second.ack(frame, AckStatus.OK);
                    }
                  }
                }
              });

      SystemCoreConfig config =
          SystemCoreConfig.builder()
              .socketPath(daemon.socket())
              .requestedRole(ClientRole.CONTROLLER)
              .requestTimeout(Duration.ofMillis(500))
              .reconnectDelays(Duration.ofMillis(20), Duration.ofMillis(50))
              .listener(listener)
              .build();
      try (SystemCoreClient client = SystemCoreClient.connect(config)) {
        await(
            () -> client.health().state() == HealthState.CONTROLLING_DISABLED,
            TEST_TIMEOUT,
            "first controller handshake");
        CompletableFuture<Ack> enable = client.enableOutputs();
        check(blockedStatus.await(2, TimeUnit.SECONDS), "status listener entered");
        check(enable.get(2, TimeUnit.SECONDS).ok(), "enable completion bypasses blocked listener");
        check(client.outputsEnabled(), "local output state follows enable ACK");
        check(
            client.writeOutput(0, 1, 0, new byte[] {1, 2, 3, 4})
                .get(2, TimeUnit.SECONDS)
                .ok(),
            "output write ACK");
        long firstGeneration = client.generation();
        CompletableFuture<Ack> disable = client.disableOutputs();
        check(!client.outputsEnabled(), "local disable barrier is immediate");
        disable.cancel(true);
        await(
            () ->
                client.generation() > firstGeneration
                    && client.health().state() == HealthState.CONTROLLING_DISABLED,
            TEST_TIMEOUT,
            "fresh disabled generation after cancelled disable");
        check(!client.outputsEnabled(), "reconnect remains disabled");
        Thread.sleep(450);
        check(!replaySeen.get(), "no enable or output replay after reconnect");
      } finally {
        releaseStatus.countDown();
      }
      joinServer(serverThread, serverFailure);
    }
  }

  private static void testBoundedBackpressure() throws Exception {
    try (FakeDaemon daemon = new FakeDaemon()) {
      AtomicReference<Throwable> serverFailure = new AtomicReference<>();
      Thread serverThread =
          daemon.start(
              serverFailure,
              server -> {
                try (FakeConnection connection = server.accept(TEST_TIMEOUT)) {
                  expectHello(connection, ClientRole.OBSERVER);
                  connection.send(
                      EcscProtocol.HELLO_ACK,
                      0,
                      EcscProtocol.encodeHelloAckForTest(
                          ClientRole.OBSERVER, false, 250, 51, 1, 0xf));
                  // Consume requests without acknowledging them. The client must remain bounded.
                  while (true) {
                    connection.read(TEST_TIMEOUT);
                  }
                } catch (EOFException expected) {
                  // Request timeout closes the bounded generation.
                }
              });
      SystemCoreConfig config =
          SystemCoreConfig.builder()
              .socketPath(daemon.socket())
              .maximumQueuedCommands(2)
              .requestTimeout(Duration.ofMillis(100))
              .reconnectDelays(Duration.ofSeconds(1), Duration.ofSeconds(1))
              .build();
      try (SystemCoreClient client = SystemCoreClient.connect(config)) {
        await(
            () -> client.health().state() == HealthState.OBSERVING,
            TEST_TIMEOUT,
            "observer handshake");
        List<CompletableFuture<Ack>> futures = new ArrayList<>();
        for (int index = 0; index < 32; index++) {
          futures.add(client.clearCounters());
        }
        long immediatelyRejected = futures.stream().filter(CompletableFuture::isCompletedExceptionally).count();
        check(immediatelyRejected > 0, "bounded producer backpressure");
      }
      joinServer(serverThread, serverFailure);
    }
  }

  private static void testDisableRejectionForcesReconnect() throws Exception {
    try (FakeDaemon daemon = new FakeDaemon()) {
      AtomicReference<Throwable> serverFailure = new AtomicReference<>();
      CountDownLatch secondHandshake = new CountDownLatch(1);
      Thread serverThread =
          daemon.start(
              serverFailure,
              server -> {
                try (FakeConnection first = server.accept(TEST_TIMEOUT)) {
                  expectHello(first, ClientRole.CONTROLLER);
                  first.send(
                      EcscProtocol.HELLO_ACK,
                      0,
                      EcscProtocol.encodeHelloAckForTest(
                          ClientRole.CONTROLLER, false, 250, 61, 1, 0xf));
                  while (true) {
                    EcscProtocol.Frame frame = first.read(TEST_TIMEOUT);
                    if (frame.type() == EcscProtocol.HEARTBEAT) {
                      first.ack(frame, AckStatus.OK);
                    } else if (frame.type() == EcscProtocol.OUTPUT_ENABLE) {
                      first.ack(
                          frame,
                          frame.payload()[8] == 1
                              ? AckStatus.OK
                              : AckStatus.INTERNAL_ERROR);
                    }
                  }
                } catch (EOFException expected) {
                  // Every non-OK disable ACK must close the barred generation.
                }
                try (FakeConnection second = server.accept(TEST_TIMEOUT)) {
                  expectHello(second, ClientRole.CONTROLLER);
                  secondHandshake.countDown();
                }
              });
      SystemCoreConfig config =
          SystemCoreConfig.builder()
              .socketPath(daemon.socket())
              .requestedRole(ClientRole.CONTROLLER)
              .requestTimeout(Duration.ofMillis(500))
              .reconnectDelays(Duration.ofMillis(20), Duration.ofMillis(50))
              .build();
      try (SystemCoreClient client = SystemCoreClient.connect(config)) {
        await(
            () -> client.health().state() == HealthState.CONTROLLING_DISABLED,
            TEST_TIMEOUT,
            "controller handshake");
        check(client.enableOutputs().get(2, TimeUnit.SECONDS).ok(), "enable before disable");
        Ack rejected = client.disableOutputs().get(2, TimeUnit.SECONDS);
        check(rejected.status() == AckStatus.INTERNAL_ERROR, "disable rejection returned");
        check(secondHandshake.await(3, TimeUnit.SECONDS), "disable rejection forced reconnect");
      }
      joinServer(serverThread, serverFailure);
    }
  }

  private static byte[] statusPayload(long epoch, boolean outputsEnabled) {
    return EcscProtocol.encodeStatusForTest(
        new Status(
            BusState.OPERATIONAL,
            true,
            outputsEnabled,
            true,
            false,
            false,
            false,
            false,
            false,
            SchedulingMode.STANDARD,
            0,
            false,
            1,
            1,
            0,
            10,
            5,
            0,
            0,
            epoch,
            123,
            1,
            10_000,
            Instant.now()));
  }

  private static void expectHello(FakeConnection connection, ClientRole role)
      throws Exception {
    EcscProtocol.Frame hello = connection.read(TEST_TIMEOUT);
    check(hello.type() == EcscProtocol.HELLO && hello.requestId() == 0, "HELLO framing");
    byte[] payload = hello.payload();
    check(payload.length == 12 && payload[0] == role.wireValue(), "HELLO role");
  }

  private static boolean supportsUnixSockets() {
    try (ServerSocketChannel probe = ServerSocketChannel.open(StandardProtocolFamily.UNIX)) {
      return probe.isOpen();
    } catch (UnsupportedOperationException | IOException unavailable) {
      return false;
    }
  }

  private static void assertHex(Properties vectors, String key, byte[] actual) {
    String expected = vectors.getProperty(key);
    check(expected != null, "missing golden vector " + key);
    check(expected.equals(HEX.formatHex(actual)), "golden vector mismatch: " + key);
  }

  private static void await(BooleanSupplier condition, Duration timeout, String message)
      throws Exception {
    long deadline = System.nanoTime() + timeout.toNanos();
    while (!condition.getAsBoolean()) {
      if (System.nanoTime() - deadline >= 0) {
        throw new AssertionError("timed out: " + message);
      }
      Thread.sleep(2);
    }
  }

  private static void awaitLatchUnchecked(CountDownLatch latch) {
    try {
      latch.await();
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
    }
  }

  private static void joinServer(
      Thread serverThread, AtomicReference<Throwable> failure) throws Exception {
    serverThread.join(TEST_TIMEOUT.toMillis());
    check(!serverThread.isAlive(), "fake daemon stopped");
    if (failure.get() != null) {
      throw new AssertionError("fake daemon failed", failure.get());
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static <T extends Throwable> void expectThrows(
      Class<T> expected, ThrowingRunnable action, String message) throws Exception {
    try {
      action.run();
    } catch (Throwable failure) {
      if (expected.isInstance(failure)) {
        return;
      }
      throw new AssertionError(message + " threw " + failure, failure);
    }
    throw new AssertionError(message + " did not throw " + expected.getSimpleName());
  }

  @FunctionalInterface
  private interface ThrowingRunnable {
    void run() throws Exception;
  }

  @FunctionalInterface
  private interface ServerScript {
    void run(FakeDaemon server) throws Exception;
  }

  private static final class FakeDaemon implements AutoCloseable {
    private final Path directory;
    private final Path socket;
    private final ServerSocketChannel server;

    FakeDaemon() throws IOException {
      directory = Files.createTempDirectory("ecsc-j");
      socket = directory.resolve("d.sock");
      server = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
      server.configureBlocking(false);
      server.bind(UnixDomainSocketAddress.of(socket));
    }

    Path socket() {
      return socket;
    }

    Thread start(AtomicReference<Throwable> failure, ServerScript script) {
      Thread thread =
          new Thread(
              () -> {
                try {
                  script.run(this);
                } catch (Throwable error) {
                  failure.set(error);
                }
              },
              "ec-systemcore-java-fake-daemon");
      thread.setDaemon(true);
      thread.start();
      return thread;
    }

    FakeConnection accept(Duration timeout) throws Exception {
      long deadline = System.nanoTime() + timeout.toNanos();
      while (true) {
        SocketChannel channel = server.accept();
        if (channel != null) {
          channel.configureBlocking(false);
          return new FakeConnection(channel);
        }
        if (System.nanoTime() - deadline >= 0) {
          throw new AssertionError("fake daemon accept timed out");
        }
        Thread.sleep(1);
      }
    }

    @Override
    public void close() throws IOException {
      server.close();
      Files.deleteIfExists(socket);
      Files.deleteIfExists(directory);
    }
  }

  private static final class FakeConnection implements AutoCloseable {
    private final SocketChannel channel;
    private final EcscProtocol.FrameDecoder decoder = new EcscProtocol.FrameDecoder();
    private final ArrayDeque<EcscProtocol.Frame> ready = new ArrayDeque<>();
    private final ByteBuffer input = ByteBuffer.allocate(4096);

    FakeConnection(SocketChannel channel) {
      this.channel = channel;
    }

    EcscProtocol.Frame read(Duration timeout) throws Exception {
      EcscProtocol.Frame frame = tryRead(timeout);
      if (frame == null) {
        throw new AssertionError("fake daemon read timed out");
      }
      return frame;
    }

    EcscProtocol.Frame tryRead(Duration timeout) throws Exception {
      long deadline = System.nanoTime() + timeout.toNanos();
      while (true) {
        if (!ready.isEmpty()) {
          return ready.remove();
        }
        input.clear();
        int count = channel.read(input);
        if (count < 0) {
          throw new EOFException("client closed fake daemon connection");
        }
        if (count > 0) {
          input.flip();
          ready.addAll(decoder.feed(input));
          continue;
        }
        if (System.nanoTime() - deadline >= 0) {
          return null;
        }
        Thread.sleep(1);
      }
    }

    void ack(EcscProtocol.Frame request, AckStatus status) throws Exception {
      send(
          EcscProtocol.ACK,
          request.requestId(),
          EcscProtocol.encodeAckForTest(status, 0));
    }

    void send(int type, long requestId, byte[] payload) throws Exception {
      write(EcscProtocol.encodeFrame(type, requestId, payload));
    }

    void sendFragmented(int type, long requestId, byte[] payload, int fragmentBytes)
        throws Exception {
      ByteBuffer frame = EcscProtocol.encodeFrame(type, requestId, payload);
      while (frame.hasRemaining()) {
        int length = Math.min(fragmentBytes, frame.remaining());
        ByteBuffer fragment = frame.slice();
        fragment.limit(length);
        write(fragment);
        frame.position(frame.position() + length);
      }
    }

    void sendTogether(ByteBuffer... frames) throws Exception {
      int total = 0;
      for (ByteBuffer frame : frames) {
        total = Math.addExact(total, frame.remaining());
      }
      ByteBuffer combined = ByteBuffer.allocate(total);
      for (ByteBuffer frame : frames) {
        combined.put(frame);
      }
      combined.flip();
      write(combined);
    }

    private void write(ByteBuffer bytes) throws Exception {
      long deadline = System.nanoTime() + TEST_TIMEOUT.toNanos();
      while (bytes.hasRemaining()) {
        int count = channel.write(bytes);
        if (count < 0) {
          throw new EOFException("client closed during fake daemon write");
        }
        if (count == 0) {
          if (System.nanoTime() - deadline >= 0) {
            throw new AssertionError("fake daemon write timed out");
          }
          Thread.sleep(1);
        }
      }
    }

    @Override
    public void close() throws IOException {
      channel.close();
    }
  }
}
