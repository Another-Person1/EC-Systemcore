package ec_systemcore;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import ec_systemcore.SystemCoreTypes.Ack;
import ec_systemcore.SystemCoreTypes.AckStatus;
import ec_systemcore.SystemCoreTypes.AdapterIdentity;
import ec_systemcore.SystemCoreTypes.AdapterLockReason;
import ec_systemcore.SystemCoreTypes.AdapterLockState;
import ec_systemcore.SystemCoreTypes.BusInfo;
import ec_systemcore.SystemCoreTypes.BusState;
import ec_systemcore.SystemCoreTypes.ClientRole;
import ec_systemcore.SystemCoreTypes.DisableReason;
import ec_systemcore.SystemCoreTypes.OutputsDisabled;
import ec_systemcore.SystemCoreTypes.SchedulingMode;
import ec_systemcore.SystemCoreTypes.Status;

/** Bounded ECSC version 1 wire codec shared by the Java client and its fake daemon tests. */
final class EcscProtocol {
  static final int VERSION = 1;
  static final int ENVELOPE_BYTES = 12;
  static final int LENGTH_PREFIX_BYTES = 4;
  static final int MAXIMUM_FRAME_BODY = 8192;
  static final int MAXIMUM_BUFFERED_INPUT =
      2 * (LENGTH_PREFIX_BYTES + MAXIMUM_FRAME_BODY);
  static final int MAXIMUM_OUTPUT_WRITE = 1024;
  static final int MAXIMUM_PDO_CHUNK = 1024;
  static final int MAXIMUM_PDO_IMAGE = 1024 * 1024;
  static final int MAXIMUM_BUS_INFO = 2048;

  static final int HELLO = 0x01;
  static final int HEARTBEAT = 0x02;
  static final int OUTPUT_ENABLE = 0x03;
  static final int OUTPUT_WRITE = 0x04;
  static final int CLEAR_COUNTERS = 0x05;
  static final int RELEASE_CONTROL = 0x06;
  static final int SUBSCRIBE_INPUTS = 0x07;
  static final int ADAPTER_UNLOCK = 0x08;
  static final int ADAPTER_RESCAN = 0x09;
  static final int HELLO_ACK = 0x80;
  static final int STATUS = 0x81;
  static final int ACK = 0x82;
  static final int PDO_INPUT = 0x83;
  static final int BUS_INFO = 0x84;
  static final int ERROR = 0x85;
  static final int OUTPUTS_DISABLED = 0x86;

  private static final byte[] MAGIC = {'E', 'C', 'S', 'C'};
  private static final ByteOrder WIRE_ORDER = ByteOrder.LITTLE_ENDIAN;

  private EcscProtocol() {}

  record Frame(int type, long requestId, byte[] payload) {
    Frame {
      if (!knownMessageType(type)) {
        throw new IllegalArgumentException("unknown message type");
      }
      if (requestId < 0 || requestId > 0xffff_ffffL) {
        throw new IllegalArgumentException("requestId must be unsigned 32-bit");
      }
      payload = Objects.requireNonNull(payload, "payload").clone();
    }

    @Override
    public byte[] payload() {
      return payload.clone();
    }
  }

  record HelloAck(
      ClientRole grantedRole,
      boolean outputsEnabled,
      int heartbeatTimeoutMillis,
      long epoch,
      long peerPid,
      long capabilities) {}

  record PdoChunk(
      int busIndex,
      int subDeviceIndex,
      int offset,
      int totalSize,
      long epoch,
      long cycleSequence,
      byte[] data) {
    PdoChunk {
      data = data.clone();
    }

    @Override
    public byte[] data() {
      return data.clone();
    }
  }

  static ByteBuffer encodeFrame(int type, long requestId, byte[] payload) {
    Objects.requireNonNull(payload, "payload");
    if (!knownMessageType(type)) {
      throw new IllegalArgumentException("unknown message type " + type);
    }
    if (requestId < 0 || requestId > 0xffff_ffffL) {
      throw new IllegalArgumentException("requestId must be unsigned 32-bit");
    }
    int bodyBytes = ENVELOPE_BYTES + payload.length;
    if (bodyBytes > MAXIMUM_FRAME_BODY) {
      throw new IllegalArgumentException("frame exceeds ECSC maximum");
    }
    ByteBuffer encoded =
        ByteBuffer.allocate(LENGTH_PREFIX_BYTES + bodyBytes).order(WIRE_ORDER);
    encoded.putInt(bodyBytes);
    encoded.put(MAGIC);
    encoded.put((byte) VERSION);
    encoded.put((byte) type);
    encoded.putShort((short) 0);
    encoded.putInt((int) requestId);
    encoded.put(payload);
    encoded.flip();
    return encoded;
  }

  static byte[] encodeHello(
      ClientRole role, boolean subscribeInputs, int periodMillis, long lastSeenEpoch) {
    Objects.requireNonNull(role, "role");
    requireRange(periodMillis, 10, 1000, "input period");
    ByteBuffer payload = allocate(12);
    payload.put((byte) role.wireValue());
    payload.put((byte) (subscribeInputs ? 1 : 0));
    payload.putShort((short) periodMillis);
    payload.putLong(lastSeenEpoch);
    return payload.array();
  }

  static byte[] encodeEpoch(long epoch) {
    requireEpoch(epoch);
    return allocate(8).putLong(epoch).array();
  }

  static byte[] encodeOutputEnable(long epoch, boolean enabled) {
    requireEpoch(epoch);
    return allocate(9).putLong(epoch).put((byte) (enabled ? 1 : 0)).array();
  }

  static byte[] encodeOutputWrite(
      long epoch, int busIndex, int subDeviceIndex, long offset, byte[] data) {
    requireEpoch(epoch);
    requireRange(busIndex, 0, 0xffff, "busIndex");
    requireRange(subDeviceIndex, 1, 0xffff, "subDeviceIndex");
    if (offset < 0 || offset > 0xffff_ffffL) {
      throw new IllegalArgumentException("offset must be unsigned 32-bit");
    }
    Objects.requireNonNull(data, "data");
    if (data.length == 0 || data.length > MAXIMUM_OUTPUT_WRITE) {
      throw new IllegalArgumentException(
          "output write must contain 1.." + MAXIMUM_OUTPUT_WRITE + " bytes");
    }
    ByteBuffer payload = allocate(20 + data.length);
    payload.putLong(epoch);
    payload.putShort((short) busIndex);
    payload.putShort((short) subDeviceIndex);
    payload.putInt((int) offset);
    payload.putShort((short) data.length);
    payload.putShort((short) 0);
    payload.put(data);
    return payload.array();
  }

  static byte[] encodeSubscription(boolean enabled, int periodMillis) {
    requireRange(periodMillis, 10, 1000, "input period");
    return allocate(4)
        .put((byte) (enabled ? 1 : 0))
        .put((byte) 0)
        .putShort((short) periodMillis)
        .array();
  }

  static byte[] encodeAdapterUnlock(long epoch, int busIndex) {
    requireEpoch(epoch);
    requireRange(busIndex, 0, 0xffff, "busIndex");
    return allocate(10).putLong(epoch).putShort((short) busIndex).array();
  }

  static HelloAck decodeHelloAck(byte[] payload) throws ProtocolException {
    requireLength(payload, 20, "HELLO_ACK");
    ByteBuffer bytes = wrap(payload);
    ClientRole role = ClientRole.fromWire(unsignedByte(bytes.get()));
    int outputs = unsignedByte(bytes.get());
    if (outputs > 1) {
      throw new ProtocolException("HELLO_ACK outputsEnabled is not boolean");
    }
    int heartbeat = unsignedShort(bytes.getShort());
    long epoch = bytes.getLong();
    long peerPid = unsignedInt(bytes.getInt());
    long capabilities = unsignedInt(bytes.getInt());
    if (heartbeat == 0 || epoch == 0) {
      throw new ProtocolException("HELLO_ACK heartbeat/epoch is invalid");
    }
    return new HelloAck(role, outputs != 0, heartbeat, epoch, peerPid, capabilities);
  }

  static Ack decodeAck(byte[] payload) throws ProtocolException {
    requireLength(payload, 8, "ACK");
    ByteBuffer bytes = wrap(payload);
    AckStatus status = AckStatus.fromWire(unsignedShort(bytes.getShort()));
    if (bytes.getShort() != 0) {
      throw new ProtocolException("ACK reserved field is non-zero");
    }
    return new Ack(status, unsignedInt(bytes.getInt()));
  }

  static Status decodeStatus(byte[] payload, Instant receivedAt)
      throws ProtocolException {
    requireLength(payload, 56, "STATUS");
    ByteBuffer bytes = wrap(payload);
    BusState state = BusState.fromWire(unsignedByte(bytes.get()));
    int flags = unsignedByte(bytes.get());
    int activeAdapters = unsignedShort(bytes.getShort());
    int subDevices = unsignedShort(bytes.getShort());
    int faults = unsignedShort(bytes.getShort());
    long maxJitter = unsignedInt(bytes.getInt());
    long currentJitter = unsignedInt(bytes.getInt());
    long lostFrames = bytes.getLong();
    long overruns = bytes.getLong();
    long epoch = bytes.getLong();
    long controllerPid = unsignedInt(bytes.getInt());
    int configuredBuses = unsignedShort(bytes.getShort());
    int scheduling = unsignedByte(bytes.get());
    int realtimeErrors = unsignedByte(bytes.get());
    long monotonic = bytes.getLong();
    if (epoch == 0 || scheduling > 1 || (realtimeErrors & 0x80) != 0) {
      throw new ProtocolException("STATUS epoch/scheduling diagnostics are invalid");
    }
    return new Status(
        state,
        (flags & (1 << 0)) != 0,
        (flags & (1 << 1)) != 0,
        (flags & (1 << 2)) != 0,
        (flags & (1 << 3)) != 0,
        (flags & (1 << 4)) != 0,
        (flags & (1 << 5)) != 0,
        (flags & (1 << 6)) != 0,
        (flags & (1 << 7)) != 0,
        scheduling == 1 ? SchedulingMode.FIFO : SchedulingMode.STANDARD,
        realtimeErrors,
        (realtimeErrors & (1 << 6)) != 0,
        activeAdapters,
        subDevices,
        faults,
        maxJitter,
        currentJitter,
        lostFrames,
        overruns,
        epoch,
        controllerPid,
        configuredBuses,
        monotonic,
        receivedAt);
  }

  static BusInfo decodeBusInfo(byte[] payload) throws ProtocolException {
    if (payload.length < 30 || payload.length > MAXIMUM_BUS_INFO) {
      throw new ProtocolException("BUS_INFO length is invalid");
    }
    ByteBuffer bytes = wrap(payload);
    int busIndex = unsignedShort(bytes.getShort());
    BusState state = BusState.fromWire(unsignedByte(bytes.get()));
    int link = unsignedByte(bytes.get());
    AdapterLockState lockState = AdapterLockState.fromWire(unsignedByte(bytes.get()));
    AdapterLockReason lockReason = AdapterLockReason.fromWire(unsignedByte(bytes.get()));
    int subDevices = unsignedShort(bytes.getShort());
    int[] lengths = new int[7];
    int expected = 30;
    for (int index = 0; index < lengths.length; index++) {
      lengths[index] = unsignedShort(bytes.getShort());
      expected = Math.addExact(expected, lengths[index]);
    }
    long epoch = bytes.getLong();
    if (link > 1 || epoch == 0 || expected != payload.length) {
      throw new ProtocolException("BUS_INFO fields are invalid");
    }
    String[] values = new String[7];
    for (int index = 0; index < values.length; index++) {
      byte[] encoded = new byte[lengths[index]];
      bytes.get(encoded);
      values[index] = strictUtf8(encoded, "BUS_INFO string");
    }
    return new BusInfo(
        busIndex,
        state,
        link != 0,
        lockState,
        lockReason,
        subDevices,
        values[0],
        values[1],
        new AdapterIdentity(values[2], values[3], values[4], values[5], values[6]),
        epoch);
  }

  static PdoChunk decodePdoChunk(byte[] payload, int maximumTotalSize)
      throws ProtocolException {
    if (payload.length < 32) {
      throw new ProtocolException("PDO_INPUT payload is truncated");
    }
    ByteBuffer bytes = wrap(payload);
    int bus = unsignedShort(bytes.getShort());
    int subDevice = unsignedShort(bytes.getShort());
    long offsetUnsigned = unsignedInt(bytes.getInt());
    long totalUnsigned = unsignedInt(bytes.getInt());
    int chunkLength = unsignedShort(bytes.getShort());
    int reserved = unsignedShort(bytes.getShort());
    long epoch = bytes.getLong();
    long sequence = bytes.getLong();
    if (subDevice == 0
        || reserved != 0
        || epoch == 0
        || sequence == 0
        || totalUnsigned == 0
        || chunkLength == 0
        || chunkLength > MAXIMUM_PDO_CHUNK
        || totalUnsigned > maximumTotalSize
        || offsetUnsigned > totalUnsigned
        || chunkLength > totalUnsigned - offsetUnsigned
        || payload.length != 32 + chunkLength) {
      throw new ProtocolException("PDO_INPUT bounds are invalid");
    }
    byte[] data = new byte[chunkLength];
    bytes.get(data);
    return new PdoChunk(
        bus,
        subDevice,
        Math.toIntExact(offsetUnsigned),
        Math.toIntExact(totalUnsigned),
        epoch,
        sequence,
        data);
  }

  static OutputsDisabled decodeOutputsDisabled(byte[] payload, Instant receivedAt)
      throws ProtocolException {
    requireLength(payload, 12, "OUTPUTS_DISABLED");
    ByteBuffer bytes = wrap(payload);
    DisableReason reason = DisableReason.fromWire(unsignedShort(bytes.getShort()));
    int busIndex = unsignedShort(bytes.getShort());
    long epoch = bytes.getLong();
    if (epoch == 0) {
      throw new ProtocolException("OUTPUTS_DISABLED epoch is zero");
    }
    return new OutputsDisabled(reason, busIndex, epoch, receivedAt);
  }

  static byte[] bytes(ByteBuffer buffer) {
    ByteBuffer copy = buffer.asReadOnlyBuffer();
    byte[] result = new byte[copy.remaining()];
    copy.get(result);
    return result;
  }

  static final class FrameDecoder {
    private byte[] buffer = new byte[1024];
    private int size;

    List<Frame> feed(ByteBuffer input) throws ProtocolException {
      int incoming = input.remaining();
      if (incoming > MAXIMUM_BUFFERED_INPUT - size) {
        reset();
        throw new ProtocolException("ECSC input buffer exceeded its limit");
      }
      ensureCapacity(size + incoming);
      input.get(buffer, size, incoming);
      size += incoming;
      List<Frame> frames = new ArrayList<>();
      int offset = 0;
      while (size - offset >= LENGTH_PREFIX_BYTES) {
        long bodyUnsigned = readUnsignedInt(buffer, offset);
        if (bodyUnsigned < ENVELOPE_BYTES || bodyUnsigned > MAXIMUM_FRAME_BODY) {
          reset();
          throw new ProtocolException("invalid ECSC frame length " + bodyUnsigned);
        }
        int bodyBytes = (int) bodyUnsigned;
        int totalBytes = LENGTH_PREFIX_BYTES + bodyBytes;
        if (size - offset < totalBytes) {
          break;
        }
        int bodyOffset = offset + LENGTH_PREFIX_BYTES;
        if (!Arrays.equals(
            MAGIC, 0, MAGIC.length, buffer, bodyOffset, bodyOffset + MAGIC.length)) {
          reset();
          throw new ProtocolException("invalid ECSC magic");
        }
        int version = unsignedByte(buffer[bodyOffset + 4]);
        int type = unsignedByte(buffer[bodyOffset + 5]);
        int flags = readUnsignedShort(buffer, bodyOffset + 6);
        long requestId = readUnsignedInt(buffer, bodyOffset + 8);
        if (version != VERSION || flags != 0 || !knownMessageType(type)) {
          reset();
          throw new ProtocolException("unsupported ECSC version, flags, or type");
        }
        int payloadOffset = bodyOffset + ENVELOPE_BYTES;
        byte[] payload = Arrays.copyOfRange(buffer, payloadOffset, offset + totalBytes);
        frames.add(new Frame(type, requestId, payload));
        offset += totalBytes;
      }
      if (offset > 0) {
        System.arraycopy(buffer, offset, buffer, 0, size - offset);
        size -= offset;
      }
      return frames;
    }

    int bufferedBytes() {
      return size;
    }

    void reset() {
      size = 0;
    }

    private void ensureCapacity(int required) {
      if (required <= buffer.length) {
        return;
      }
      int capacity = buffer.length;
      while (capacity < required) {
        capacity = Math.min(MAXIMUM_BUFFERED_INPUT, capacity * 2);
      }
      buffer = Arrays.copyOf(buffer, capacity);
    }
  }

  static byte[] encodeBusInfoForTest(
      int busIndex,
      BusState state,
      boolean linkUp,
      AdapterLockState lockState,
      AdapterLockReason lockReason,
      int subDevices,
      AdapterIdentity identity,
      String logicalName,
      String physicalInterface,
      long epoch) {
    requireEpoch(epoch);
    byte[][] fields = {
      utf8(logicalName),
      utf8(physicalInterface),
      utf8(identity.idPath()),
      utf8(identity.permanentMac()),
      utf8(identity.usbSerial()),
      utf8(identity.usbVendorId()),
      utf8(identity.usbProductId())
    };
    int total = 30;
    for (byte[] field : fields) {
      if (field.length > 0xffff) {
        throw new IllegalArgumentException("BUS_INFO field is too long");
      }
      total = Math.addExact(total, field.length);
    }
    if (total > MAXIMUM_BUS_INFO) {
      throw new IllegalArgumentException("BUS_INFO exceeds maximum");
    }
    ByteBuffer payload = allocate(total);
    payload.putShort((short) busIndex);
    payload.put((byte) state.wireValue());
    payload.put((byte) (linkUp ? 1 : 0));
    payload.put((byte) lockState.wireValue());
    payload.put((byte) lockReason.wireValue());
    payload.putShort((short) subDevices);
    for (byte[] field : fields) {
      payload.putShort((short) field.length);
    }
    payload.putLong(epoch);
    for (byte[] field : fields) {
      payload.put(field);
    }
    return payload.array();
  }

  static byte[] encodeHelloAckForTest(
      ClientRole role,
      boolean outputsEnabled,
      int heartbeatMillis,
      long epoch,
      long peerPid,
      long capabilities) {
    requireEpoch(epoch);
    requireRange(heartbeatMillis, 1, 0xffff, "heartbeatMillis");
    ByteBuffer payload = allocate(20);
    payload.put((byte) role.wireValue());
    payload.put((byte) (outputsEnabled ? 1 : 0));
    payload.putShort((short) heartbeatMillis);
    payload.putLong(epoch);
    payload.putInt((int) peerPid);
    payload.putInt((int) capabilities);
    return payload.array();
  }

  static byte[] encodeStatusForTest(Status status) {
    ByteBuffer payload = allocate(56);
    int flags = 0;
    flags |= status.operational() ? 1 << 0 : 0;
    flags |= status.outputsEnabled() ? 1 << 1 : 0;
    flags |= status.controllerConnected() ? 1 << 2 : 0;
    flags |= status.preemptRtAvailable() ? 1 << 3 : 0;
    flags |= status.realtimeApplied() ? 1 << 4 : 0;
    flags |= status.realtimeRequested() ? 1 << 5 : 0;
    flags |= status.timingDegraded() ? 1 << 6 : 0;
    flags |= status.reinitializing() ? 1 << 7 : 0;
    payload.put((byte) status.aggregateState().wireValue());
    payload.put((byte) flags);
    payload.putShort((short) status.activeAdapters());
    payload.putShort((short) status.subDeviceCount());
    payload.putShort((short) status.activeFaults());
    payload.putInt((int) status.maximumJitterMicros());
    payload.putInt((int) status.currentJitterMicros());
    payload.putLong(status.lostFrames());
    payload.putLong(status.cycleOverruns());
    payload.putLong(status.epoch());
    payload.putInt((int) status.controllerPid());
    payload.putShort((short) status.configuredBuses());
    payload.put((byte) (status.schedulingMode() == SchedulingMode.FIFO ? 1 : 0));
    payload.put((byte) status.realtimeErrorBits());
    payload.putLong(status.monotonicTimestampMicros());
    return payload.array();
  }

  static byte[] encodeAckForTest(AckStatus status, long detail) {
    if (detail < 0 || detail > 0xffff_ffffL) {
      throw new IllegalArgumentException("detail must be unsigned 32-bit");
    }
    return allocate(8)
        .putShort((short) status.wireValue())
        .putShort((short) 0)
        .putInt((int) detail)
        .array();
  }

  static byte[] encodeOutputsDisabledForTest(
      DisableReason reason, int busIndex, long epoch) {
    requireEpoch(epoch);
    return allocate(12)
        .putShort((short) reason.wireValue())
        .putShort((short) busIndex)
        .putLong(epoch)
        .array();
  }

  static byte[] encodePdoForTest(
      int bus,
      int subDevice,
      int offset,
      int total,
      long epoch,
      long sequence,
      byte[] data) {
    requireEpoch(epoch);
    if (data.length > MAXIMUM_PDO_CHUNK
        || offset < 0
        || total < 0
        || offset > total
        || data.length > total - offset) {
      throw new IllegalArgumentException("invalid PDO test payload");
    }
    return allocate(32 + data.length)
        .putShort((short) bus)
        .putShort((short) subDevice)
        .putInt(offset)
        .putInt(total)
        .putShort((short) data.length)
        .putShort((short) 0)
        .putLong(epoch)
        .putLong(sequence)
        .put(data)
        .array();
  }

  private static boolean knownMessageType(int type) {
    return (type >= HELLO && type <= ADAPTER_RESCAN)
        || (type >= HELLO_ACK && type <= OUTPUTS_DISABLED);
  }

  private static ByteBuffer allocate(int size) {
    return ByteBuffer.allocate(size).order(WIRE_ORDER);
  }

  private static ByteBuffer wrap(byte[] bytes) {
    return ByteBuffer.wrap(bytes).order(WIRE_ORDER);
  }

  private static int unsignedByte(byte value) {
    return Byte.toUnsignedInt(value);
  }

  private static int unsignedShort(short value) {
    return Short.toUnsignedInt(value);
  }

  private static long unsignedInt(int value) {
    return Integer.toUnsignedLong(value);
  }

  private static int readUnsignedShort(byte[] bytes, int offset) {
    return unsignedByte(bytes[offset]) | (unsignedByte(bytes[offset + 1]) << 8);
  }

  private static long readUnsignedInt(byte[] bytes, int offset) {
    return (long) unsignedByte(bytes[offset])
        | ((long) unsignedByte(bytes[offset + 1]) << 8)
        | ((long) unsignedByte(bytes[offset + 2]) << 16)
        | ((long) unsignedByte(bytes[offset + 3]) << 24);
  }

  private static String strictUtf8(byte[] encoded, String field)
      throws ProtocolException {
    try {
      CharBuffer decoded =
          StandardCharsets.UTF_8
              .newDecoder()
              .onMalformedInput(CodingErrorAction.REPORT)
              .onUnmappableCharacter(CodingErrorAction.REPORT)
              .decode(ByteBuffer.wrap(encoded));
      String value = decoded.toString();
      if (value.indexOf('\0') >= 0) {
        throw new ProtocolException(field + " contains NUL");
      }
      return value;
    } catch (CharacterCodingException error) {
      throw new ProtocolException(field + " is not valid UTF-8", error);
    }
  }

  private static byte[] utf8(String value) {
    return Objects.requireNonNull(value, "value").getBytes(StandardCharsets.UTF_8);
  }

  private static void requireLength(byte[] payload, int length, String field)
      throws ProtocolException {
    if (payload.length != length) {
      throw new ProtocolException(field + " payload must be " + length + " bytes");
    }
  }

  private static void requireEpoch(long epoch) {
    if (epoch == 0) {
      throw new IllegalArgumentException("epoch must be non-zero");
    }
  }

  private static void requireRange(int value, int minimum, int maximum, String field) {
    if (value < minimum || value > maximum) {
      throw new IllegalArgumentException(
          field + " must be between " + minimum + " and " + maximum);
    }
  }
}
