package ec_systemcore;

import java.io.EOFException;
import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.BitSet;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import ec_systemcore.SystemCoreTypes.Ack;
import ec_systemcore.SystemCoreTypes.AckStatus;
import ec_systemcore.SystemCoreTypes.BusInfo;
import ec_systemcore.SystemCoreTypes.ClientRole;
import ec_systemcore.SystemCoreTypes.Health;
import ec_systemcore.SystemCoreTypes.HealthState;
import ec_systemcore.SystemCoreTypes.OutputsDisabled;
import ec_systemcore.SystemCoreTypes.PdoAddress;
import ec_systemcore.SystemCoreTypes.PdoInput;
import ec_systemcore.SystemCoreTypes.Status;

/**
 * Nonblocking, reconnecting ECSC version 1 client for robot code.
 *
 * <p>Every transport generation starts disabled. Commands are tagged with both generation and
 * epoch, never replayed after a disconnect, and failed if either value changes before they reach
 * the socket. A successful explicit disable creates a reconnect barrier; a later enable is not
 * accepted until a new HELLO handshake establishes a fresh generation.
 */
public final class SystemCoreClient implements AutoCloseable {
  private static final int READ_BUFFER_BYTES = 8192;
  private static final int CALLBACK_QUEUE_LIMIT = 512;
  private static final int COMPLETION_THREADS = 2;
  private static final int HEARTBEAT_FRAME_BYTES =
      EcscProtocol.LENGTH_PREFIX_BYTES + EcscProtocol.ENVELOPE_BYTES + Long.BYTES;
  private static final Duration PDO_ASSEMBLY_MAX_AGE = Duration.ofSeconds(2);

  private final SystemCoreConfig config;
  private final SystemCoreListener listener;
  private final ArrayBlockingQueue<Command> commands;
  private final AtomicBoolean running = new AtomicBoolean();
  private final AtomicBoolean closed = new AtomicBoolean();
  private final AtomicLong generationCounter = new AtomicLong();
  private final AtomicReference<SessionView> session =
      new AtomicReference<>(SessionView.disconnected(0));
  private final AtomicReference<Health> health =
      new AtomicReference<>(
          new Health(HealthState.STOPPED, "client has not started", null, 0, Instant.now()));
  private final AtomicReference<Status> latestStatus = new AtomicReference<>();
  private final AtomicReference<Map<Integer, BusInfo>> latestBuses =
      new AtomicReference<>(Map.of());
  private final ConcurrentHashMap<PdoAddress, PdoInput> latestInputs =
      new ConcurrentHashMap<>();
  private final AtomicReference<OutputsDisabled> latestOutputsDisabled =
      new AtomicReference<>();
  private final ThreadPoolExecutor callbackSerial;
  private final ThreadPoolExecutor completionExecutor;
  private final Semaphore futureSlots;
  private final Executor configuredCallbackExecutor;
  private final Object lifecycleLock = new Object();

  private volatile Thread worker;
  private volatile SocketChannel activeChannel;
  private volatile long lastSeenEpoch;
  private long retainedPdoBytes;

  /** Creates a stopped client. Call {@link #start()} once construction is complete. */
  public SystemCoreClient(SystemCoreConfig config) {
    this.config = Objects.requireNonNull(config, "config");
    listener = config.listener();
    commands = new ArrayBlockingQueue<>(config.maximumQueuedCommands());
    int maximumOutstandingFutures = Math.multiplyExact(config.maximumQueuedCommands(), 2);
    futureSlots = new Semaphore(maximumOutstandingFutures);
    configuredCallbackExecutor = config.callbackExecutor();
    callbackSerial =
        new ThreadPoolExecutor(
            1,
            1,
            0,
            TimeUnit.MILLISECONDS,
            new ArrayBlockingQueue<>(CALLBACK_QUEUE_LIMIT),
            daemonThreadFactory("ec-systemcore-callback"),
            new ThreadPoolExecutor.AbortPolicy());
    completionExecutor =
        new ThreadPoolExecutor(
            COMPLETION_THREADS,
            COMPLETION_THREADS,
            0,
            TimeUnit.MILLISECONDS,
            new ArrayBlockingQueue<>(maximumOutstandingFutures),
            daemonThreadFactory("ec-systemcore-completion"),
            new ThreadPoolExecutor.AbortPolicy());
  }

  /** Creates and starts a client. */
  public static SystemCoreClient connect(SystemCoreConfig config) {
    return new SystemCoreClient(config).start();
  }

  /** Starts the single socket worker. This method is idempotent and does not block on connection. */
  public SystemCoreClient start() {
    synchronized (lifecycleLock) {
      if (closed.get()) {
        throw new IllegalStateException("client is closed");
      }
      if (running.compareAndSet(false, true)) {
        Thread thread = daemonThreadFactory("ec-systemcore-io").newThread(this::runWorker);
        worker = thread;
        thread.start();
      }
    }
    return this;
  }

  public boolean isRunning() {
    return running.get();
  }

  public Health health() {
    return health.get();
  }

  public Optional<Status> latestStatus() {
    return Optional.ofNullable(latestStatus.get());
  }

  public Map<Integer, BusInfo> latestBuses() {
    return latestBuses.get();
  }

  /** Returns immutable, generation-local complete PDO images for nonblocking robot-loop polling. */
  public Map<PdoAddress, PdoInput> latestInputs() {
    return Map.copyOf(latestInputs);
  }

  public Optional<PdoInput> latestInput(int busIndex, int subDeviceIndex) {
    return Optional.ofNullable(latestInputs.get(new PdoAddress(busIndex, subDeviceIndex)));
  }

  public Optional<OutputsDisabled> latestOutputsDisabled() {
    return Optional.ofNullable(latestOutputsDisabled.get());
  }

  public Optional<ClientRole> grantedRole() {
    SessionView view = session.get();
    return view.ready ? Optional.of(view.role) : Optional.empty();
  }

  public long generation() {
    return session.get().generation;
  }

  public Optional<String> epochUnsigned() {
    SessionView view = session.get();
    return view.ready ? Optional.of(Long.toUnsignedString(view.epoch)) : Optional.empty();
  }

  public boolean outputsEnabled() {
    SessionView view = session.get();
    return view.ready
        && view.role == ClientRole.CONTROLLER
        && view.outputsEnabled
        && !view.disableBarrier;
  }

  /**
   * Requests output enable or disable without blocking the caller.
   *
   * <p>Disable immediately raises a local barrier. An enable submitted immediately afterward is
   * failed locally and cannot cross the daemon's disable/connection-close boundary.
   */
  public CompletableFuture<Ack> setOutputsEnabled(boolean enabled) {
    SessionView captured;
    if (enabled) {
      captured = session.get();
      if (!controllerReady(captured)) {
        return failedFuture(new IllegalStateException("controller session is not ready"));
      }
      if (captured.disableBarrier) {
        return failedFuture(
            new IllegalStateException("fresh connection required after output disable"));
      }
    } else {
      while (true) {
        captured = session.get();
        if (!controllerReady(captured)) {
          return failedFuture(new IllegalStateException("controller session is not ready"));
        }
        if (captured.disableBarrier) {
          return failedFuture(new IllegalStateException("output disable is already in progress"));
        }
        SessionView barred = captured.withDisableBarrier();
        if (session.compareAndSet(captured, barred)) {
          captured = barred;
          break;
        }
      }
    }
    return submit(
        captured,
        EcscProtocol.OUTPUT_ENABLE,
        EcscProtocol.encodeOutputEnable(captured.epoch, enabled),
        enabled ? CommandKind.ENABLE : CommandKind.DISABLE);
  }

  public CompletableFuture<Ack> enableOutputs() {
    return setOutputsEnabled(true);
  }

  public CompletableFuture<Ack> disableOutputs() {
    return setOutputsEnabled(false);
  }

  /**
   * Queues one bounded process-output write.
   *
   * <p>The daemon requires writes to continue within its configured output-command timeout even
   * when heartbeats continue. This method intentionally does not replay or periodically resend data.
   */
  public CompletableFuture<Ack> writeOutput(
      int busIndex, int subDeviceIndex, long offset, byte[] data) {
    SessionView captured = session.get();
    if (!controllerReady(captured) || !captured.outputsEnabled || captured.disableBarrier) {
      return failedFuture(new IllegalStateException("outputs are not enabled in this generation"));
    }
    byte[] payload =
        EcscProtocol.encodeOutputWrite(
            captured.epoch,
            busIndex,
            subDeviceIndex,
            offset,
            Objects.requireNonNull(data, "data"));
    return submit(captured, EcscProtocol.OUTPUT_WRITE, payload, CommandKind.WRITE);
  }

  /**
   * Enqueues an output image without allocating a command future.
   *
   * <p>This is the preferred hot-loop API. It returns {@code false} when the current generation
   * is not enabled or the bounded queue is full. The daemon still acknowledges the write; a
   * rejected acknowledgement forces a fail-closed reconnect because no future exists to report
   * the rejection.
   */
  public boolean tryWriteOutput(
      int busIndex, int subDeviceIndex, long offset, byte[] data) {
    SessionView captured = session.get();
    if (!running.get()
        || !controllerReady(captured)
        || !captured.outputsEnabled
        || captured.disableBarrier
        || commands.remainingCapacity() == 0) {
      return false;
    }
    byte[] payload =
        EcscProtocol.encodeOutputWrite(
            captured.epoch,
            busIndex,
            subDeviceIndex,
            offset,
            Objects.requireNonNull(data, "data"));
    return commands.offer(
        new Command(
            captured.generation,
            captured.epoch,
            EcscProtocol.OUTPUT_WRITE,
            payload,
            CommandKind.WRITE,
            null));
  }

  public CompletableFuture<Ack> clearCounters() {
    SessionView captured = requireReadySession();
    if (captured == null) {
      return failedFuture(new IllegalStateException("session is not ready"));
    }
    return submit(captured, EcscProtocol.CLEAR_COUNTERS, new byte[0], CommandKind.GENERIC);
  }

  public CompletableFuture<Ack> subscribeInputs(boolean enabled, int periodMillis) {
    SessionView captured = requireReadySession();
    if (captured == null) {
      return failedFuture(new IllegalStateException("session is not ready"));
    }
    return submit(
        captured,
        EcscProtocol.SUBSCRIBE_INPUTS,
        EcscProtocol.encodeSubscription(enabled, periodMillis),
        CommandKind.GENERIC);
  }

  public CompletableFuture<Ack> unlockAdapter(int busIndex) {
    SessionView captured = session.get();
    if (!controllerReady(captured) || captured.outputsEnabled || captured.disableBarrier) {
      return failedFuture(
          new IllegalStateException("adapter unlock requires a disabled controller session"));
    }
    return submit(
        captured,
        EcscProtocol.ADAPTER_UNLOCK,
        EcscProtocol.encodeAdapterUnlock(captured.epoch, busIndex),
        CommandKind.GENERIC);
  }

  public CompletableFuture<Ack> rescanAdapters() {
    SessionView captured = requireReadySession();
    if (captured == null) {
      return failedFuture(new IllegalStateException("session is not ready"));
    }
    return submit(captured, EcscProtocol.ADAPTER_RESCAN, new byte[0], CommandKind.GENERIC);
  }

  public CompletableFuture<Ack> releaseControl() {
    while (true) {
      SessionView captured = session.get();
      if (!controllerReady(captured)) {
        return failedFuture(new IllegalStateException("controller session is not ready"));
      }
      if (captured.disableBarrier) {
        return failedFuture(new IllegalStateException("control release is already in progress"));
      }
      SessionView barred = captured.withDisableBarrier();
      if (session.compareAndSet(captured, barred)) {
        return submit(
            barred,
            EcscProtocol.RELEASE_CONTROL,
            EcscProtocol.encodeEpoch(barred.epoch),
            CommandKind.RELEASE);
      }
    }
  }

  /** Safely invalidates the active generation and asks the worker to reconnect. */
  public void requestReconnect() {
    closeActiveChannel();
  }

  @Override
  public void close() {
    Thread currentWorker;
    synchronized (lifecycleLock) {
      if (!closed.compareAndSet(false, true)) {
        return;
      }
      running.set(false);
      currentWorker = worker;
    }
    closeActiveChannel();
    if (currentWorker != null) {
      currentWorker.interrupt();
      if (currentWorker != Thread.currentThread()) {
        try {
          currentWorker.join(2000);
        } catch (InterruptedException interrupted) {
          Thread.currentThread().interrupt();
        }
      }
    }
    failQueuedCommands(new IllegalStateException("client is closed"));
    updateHealth(HealthState.STOPPED, "client stopped", null, session.get().generation);
    callbackSerial.shutdown();
    completionExecutor.shutdown();
  }

  private CompletableFuture<Ack> submit(
      SessionView captured, int type, byte[] payload, CommandKind kind) {
    if (!futureSlots.tryAcquire()) {
      if (kind == CommandKind.DISABLE || kind == CommandKind.RELEASE) {
        requestReconnect();
      }
      return failedFuture(
          new RejectedExecutionException("too many outstanding command futures"));
    }
    CompletableFuture<Ack> result =
        new IsolatedFuture(
            futureSlots,
            kind == CommandKind.DISABLE || kind == CommandKind.RELEASE
                ? this::requestReconnect
                : null);
    Command command =
        new Command(captured.generation, captured.epoch, type, payload.clone(), kind, result);
    if (!running.get()) {
      result.completeExceptionally(new IllegalStateException("client is not running"));
      return result;
    }
    if (!commands.offer(command)) {
      result.completeExceptionally(new RejectedExecutionException("client command queue is full"));
      if (kind == CommandKind.DISABLE || kind == CommandKind.RELEASE) {
        requestReconnect();
      }
    }
    return result;
  }

  private SessionView requireReadySession() {
    SessionView view = session.get();
    return view.ready ? view : null;
  }

  private static boolean controllerReady(SessionView view) {
    return view.ready && view.role == ClientRole.CONTROLLER;
  }

  private void runWorker() {
    long reconnectDelayMillis = config.minimumReconnectDelay().toMillis();
    try {
      while (running.get()) {
        long generation = generationCounter.incrementAndGet();
        boolean established = false;
        try {
          runSession(generation);
        } catch (Exception failure) {
          if (running.get()) {
            updateHealth(
                HealthState.DEGRADED,
                safeMessage(failure, "connection lost"),
                failure,
                generation);
          }
        } finally {
          SessionView ending = session.get();
          established = ending.generation == generation && ending.ready;
          activeChannel = null;
          invalidateSession(generation, new IOException("transport generation ended"));
        }
        if (!running.get()) {
          break;
        }
        if (established) {
          reconnectDelayMillis = config.minimumReconnectDelay().toMillis();
        }
        try {
          Thread.sleep(reconnectDelayMillis);
        } catch (InterruptedException interrupted) {
          if (!running.get()) {
            break;
          }
        }
        long maximumDelayMillis = config.maximumReconnectDelay().toMillis();
        reconnectDelayMillis =
            reconnectDelayMillis >= maximumDelayMillis / 2
                ? maximumDelayMillis
                : Math.min(maximumDelayMillis, reconnectDelayMillis * 2);
      }
    } finally {
      running.set(false);
    }
  }

  private void runSession(long generation) throws IOException, ProtocolException {
    updateHealth(HealthState.CONNECTING, "connecting to daemon", null, generation);
    SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX);
    activeChannel = channel;
    Transport transport = null;
    try {
      channel.configureBlocking(false);
      connectWithDeadline(channel);
      transport = new Transport(config);
      latestStatus.set(null);
      latestBuses.set(Map.of());
      latestInputs.clear();
      retainedPdoBytes = 0;
      session.set(SessionView.handshaking(generation));
      updateHealth(HealthState.HANDSHAKING, "waiting for HELLO_ACK", null, generation);
      transport.enqueue(
          EcscProtocol.encodeFrame(
              EcscProtocol.HELLO,
              0,
              EcscProtocol.encodeHello(
                  config.requestedRole(),
                  config.subscribeInputs(),
                  config.inputPeriodMillis(),
                  lastSeenEpoch)));
      transport.helloDeadlineNanos = deadlineAfter(config.requestTimeout());

      ByteBuffer readBuffer = ByteBuffer.allocate(READ_BUFFER_BYTES);
      while (running.get() && activeChannel == channel && channel.isOpen()) {
        processCommands(transport, generation);
        queueHeartbeatIfDue(transport, generation);
        transport.flush(channel);
        readAvailable(channel, readBuffer, transport, generation);
        expireRequests(transport, generation);
        transport.assemblies.expireOlderThan(PDO_ASSEMBLY_MAX_AGE);
        if (!transport.handshakeComplete
            && System.nanoTime() - transport.helloDeadlineNanos >= 0) {
          throw new IOException("HELLO_ACK timed out");
        }
        if (transport.forceReconnect) {
          throw new IOException("daemon safety state requires a fresh connection");
        }
        if (!transport.didWork) {
          try {
            Thread.sleep(2);
          } catch (InterruptedException interrupted) {
            if (!running.get()) {
              return;
            }
          }
        }
        transport.didWork = false;
      }
      if (running.get()) {
        throw new EOFException("daemon socket closed");
      }
    } finally {
      if (transport != null) {
        transport.failPending(new IOException("transport generation ended"), this);
      }
      try {
        channel.close();
      } catch (IOException ignored) {
        // The original transport failure is more useful.
      }
    }
  }

  private void connectWithDeadline(SocketChannel channel) throws IOException {
    if (channel.connect(UnixDomainSocketAddress.of(config.socketPath()))) {
      return;
    }
    long deadline = deadlineAfter(config.requestTimeout());
    try (Selector selector = Selector.open()) {
      channel.register(selector, SelectionKey.OP_CONNECT);
      while (running.get() && activeChannel == channel && channel.isOpen()) {
        long remainingNanos = deadline - System.nanoTime();
        if (remainingNanos <= 0) {
          throw new IOException("daemon connection timed out");
        }
        long waitMillis =
            Math.max(
                1,
                Math.min(100, TimeUnit.NANOSECONDS.toMillis(remainingNanos)));
        selector.select(waitMillis);
        if (channel.finishConnect()) {
          return;
        }
      }
    }
    throw new IOException("daemon connection was cancelled");
  }

  private void processCommands(Transport transport, long generation) throws ProtocolException {
    for (int count = 0; count < config.maximumQueuedCommands(); count++) {
      if (transport.pending.size() >= config.maximumQueuedCommands()) {
        return;
      }
      Command command = commands.poll();
      if (command == null) {
        return;
      }
      SessionView current = session.get();
      if (!transport.handshakeComplete
          || !current.ready
          || command.generation != generation
          || command.generation != current.generation
          || command.epoch != current.epoch) {
        completeExceptionally(
            command.result,
            new IllegalStateException("command invalidated by connection or epoch change"));
        if (command.kind == CommandKind.DISABLE || command.kind == CommandKind.RELEASE) {
          transport.forceReconnect = true;
        }
        continue;
      }
      if (command.result != null && command.result.isCancelled()) {
        retire(command.result);
        if (command.kind == CommandKind.DISABLE || command.kind == CommandKind.RELEASE) {
          transport.forceReconnect = true;
        }
        continue;
      }
      long requestId = transport.nextRequestId();
      ByteBuffer encoded = EcscProtocol.encodeFrame(command.type, requestId, command.payload);
      if (!transport.canEnqueueCommand(encoded.remaining())) {
        completeExceptionally(
            command.result, new RejectedExecutionException("outbound byte limit reached"));
        if (command.kind == CommandKind.DISABLE || command.kind == CommandKind.RELEASE) {
          transport.forceReconnect = true;
        }
        continue;
      }
      PendingRequest pending =
          new PendingRequest(
              command.kind,
              command.result,
              deadlineAfter(config.requestTimeout()),
              generation,
              command.epoch);
      transport.pending.put(requestId, pending);
      transport.enqueue(encoded);
      transport.didWork = true;
    }
  }

  private void queueHeartbeatIfDue(Transport transport, long generation)
      throws ProtocolException {
    if (!transport.handshakeComplete || System.nanoTime() - transport.nextHeartbeatNanos < 0) {
      return;
    }
    SessionView current = session.get();
    if (!controllerReady(current)
        || current.generation != generation
        || transport.heartbeatRequestId != 0) {
      return;
    }
    long requestId = transport.nextRequestId();
    ByteBuffer frame =
        EcscProtocol.encodeFrame(
            EcscProtocol.HEARTBEAT, requestId, EcscProtocol.encodeEpoch(current.epoch));
    if (!transport.canEnqueue(frame.remaining())) {
      throw new ProtocolException("unable to queue safety heartbeat");
    }
    transport.pending.put(
        requestId,
        new PendingRequest(
            CommandKind.HEARTBEAT,
            null,
            deadlineAfterMillis(
                Math.min(
                    config.requestTimeout().toMillis(),
                    transport.heartbeatAckTimeoutMillis)),
            generation,
            current.epoch));
    transport.heartbeatRequestId = requestId;
    transport.enqueue(frame);
    transport.nextHeartbeatNanos = deadlineAfterMillis(transport.heartbeatIntervalMillis);
    transport.didWork = true;
  }

  private void readAvailable(
      SocketChannel channel, ByteBuffer readBuffer, Transport transport, long generation)
      throws IOException, ProtocolException {
    while (true) {
      readBuffer.clear();
      int count = channel.read(readBuffer);
      if (count < 0) {
        throw new EOFException("daemon closed the socket");
      }
      if (count == 0) {
        return;
      }
      transport.didWork = true;
      readBuffer.flip();
      List<EcscProtocol.Frame> frames = transport.decoder.feed(readBuffer);
      for (EcscProtocol.Frame frame : frames) {
        processFrame(frame, transport, generation);
      }
    }
  }

  private void processFrame(EcscProtocol.Frame frame, Transport transport, long generation)
      throws ProtocolException {
    if (!transport.handshakeComplete) {
      if (frame.type() != EcscProtocol.HELLO_ACK || frame.requestId() != 0) {
        throw new ProtocolException("expected HELLO_ACK as the first daemon frame");
      }
      EcscProtocol.HelloAck hello = EcscProtocol.decodeHelloAck(frame.payload());
      if (config.requestedRole() == ClientRole.OBSERVER
          && hello.grantedRole() != ClientRole.OBSERVER) {
        throw new ProtocolException("daemon granted a role broader than requested");
      }
      if (hello.grantedRole() == ClientRole.CONTROLLER && hello.outputsEnabled()) {
        throw new ProtocolException("new controller generation unexpectedly has outputs enabled");
      }
      transport.handshakeComplete = true;
      transport.heartbeatIntervalMillis =
          Math.max(10, Math.min(100, hello.heartbeatTimeoutMillis() / 3));
      transport.heartbeatAckTimeoutMillis =
          Math.max(10, hello.heartbeatTimeoutMillis() / 2);
      transport.nextHeartbeatNanos =
          deadlineAfterMillis(transport.heartbeatIntervalMillis);
      lastSeenEpoch = hello.epoch();
      boolean observableOutputs =
          hello.grantedRole() == ClientRole.OBSERVER && hello.outputsEnabled();
      session.set(
          new SessionView(
              generation,
              true,
              hello.grantedRole(),
              hello.epoch(),
              observableOutputs,
              false));
      HealthState state =
          hello.grantedRole() == ClientRole.CONTROLLER
              ? HealthState.CONTROLLING_DISABLED
              : HealthState.OBSERVING;
      String message =
          config.requestedRole() == ClientRole.CONTROLLER
                  && hello.grantedRole() != ClientRole.CONTROLLER
              ? "daemon granted observer role; outputs unavailable"
              : "HELLO handshake complete";
      updateHealth(state, message, null, generation);
      return;
    }

    if (frame.type() == EcscProtocol.HELLO_ACK || frame.requestId() != 0
        && frame.type() != EcscProtocol.ACK && frame.type() != EcscProtocol.ERROR) {
      throw new ProtocolException("daemon event/request ID pairing is invalid");
    }
    switch (frame.type()) {
      case EcscProtocol.ACK, EcscProtocol.ERROR ->
          processAck(frame, transport, generation);
      case EcscProtocol.STATUS -> processStatus(frame, transport, generation);
      case EcscProtocol.BUS_INFO -> processBusInfo(frame, transport, generation);
      case EcscProtocol.PDO_INPUT -> processPdo(frame, transport, generation);
      case EcscProtocol.OUTPUTS_DISABLED ->
          processOutputsDisabled(frame, transport, generation);
      default -> throw new ProtocolException("unexpected daemon message type " + frame.type());
    }
  }

  private void processAck(EcscProtocol.Frame frame, Transport transport, long generation)
      throws ProtocolException {
    if (frame.requestId() == 0) {
      throw new ProtocolException("ACK/ERROR request ID must be non-zero");
    }
    PendingRequest pending = transport.pending.remove(frame.requestId());
    if (pending == null) {
      throw new ProtocolException("unsolicited or expired acknowledgement");
    }
    if (transport.heartbeatRequestId == frame.requestId()) {
      transport.heartbeatRequestId = 0;
    }
    Ack ack = EcscProtocol.decodeAck(frame.payload());
    if (pending.generation != generation || pending.epoch != session.get().epoch) {
      completeExceptionally(
          pending.result,
          new IllegalStateException("acknowledgement belongs to an invalidated generation"));
      transport.forceReconnect = true;
      return;
    }
    if (pending.kind == CommandKind.HEARTBEAT) {
      if (!ack.ok()) {
        throw new ProtocolException("heartbeat rejected with " + ack.status());
      }
      return;
    }

    if (pending.kind == CommandKind.ENABLE && ack.ok()) {
      updateOutputState(generation, pending.epoch, true);
    }
    if (pending.kind == CommandKind.WRITE && pending.result == null && !ack.ok()) {
      transport.forceReconnect = true;
    }
    if (pending.kind == CommandKind.DISABLE || pending.kind == CommandKind.RELEASE) {
      if (ack.ok()) {
        updateOutputState(generation, pending.epoch, false);
      }
      transport.forceReconnect = true;
    }
    if (ack.status() == AckStatus.STALE_EPOCH
        || ack.status() == AckStatus.UNAUTHORIZED
        || ack.status() == AckStatus.DISABLED) {
      transport.forceReconnect = true;
    }
    complete(pending.result, ack);
  }

  private void processStatus(
      EcscProtocol.Frame frame, Transport transport, long generation) throws ProtocolException {
    Status status = EcscProtocol.decodeStatus(frame.payload(), Instant.now());
    SessionView current = session.get();
    if (current.generation != generation || status.epoch() != current.epoch) {
      transport.forceReconnect = true;
      return;
    }
    latestStatus.set(status);
    dispatchListener(callback -> callback.onStatus(status));
  }

  private void processBusInfo(
      EcscProtocol.Frame frame, Transport transport, long generation) throws ProtocolException {
    BusInfo busInfo = EcscProtocol.decodeBusInfo(frame.payload());
    SessionView current = session.get();
    if (current.generation != generation || busInfo.epoch() != current.epoch) {
      transport.forceReconnect = true;
      return;
    }
    Map<Integer, BusInfo> updated = new HashMap<>(latestBuses.get());
    if (!updated.containsKey(busInfo.busIndex())
        && updated.size() >= config.maximumTrackedBuses()) {
      throw new ProtocolException("retained BUS_INFO limit exceeded");
    }
    updated.put(busInfo.busIndex(), busInfo);
    latestBuses.set(Map.copyOf(updated));
    dispatchListener(callback -> callback.onBusInfo(busInfo));
  }

  private void processPdo(
      EcscProtocol.Frame frame, Transport transport, long generation) throws ProtocolException {
    EcscProtocol.PdoChunk chunk =
        EcscProtocol.decodePdoChunk(frame.payload(), config.maximumPdoImageBytes());
    SessionView current = session.get();
    if (current.generation != generation || chunk.epoch() != current.epoch) {
      transport.forceReconnect = true;
      return;
    }
    Optional<PdoInput> completed = transport.assemblies.accept(chunk, Instant.now());
    if (completed.isPresent()) {
      PdoInput input = completed.orElseThrow();
      PdoAddress address = new PdoAddress(input.busIndex(), input.subDeviceIndex());
      if (!latestInputs.containsKey(address)
          && latestInputs.size() >= config.maximumTrackedPdoInputs()) {
        throw new ProtocolException("retained PDO input limit exceeded");
      }
      PdoInput previous = latestInputs.get(address);
      long nextRetainedBytes =
          retainedPdoBytes
              - (previous == null ? 0 : previous.size())
              + input.size();
      if (nextRetainedBytes > config.maximumRetainedPdoBytes()) {
        throw new ProtocolException("retained PDO byte limit exceeded");
      }
      latestInputs.put(address, input);
      retainedPdoBytes = nextRetainedBytes;
      dispatchListener(callback -> callback.onPdoInput(input));
    }
  }

  private void processOutputsDisabled(
      EcscProtocol.Frame frame, Transport transport, long generation) throws ProtocolException {
    OutputsDisabled disabled =
        EcscProtocol.decodeOutputsDisabled(frame.payload(), Instant.now());
    updateOutputState(generation, session.get().epoch, false);
    lastSeenEpoch = disabled.epoch();
    latestOutputsDisabled.set(disabled);
    dispatchListener(callback -> callback.onOutputsDisabled(disabled), true);
    transport.forceReconnect = true;
  }

  private void expireRequests(Transport transport, long generation) throws IOException {
    long now = System.nanoTime();
    List<Long> expired = new ArrayList<>();
    for (Map.Entry<Long, PendingRequest> entry : transport.pending.entrySet()) {
      if (now - entry.getValue().deadlineNanos >= 0) {
        expired.add(entry.getKey());
      }
    }
    if (expired.isEmpty()) {
      return;
    }
    for (long requestId : expired) {
      PendingRequest pending = transport.pending.remove(requestId);
      if (pending != null) {
        completeExceptionally(
            pending.result,
            new IOException("daemon request timed out in generation " + generation));
      }
    }
    throw new IOException("daemon request timed out");
  }

  private void updateOutputState(long generation, long epoch, boolean enabled) {
    while (true) {
      SessionView current = session.get();
      if (current.generation != generation || current.epoch != epoch) {
        return;
      }
      SessionView updated =
          new SessionView(
              current.generation,
              current.ready,
              current.role,
              current.epoch,
              enabled,
              current.disableBarrier);
      if (session.compareAndSet(current, updated)) {
        updateHealth(
            enabled ? HealthState.CONTROLLING_ENABLED : HealthState.CONTROLLING_DISABLED,
            enabled ? "outputs enabled" : "outputs disabled",
            null,
            generation);
        return;
      }
    }
  }

  private void invalidateSession(long generation, Throwable failure) {
    while (true) {
      SessionView current = session.get();
      if (current.generation > generation) {
        break;
      }
      if (session.compareAndSet(current, SessionView.disconnected(generation))) {
        break;
      }
    }
    latestStatus.set(null);
    latestBuses.set(Map.of());
    latestInputs.clear();
    retainedPdoBytes = 0;
    failQueuedCommands(failure);
  }

  private void failQueuedCommands(Throwable failure) {
    Command command;
    while ((command = commands.poll()) != null) {
      completeExceptionally(command.result, failure);
    }
  }

  private void closeActiveChannel() {
    SocketChannel channel = activeChannel;
    if (channel != null) {
      try {
        channel.close();
      } catch (IOException ignored) {
        // Closing is best effort; the worker also owns the channel lifetime.
      }
    }
  }

  private void updateHealth(
      HealthState state, String message, Throwable cause, long generation) {
    Health updated = new Health(state, message, cause, generation, Instant.now());
    Health previous = health.getAndSet(updated);
    if (previous.state() != updated.state()
        || !previous.message().equals(updated.message())
        || previous.generation() != updated.generation()) {
      dispatchListener(callback -> callback.onHealthChanged(updated), true);
    }
  }

  private void dispatchListener(Consumer<SystemCoreListener> invocation) {
    dispatchListener(invocation, false);
  }

  private void dispatchListener(
      Consumer<SystemCoreListener> invocation, boolean critical) {
    Runnable safe =
        () -> {
          try {
            invocation.accept(listener);
          } catch (Throwable ignored) {
            // User callbacks must never stop heartbeats, decoding, or reconnect.
          }
        };
    Runnable queued =
        () -> {
          if (configuredCallbackExecutor == null) {
            safe.run();
            return;
          }
          try {
            CompletableFuture.runAsync(safe, configuredCallbackExecutor).join();
          } catch (Throwable ignored) {
            // Executor rejection and callback failures are isolated.
          }
        };
    try {
      callbackSerial.execute(queued);
    } catch (RejectedExecutionException rejected) {
      if (!critical || callbackSerial.isShutdown()) {
        return;
      }
      callbackSerial.getQueue().poll();
      try {
        callbackSerial.execute(queued);
      } catch (RejectedExecutionException ignored) {
        // A concurrent critical event won the bounded queue slot.
      }
    }
  }

  private void complete(CompletableFuture<Ack> result, Ack value) {
    if (result == null) {
      return;
    }
    if (result.isDone()) {
      retire(result);
      return;
    }
    try {
      completionExecutor.execute(() -> result.complete(value));
    } catch (RejectedExecutionException rejected) {
      if (!completionExecutor.isShutdown()) {
        requestReconnect();
      }
      retire(result);
    }
  }

  private void completeExceptionally(CompletableFuture<Ack> result, Throwable failure) {
    if (result == null) {
      return;
    }
    if (result.isDone()) {
      retire(result);
      return;
    }
    try {
      completionExecutor.execute(() -> result.completeExceptionally(failure));
    } catch (RejectedExecutionException rejected) {
      if (!completionExecutor.isShutdown()) {
        requestReconnect();
      }
      retire(result);
    }
  }

  private static void retire(CompletableFuture<Ack> result) {
    if (result instanceof IsolatedFuture isolated) {
      isolated.retire();
    }
  }

  private static CompletableFuture<Ack> failedFuture(Throwable failure) {
    CompletableFuture<Ack> result = new CompletableFuture<>();
    result.completeExceptionally(failure);
    return result;
  }

  private static long deadlineAfter(Duration duration) {
    return System.nanoTime() + duration.toNanos();
  }

  private static long deadlineAfterMillis(long milliseconds) {
    return System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(milliseconds);
  }

  private static String safeMessage(Throwable failure, String fallback) {
    String value = failure.getMessage();
    return value == null || value.isBlank() ? fallback : value;
  }

  private static ThreadFactory daemonThreadFactory(String name) {
    return task -> {
      Thread thread = new Thread(task, name);
      thread.setDaemon(true);
      return thread;
    };
  }

  private enum CommandKind {
    HEARTBEAT,
    ENABLE,
    DISABLE,
    WRITE,
    RELEASE,
    GENERIC
  }

  private record Command(
      long generation,
      long epoch,
      int type,
      byte[] payload,
      CommandKind kind,
      CompletableFuture<Ack> result) {}

  private record PendingRequest(
      CommandKind kind,
      CompletableFuture<Ack> result,
      long deadlineNanos,
      long generation,
      long epoch) {}

  private record SessionView(
      long generation,
      boolean ready,
      ClientRole role,
      long epoch,
      boolean outputsEnabled,
      boolean disableBarrier) {
    static SessionView disconnected(long generation) {
      return new SessionView(generation, false, ClientRole.OBSERVER, 0, false, true);
    }

    static SessionView handshaking(long generation) {
      return new SessionView(generation, false, ClientRole.OBSERVER, 0, false, true);
    }

    SessionView withDisableBarrier() {
      return new SessionView(generation, ready, role, epoch, outputsEnabled, true);
    }
  }

  /**
   * Holds one bounded completion slot until the transport has retired the command.
   *
   * <p>Cancellation deliberately does not release the slot immediately: the command may already
   * be in flight, and retaining the slot keeps cancellation/replacement loops from bypassing the
   * global outstanding-future bound.
   */
  private static final class IsolatedFuture extends CompletableFuture<Ack> {
    private final Semaphore slots;
    private final Runnable safetyCancellation;
    private final AtomicBoolean retired = new AtomicBoolean();

    IsolatedFuture(Semaphore slots, Runnable safetyCancellation) {
      this.slots = slots;
      this.safetyCancellation = safetyCancellation;
    }

    @Override
    public boolean cancel(boolean mayInterruptIfRunning) {
      boolean cancelled = super.cancel(mayInterruptIfRunning);
      if (cancelled && safetyCancellation != null) {
        safetyCancellation.run();
      }
      return cancelled;
    }

    @Override
    public boolean complete(Ack value) {
      try {
        return super.complete(value);
      } finally {
        retire();
      }
    }

    @Override
    public boolean completeExceptionally(Throwable failure) {
      try {
        return super.completeExceptionally(failure);
      } finally {
        retire();
      }
    }

    void retire() {
      if (retired.compareAndSet(false, true)) {
        slots.release();
      }
    }
  }

  private static final class OutboundFrame {
    final ByteBuffer bytes;
    final int size;

    OutboundFrame(ByteBuffer bytes) {
      this.bytes = bytes;
      size = bytes.remaining();
    }
  }

  private static final class Transport {
    final EcscProtocol.FrameDecoder decoder = new EcscProtocol.FrameDecoder();
    final ArrayDeque<OutboundFrame> outbound = new ArrayDeque<>();
    final Map<Long, PendingRequest> pending = new HashMap<>();
    final PdoAssembler assemblies;
    final int maximumOutboundBytes;
    int outboundBytes;
    long requestId = 1;
    long helloDeadlineNanos;
    long nextHeartbeatNanos;
    long heartbeatRequestId;
    int heartbeatIntervalMillis = 100;
    int heartbeatAckTimeoutMillis = 100;
    boolean handshakeComplete;
    boolean forceReconnect;
    boolean didWork;

    Transport(SystemCoreConfig config) {
      maximumOutboundBytes = config.maximumOutboundBytes();
      assemblies =
          new PdoAssembler(
              config.maximumPdoAssemblies(), config.maximumPdoAssemblyBytes());
    }

    long nextRequestId() {
      long selected = requestId;
      requestId = requestId == 0xffff_ffffL ? 1 : requestId + 1;
      return selected;
    }

    boolean canEnqueue(int size) {
      return size <= maximumOutboundBytes - outboundBytes;
    }

    boolean canEnqueueCommand(int size) {
      return size <= maximumOutboundBytes - outboundBytes - HEARTBEAT_FRAME_BYTES;
    }

    void enqueue(ByteBuffer bytes) {
      OutboundFrame frame = new OutboundFrame(bytes);
      if (!canEnqueue(frame.size)) {
        throw new IllegalStateException("outbound byte limit exceeded");
      }
      outbound.add(frame);
      outboundBytes += frame.size;
      didWork = true;
    }

    void flush(SocketChannel channel) throws IOException {
      while (!outbound.isEmpty()) {
        OutboundFrame frame = outbound.peek();
        int written = channel.write(frame.bytes);
        if (written < 0) {
          throw new EOFException("daemon socket closed during write");
        }
        if (written == 0) {
          return;
        }
        didWork = true;
        if (!frame.bytes.hasRemaining()) {
          outbound.remove();
          outboundBytes -= frame.size;
        }
      }
    }

    void failPending(Throwable failure, SystemCoreClient owner) {
      for (PendingRequest request : pending.values()) {
        owner.completeExceptionally(request.result, failure);
      }
      pending.clear();
      outbound.clear();
      outboundBytes = 0;
    }
  }

  /**
   * Bounded and overlap-coherent multi-chunk PDO assembler. Package visibility supports
   * deterministic protocol tests without a live robot.
   */
  static final class PdoAssembler {
    private final int maximumAssemblies;
    private final int maximumBytes;
    private final Map<PdoKey, Assembly> assemblies = new HashMap<>();
    private final Map<PdoTarget, Long> deliveredSequences = new HashMap<>();
    private int allocatedBytes;

    PdoAssembler(int maximumAssemblies, int maximumBytes) {
      if (maximumAssemblies <= 0 || maximumBytes <= 0) {
        throw new IllegalArgumentException("PDO assembly limits must be positive");
      }
      this.maximumAssemblies = maximumAssemblies;
      this.maximumBytes = maximumBytes;
    }

    synchronized Optional<PdoInput> accept(EcscProtocol.PdoChunk chunk, Instant receivedAt)
        throws ProtocolException {
      Objects.requireNonNull(chunk, "chunk");
      Objects.requireNonNull(receivedAt, "receivedAt");
      PdoKey key =
          new PdoKey(
              chunk.busIndex(),
              chunk.subDeviceIndex(),
              chunk.epoch(),
              chunk.cycleSequence());
      PdoTarget target =
          new PdoTarget(chunk.busIndex(), chunk.subDeviceIndex(), chunk.epoch());
      Long delivered = deliveredSequences.get(target);
      if (delivered != null
          && Long.compareUnsigned(chunk.cycleSequence(), delivered) <= 0) {
        return Optional.empty();
      }
      Assembly assembly = assemblies.get(key);
      if (assembly == null) {
        if (assemblies.size() >= maximumAssemblies
            || chunk.totalSize() > maximumBytes - allocatedBytes) {
          throw new ProtocolException("PDO assembly limits exceeded");
        }
        assembly = new Assembly(chunk.totalSize(), System.nanoTime());
        assemblies.put(key, assembly);
        allocatedBytes += chunk.totalSize();
      } else if (assembly.data.length != chunk.totalSize()) {
        throw new ProtocolException("PDO chunks disagree on total size");
      }

      byte[] data = chunk.data();
      for (int index = 0; index < data.length; index++) {
        int destination = chunk.offset() + index;
        if (assembly.present.get(destination)
            && assembly.data[destination] != data[index]) {
          throw new ProtocolException("overlapping PDO chunks contain different bytes");
        }
        assembly.data[destination] = data[index];
        assembly.present.set(destination);
      }
      if (assembly.present.cardinality() != assembly.data.length) {
        return Optional.empty();
      }
      assemblies.remove(key);
      allocatedBytes -= assembly.data.length;
      deliveredSequences.put(target, chunk.cycleSequence());
      var iterator = assemblies.entrySet().iterator();
      while (iterator.hasNext()) {
        Map.Entry<PdoKey, Assembly> entry = iterator.next();
        PdoKey candidate = entry.getKey();
        if (candidate.bus() == chunk.busIndex()
            && candidate.subDevice() == chunk.subDeviceIndex()
            && candidate.epoch() == chunk.epoch()
            && Long.compareUnsigned(candidate.sequence(), chunk.cycleSequence()) <= 0) {
          allocatedBytes -= entry.getValue().data.length;
          iterator.remove();
        }
      }
      return Optional.of(
          new PdoInput(
              chunk.busIndex(),
              chunk.subDeviceIndex(),
              chunk.cycleSequence(),
              chunk.epoch(),
              assembly.data,
              receivedAt));
    }

    synchronized void expireOlderThan(Duration maximumAge) {
      long threshold = System.nanoTime() - maximumAge.toNanos();
      assemblies.entrySet().removeIf(
          entry -> {
            if (entry.getValue().createdNanos - threshold < 0) {
              allocatedBytes -= entry.getValue().data.length;
              return true;
            }
            return false;
          });
    }

    synchronized int assemblyCount() {
      return assemblies.size();
    }

    synchronized int allocatedBytes() {
      return allocatedBytes;
    }

    private record PdoKey(int bus, int subDevice, long epoch, long sequence) {}

    private record PdoTarget(int bus, int subDevice, long epoch) {}

    private static final class Assembly {
      final byte[] data;
      final BitSet present;
      final long createdNanos;

      Assembly(int size, long createdNanos) {
        data = new byte[size];
        present = new BitSet(size);
        this.createdNanos = createdNanos;
      }
    }
  }
}
