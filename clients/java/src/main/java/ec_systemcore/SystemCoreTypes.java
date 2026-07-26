package ec_systemcore;

import java.time.Instant;
import java.util.HexFormat;
import java.util.Objects;

/** Immutable public value types used by {@link SystemCoreClient}. */
public final class SystemCoreTypes {
  private SystemCoreTypes() {}

  public enum ClientRole {
    OBSERVER(0),
    CONTROLLER(1);

    private final int wireValue;

    ClientRole(int wireValue) {
      this.wireValue = wireValue;
    }

    int wireValue() {
      return wireValue;
    }

    static ClientRole fromWire(int value) throws ProtocolException {
      return switch (value) {
        case 0 -> OBSERVER;
        case 1 -> CONTROLLER;
        default -> throw new ProtocolException("invalid client role " + value);
      };
    }
  }

  public enum AckStatus {
    OK(0),
    MALFORMED(1),
    UNAUTHORIZED(2),
    BUSY(3),
    DISABLED(4),
    BAD_TARGET(5),
    BOUNDS(6),
    STALE_EPOCH(7),
    UNSUPPORTED(8),
    ADAPTER_LOCKED(9),
    TOPOLOGY_MISMATCH(10),
    QUEUE_FULL(11),
    INTERNAL_ERROR(12);

    private final int wireValue;

    AckStatus(int wireValue) {
      this.wireValue = wireValue;
    }

    public int wireValue() {
      return wireValue;
    }

    static AckStatus fromWire(int value) throws ProtocolException {
      for (AckStatus status : values()) {
        if (status.wireValue == value) {
          return status;
        }
      }
      throw new ProtocolException("invalid acknowledgement status " + value);
    }
  }

  public enum BusState {
    STARTING(0),
    WAITING_FOR_LINK(1),
    INITIALIZING(2),
    SAFE_OPERATIONAL(3),
    OPERATIONAL(4),
    FAULT(5),
    STOPPING(6);

    private final int wireValue;

    BusState(int wireValue) {
      this.wireValue = wireValue;
    }

    int wireValue() {
      return wireValue;
    }

    static BusState fromWire(int value) throws ProtocolException {
      for (BusState state : values()) {
        if (state.wireValue == value) {
          return state;
        }
      }
      throw new ProtocolException("invalid bus state " + value);
    }
  }

  public enum AdapterLockState {
    UNLOCKED(0),
    MATCHED(1),
    MISSING(2),
    MISMATCH(3),
    RUNTIME_UNLOCKED(4);

    private final int wireValue;

    AdapterLockState(int wireValue) {
      this.wireValue = wireValue;
    }

    int wireValue() {
      return wireValue;
    }

    static AdapterLockState fromWire(int value) throws ProtocolException {
      for (AdapterLockState state : values()) {
        if (state.wireValue == value) {
          return state;
        }
      }
      throw new ProtocolException("invalid adapter lock state " + value);
    }
  }

  public enum AdapterLockReason {
    NONE(0),
    INTERFACE_MISSING(1),
    IDENTITY_UNAVAILABLE(2),
    ID_PATH_MISMATCH(3),
    PERMANENT_MAC_MISMATCH(4),
    USB_SERIAL_MISMATCH(5),
    USB_VENDOR_MISMATCH(6),
    USB_PRODUCT_MISMATCH(7),
    NON_ETHERNET_INTERFACE(8),
    PATH_ONLY_IDENTITY(9);

    private final int wireValue;

    AdapterLockReason(int wireValue) {
      this.wireValue = wireValue;
    }

    int wireValue() {
      return wireValue;
    }

    static AdapterLockReason fromWire(int value) throws ProtocolException {
      for (AdapterLockReason reason : values()) {
        if (reason.wireValue == value) {
          return reason;
        }
      }
      throw new ProtocolException("invalid adapter lock reason " + value);
    }
  }

  public enum SchedulingMode {
    STANDARD,
    FIFO
  }

  public enum DisableReason {
    REQUESTED(1),
    HEARTBEAT_TIMEOUT(2),
    DISCONNECT(3),
    LINK_LOSS(4),
    REINITIALIZE(5),
    SHUTDOWN(6),
    EPOCH_CHANGE(7),
    ADAPTER_MISMATCH(8),
    PROCESS_DATA_FAULT(9),
    OUTPUT_COMMAND_TIMEOUT(10);

    private final int wireValue;

    DisableReason(int wireValue) {
      this.wireValue = wireValue;
    }

    int wireValue() {
      return wireValue;
    }

    static DisableReason fromWire(int value) throws ProtocolException {
      for (DisableReason reason : values()) {
        if (reason.wireValue == value) {
          return reason;
        }
      }
      throw new ProtocolException("invalid output-disable reason " + value);
    }
  }

  public enum HealthState {
    STOPPED,
    CONNECTING,
    HANDSHAKING,
    OBSERVING,
    CONTROLLING_DISABLED,
    CONTROLLING_ENABLED,
    DEGRADED
  }

  public record Ack(AckStatus status, long detail) {
    public Ack {
      Objects.requireNonNull(status, "status");
      if (detail < 0 || detail > 0xffff_ffffL) {
        throw new IllegalArgumentException("detail must be an unsigned 32-bit value");
      }
    }

    public boolean ok() {
      return status == AckStatus.OK;
    }
  }

  public record Status(
      BusState aggregateState,
      boolean operational,
      boolean outputsEnabled,
      boolean controllerConnected,
      boolean preemptRtAvailable,
      boolean realtimeApplied,
      boolean realtimeRequested,
      boolean timingDegraded,
      boolean reinitializing,
      SchedulingMode schedulingMode,
      int realtimeErrorBits,
      boolean distributedClockUnlocked,
      int activeAdapters,
      int subDeviceCount,
      int activeFaults,
      long maximumJitterMicros,
      long currentJitterMicros,
      long lostFrames,
      long cycleOverruns,
      long epoch,
      long controllerPid,
      int configuredBuses,
      long monotonicTimestampMicros,
      Instant receivedAt) {
    public Status {
      Objects.requireNonNull(aggregateState, "aggregateState");
      Objects.requireNonNull(schedulingMode, "schedulingMode");
      Objects.requireNonNull(receivedAt, "receivedAt");
      requireUnsigned16(activeAdapters, "activeAdapters");
      requireUnsigned16(subDeviceCount, "subDeviceCount");
      requireUnsigned16(activeFaults, "activeFaults");
      requireUnsigned32(maximumJitterMicros, "maximumJitterMicros");
      requireUnsigned32(currentJitterMicros, "currentJitterMicros");
      requireUnsigned32(controllerPid, "controllerPid");
      requireUnsigned16(configuredBuses, "configuredBuses");
      if (epoch == 0) {
        throw new IllegalArgumentException("epoch must be non-zero");
      }
      if (realtimeErrorBits < 0 || realtimeErrorBits > 0x7f) {
        throw new IllegalArgumentException("realtimeErrorBits must contain only known bits");
      }
    }

    public String epochUnsigned() {
      return Long.toUnsignedString(epoch);
    }

    public String lostFramesUnsigned() {
      return Long.toUnsignedString(lostFrames);
    }

    public String cycleOverrunsUnsigned() {
      return Long.toUnsignedString(cycleOverruns);
    }
  }

  public record AdapterIdentity(
      String idPath,
      String permanentMac,
      String usbSerial,
      String usbVendorId,
      String usbProductId) {
    public AdapterIdentity {
      idPath = Objects.requireNonNull(idPath, "idPath");
      permanentMac = Objects.requireNonNull(permanentMac, "permanentMac");
      usbSerial = Objects.requireNonNull(usbSerial, "usbSerial");
      usbVendorId = Objects.requireNonNull(usbVendorId, "usbVendorId");
      usbProductId = Objects.requireNonNull(usbProductId, "usbProductId");
    }

    public boolean lockable() {
      return !idPath.isEmpty();
    }
  }

  public record BusInfo(
      int busIndex,
      BusState state,
      boolean linkUp,
      AdapterLockState lockState,
      AdapterLockReason lockReason,
      int subDeviceCount,
      String logicalName,
      String physicalInterface,
      AdapterIdentity identity,
      long epoch) {
    public BusInfo {
      requireUnsigned16(busIndex, "busIndex");
      requireUnsigned16(subDeviceCount, "subDeviceCount");
      Objects.requireNonNull(state, "state");
      Objects.requireNonNull(lockState, "lockState");
      Objects.requireNonNull(lockReason, "lockReason");
      logicalName = Objects.requireNonNull(logicalName, "logicalName");
      physicalInterface = Objects.requireNonNull(physicalInterface, "physicalInterface");
      identity = Objects.requireNonNull(identity, "identity");
      if (epoch == 0) {
        throw new IllegalArgumentException("epoch must be non-zero");
      }
    }

    public String epochUnsigned() {
      return Long.toUnsignedString(epoch);
    }
  }

  public record PdoInput(
      int busIndex,
      int subDeviceIndex,
      long cycleSequence,
      long epoch,
      byte[] data,
      Instant receivedAt) {
    public PdoInput {
      if (busIndex < 0 || busIndex > 0xffff) {
        throw new IllegalArgumentException("busIndex must be unsigned 16-bit");
      }
      if (subDeviceIndex <= 0 || subDeviceIndex > 0xffff) {
        throw new IllegalArgumentException("subDeviceIndex must be 1..65535");
      }
      if (cycleSequence == 0 || epoch == 0) {
        throw new IllegalArgumentException("cycleSequence and epoch must be non-zero");
      }
      data = Objects.requireNonNull(data, "data").clone();
      Objects.requireNonNull(receivedAt, "receivedAt");
    }

    @Override
    public byte[] data() {
      return data.clone();
    }

    public String dataHex() {
      return HexFormat.of().formatHex(data);
    }

    public int size() {
      return data.length;
    }

    /** Copies the complete image without allocating; returns the number of bytes copied. */
    public int copyDataTo(byte[] destination) {
      return copyDataTo(destination, 0);
    }

    /** Copies the complete image at {@code destinationOffset} without allocating. */
    public int copyDataTo(byte[] destination, int destinationOffset) {
      Objects.requireNonNull(destination, "destination");
      if (destinationOffset < 0 || destinationOffset > destination.length
          || data.length > destination.length - destinationOffset) {
        throw new IndexOutOfBoundsException("destination cannot hold the complete PDO image");
      }
      System.arraycopy(data, 0, destination, destinationOffset, data.length);
      return data.length;
    }

    public String epochUnsigned() {
      return Long.toUnsignedString(epoch);
    }

    public String cycleSequenceUnsigned() {
      return Long.toUnsignedString(cycleSequence);
    }
  }

  /** Stable key for the latest complete PDO image retained by the client. */
  public record PdoAddress(int busIndex, int subDeviceIndex) {
    public PdoAddress {
      if (busIndex < 0 || busIndex > 0xffff) {
        throw new IllegalArgumentException("busIndex must be unsigned 16-bit");
      }
      if (subDeviceIndex <= 0 || subDeviceIndex > 0xffff) {
        throw new IllegalArgumentException("subDeviceIndex must be 1..65535");
      }
    }
  }

  public record OutputsDisabled(
      DisableReason reason,
      int busIndex,
      long epoch,
      Instant receivedAt) {
    public OutputsDisabled {
      Objects.requireNonNull(reason, "reason");
      Objects.requireNonNull(receivedAt, "receivedAt");
      requireUnsigned16(busIndex, "busIndex");
      if (epoch == 0) {
        throw new IllegalArgumentException("epoch must be non-zero");
      }
    }

    public boolean appliesToEveryBus() {
      return busIndex == 0xffff;
    }
  }

  public record Health(
      HealthState state,
      String message,
      Throwable cause,
      long generation,
      Instant changedAt) {
    public Health {
      Objects.requireNonNull(state, "state");
      message = Objects.requireNonNull(message, "message");
      Objects.requireNonNull(changedAt, "changedAt");
    }
  }

  private static void requireUnsigned16(int value, String field) {
    if (value < 0 || value > 0xffff) {
      throw new IllegalArgumentException(field + " must be unsigned 16-bit");
    }
  }

  private static void requireUnsigned32(long value, String field) {
    if (value < 0 || value > 0xffff_ffffL) {
      throw new IllegalArgumentException(field + " must be unsigned 32-bit");
    }
  }
}
