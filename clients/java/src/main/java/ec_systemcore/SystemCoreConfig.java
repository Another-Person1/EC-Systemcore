package ec_systemcore;

import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.Executor;
import ec_systemcore.SystemCoreTypes.ClientRole;

/** Validated runtime limits and connection policy for {@link SystemCoreClient}. */
public final class SystemCoreConfig {
  private static final int MAXIMUM_UNIX_SOCKET_PATH_BYTES = 107;
  private static final Duration MAXIMUM_RECONNECT_DELAY = Duration.ofMinutes(1);
  private static final Duration MAXIMUM_REQUEST_TIMEOUT = Duration.ofSeconds(30);

  public static final Path DEFAULT_SOCKET =
      Path.of("/run/ec-systemcore/ec-systemcore.sock");

  private final Path socketPath;
  private final ClientRole requestedRole;
  private final boolean subscribeInputs;
  private final int inputPeriodMillis;
  private final Duration minimumReconnectDelay;
  private final Duration maximumReconnectDelay;
  private final Duration requestTimeout;
  private final int maximumQueuedCommands;
  private final int maximumOutboundBytes;
  private final int maximumPdoImageBytes;
  private final int maximumPdoAssemblies;
  private final int maximumPdoAssemblyBytes;
  private final int maximumTrackedBuses;
  private final int maximumTrackedPdoInputs;
  private final int maximumRetainedPdoBytes;
  private final Executor callbackExecutor;
  private final SystemCoreListener listener;

  private SystemCoreConfig(Builder builder) {
    socketPath =
        Objects.requireNonNull(builder.socketPath, "socketPath")
            .toAbsolutePath()
            .normalize();
    String socketPathText = socketPath.toString();
    if (socketPathText.indexOf('\0') >= 0
        || socketPathText.getBytes(StandardCharsets.UTF_8).length
            > MAXIMUM_UNIX_SOCKET_PATH_BYTES) {
      throw new IllegalArgumentException(
          "socketPath must fit the Linux sockaddr_un path limit");
    }
    requestedRole = Objects.requireNonNull(builder.requestedRole, "requestedRole");
    subscribeInputs = builder.subscribeInputs;
    inputPeriodMillis = range(builder.inputPeriodMillis, 10, 1000, "inputPeriodMillis");
    minimumReconnectDelay =
        boundedDuration(
            builder.minimumReconnectDelay,
            Duration.ofMillis(1),
            MAXIMUM_RECONNECT_DELAY,
            "minimumReconnectDelay");
    maximumReconnectDelay =
        boundedDuration(
            builder.maximumReconnectDelay,
            Duration.ofMillis(1),
            MAXIMUM_RECONNECT_DELAY,
            "maximumReconnectDelay");
    if (maximumReconnectDelay.compareTo(minimumReconnectDelay) < 0) {
      throw new IllegalArgumentException(
          "maximumReconnectDelay must be at least minimumReconnectDelay");
    }
    requestTimeout =
        boundedDuration(
            builder.requestTimeout,
            Duration.ofMillis(10),
            MAXIMUM_REQUEST_TIMEOUT,
            "requestTimeout");
    maximumQueuedCommands =
        range(builder.maximumQueuedCommands, 1, 4096, "maximumQueuedCommands");
    maximumOutboundBytes =
        range(builder.maximumOutboundBytes, 8192, 1024 * 1024, "maximumOutboundBytes");
    maximumPdoImageBytes =
        range(builder.maximumPdoImageBytes, 1, EcscProtocol.MAXIMUM_PDO_IMAGE,
            "maximumPdoImageBytes");
    maximumPdoAssemblies =
        range(builder.maximumPdoAssemblies, 1, 256, "maximumPdoAssemblies");
    maximumPdoAssemblyBytes =
        range(builder.maximumPdoAssemblyBytes, maximumPdoImageBytes,
            64 * 1024 * 1024, "maximumPdoAssemblyBytes");
    maximumTrackedBuses =
        range(builder.maximumTrackedBuses, 1, 256, "maximumTrackedBuses");
    maximumTrackedPdoInputs =
        range(
            builder.maximumTrackedPdoInputs,
            1,
            4096,
            "maximumTrackedPdoInputs");
    maximumRetainedPdoBytes =
        range(
            builder.maximumRetainedPdoBytes,
            maximumPdoImageBytes,
            64 * 1024 * 1024,
            "maximumRetainedPdoBytes");
    callbackExecutor = builder.callbackExecutor;
    listener = Objects.requireNonNull(builder.listener, "listener");
  }

  public static Builder builder() {
    return new Builder();
  }

  public Path socketPath() {
    return socketPath;
  }

  public ClientRole requestedRole() {
    return requestedRole;
  }

  public boolean subscribeInputs() {
    return subscribeInputs;
  }

  public int inputPeriodMillis() {
    return inputPeriodMillis;
  }

  public Duration minimumReconnectDelay() {
    return minimumReconnectDelay;
  }

  public Duration maximumReconnectDelay() {
    return maximumReconnectDelay;
  }

  public Duration requestTimeout() {
    return requestTimeout;
  }

  public int maximumQueuedCommands() {
    return maximumQueuedCommands;
  }

  public int maximumOutboundBytes() {
    return maximumOutboundBytes;
  }

  public int maximumPdoImageBytes() {
    return maximumPdoImageBytes;
  }

  public int maximumPdoAssemblies() {
    return maximumPdoAssemblies;
  }

  public int maximumPdoAssemblyBytes() {
    return maximumPdoAssemblyBytes;
  }

  public int maximumTrackedBuses() {
    return maximumTrackedBuses;
  }

  public int maximumTrackedPdoInputs() {
    return maximumTrackedPdoInputs;
  }

  public int maximumRetainedPdoBytes() {
    return maximumRetainedPdoBytes;
  }

  public Executor callbackExecutor() {
    return callbackExecutor;
  }

  public SystemCoreListener listener() {
    return listener;
  }

  private static int range(int value, int minimum, int maximum, String field) {
    if (value < minimum || value > maximum) {
      throw new IllegalArgumentException(
          field + " must be between " + minimum + " and " + maximum);
    }
    return value;
  }

  private static Duration positive(Duration value, String field) {
    Objects.requireNonNull(value, field);
    if (value.compareTo(Duration.ofMillis(1)) < 0) {
      throw new IllegalArgumentException(field + " must be a positive millisecond duration");
    }
    return value;
  }

  private static Duration boundedDuration(
      Duration value, Duration minimum, Duration maximum, String field) {
    positive(value, field);
    if (value.compareTo(minimum) < 0 || value.compareTo(maximum) > 0) {
      throw new IllegalArgumentException(
          field + " must be between " + minimum + " and " + maximum);
    }
    return value;
  }

  public static final class Builder {
    private Path socketPath = DEFAULT_SOCKET;
    private ClientRole requestedRole = ClientRole.OBSERVER;
    private boolean subscribeInputs;
    private int inputPeriodMillis = 100;
    private Duration minimumReconnectDelay = Duration.ofMillis(250);
    private Duration maximumReconnectDelay = Duration.ofSeconds(5);
    private Duration requestTimeout = Duration.ofSeconds(2);
    private int maximumQueuedCommands = 128;
    private int maximumOutboundBytes = 64 * 1024;
    private int maximumPdoImageBytes = EcscProtocol.MAXIMUM_PDO_IMAGE;
    private int maximumPdoAssemblies = 32;
    private int maximumPdoAssemblyBytes = 4 * 1024 * 1024;
    private int maximumTrackedBuses = 32;
    private int maximumTrackedPdoInputs = 1600;
    private int maximumRetainedPdoBytes = 16 * 1024 * 1024;
    private Executor callbackExecutor;
    private SystemCoreListener listener = new SystemCoreListener() {};

    private Builder() {}

    public Builder socketPath(Path value) {
      socketPath = value;
      return this;
    }

    public Builder requestedRole(ClientRole value) {
      requestedRole = value;
      return this;
    }

    public Builder subscribeInputs(boolean value) {
      subscribeInputs = value;
      return this;
    }

    public Builder inputPeriodMillis(int value) {
      inputPeriodMillis = value;
      return this;
    }

    public Builder reconnectDelays(Duration minimum, Duration maximum) {
      minimumReconnectDelay = minimum;
      maximumReconnectDelay = maximum;
      return this;
    }

    public Builder requestTimeout(Duration value) {
      requestTimeout = value;
      return this;
    }

    public Builder maximumQueuedCommands(int value) {
      maximumQueuedCommands = value;
      return this;
    }

    public Builder maximumOutboundBytes(int value) {
      maximumOutboundBytes = value;
      return this;
    }

    public Builder maximumPdoImageBytes(int value) {
      maximumPdoImageBytes = value;
      return this;
    }

    public Builder pdoAssemblyLimits(int maximumAssemblies, int maximumBytes) {
      maximumPdoAssemblies = maximumAssemblies;
      maximumPdoAssemblyBytes = maximumBytes;
      return this;
    }

    /** Bounds immutable bus and completed-PDO snapshots retained for robot-loop polling. */
    public Builder retainedSnapshotLimits(int maximumBuses, int maximumPdoInputs) {
      maximumTrackedBuses = maximumBuses;
      maximumTrackedPdoInputs = maximumPdoInputs;
      return this;
    }

    /** Bounds the aggregate bytes retained by latest complete PDO snapshots. */
    public Builder maximumRetainedPdoBytes(int value) {
      maximumRetainedPdoBytes = value;
      return this;
    }

    public Builder listener(SystemCoreListener value) {
      listener = value;
      return this;
    }

    /**
     * Supplies the executor used for listener calls.
     *
     * <p>If omitted, the client owns a single daemon listener thread. Command futures always
     * complete on a separate bounded client-owned executor so listener code cannot delay
     * heartbeats or command completion.
     */
    public Builder callbackExecutor(Executor value) {
      callbackExecutor = value;
      return this;
    }

    public SystemCoreConfig build() {
      return new SystemCoreConfig(this);
    }
  }
}
