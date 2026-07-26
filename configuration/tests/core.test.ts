import { describe, expect, test } from "bun:test";
import {
  DEFAULT_CONFIG,
  FixedWindowRateLimiter,
  FrameDecoder,
  IpcType,
  MAX_IPC_BUFFER_BYTES,
  MAX_IPC_FRAME_BYTES,
  MAX_INTERFACE_MAPPINGS,
  MAX_OUTPUT_WRITE_BYTES,
  NonOverlappingTask,
  ProtocolError,
  ValidationError,
  adapterIdentityMatches,
  boundedBackoffDelay,
  deriveAdapterLockState,
  deriveCanonicalSysfsIdPath,
  encodeIpcFrame,
  makeObserverHelloFrame,
  parseBusInfo,
  parseHelloAck,
  parseMainDeviceStatus,
  validateConfig
} from "../core";

function bytes(hex: string): Uint8Array {
  return Uint8Array.from(
    hex.replace(/\s+/g, "").match(/.{2}/g)?.map((value) => Number.parseInt(value, 16)) ?? []
  );
}

describe("ECSC v1 framing", () => {
  test("observer HELLO matches the cross-language golden bytes", () => {
    expect(makeObserverHelloFrame()).toEqual(bytes(`
      18 00 00 00
      45 43 53 43 01 01 00 00 00 00 00 00
      00 00 e8 03 00 00 00 00 00 00 00 00
    `));
  });

  test("request ID and type use the specified wire byte order", () => {
    expect(encodeIpcFrame(IpcType.CLEAR_COUNTERS, 0x11223344)).toEqual(bytes(`
      0c 00 00 00
      45 43 53 43 01 05 00 00 44 33 22 11
    `));
  });

  test("decodes partial and coalesced frames without losing boundaries", () => {
    const first = encodeIpcFrame(IpcType.ACK, 1, new Uint8Array(8));
    const second = encodeIpcFrame(IpcType.ERROR, 2, new Uint8Array(8));
    const combined = new Uint8Array(first.byteLength + second.byteLength);
    combined.set(first);
    combined.set(second, first.byteLength);
    const decoder = new FrameDecoder();

    expect(decoder.push(combined.slice(0, 3))).toEqual([]);
    expect(decoder.push(combined.slice(3, first.byteLength + 5))).toEqual([
      {
        version: 1,
        type: IpcType.ACK,
        flags: 0,
        requestId: 1,
        payload: new Uint8Array(8)
      }
    ]);
    expect(decoder.push(combined.slice(first.byteLength + 5))).toEqual([
      {
        version: 1,
        type: IpcType.ERROR,
        flags: 0,
        requestId: 2,
        payload: new Uint8Array(8)
      }
    ]);
    expect(decoder.bufferedBytes).toBe(0);
  });

  test("rejects invalid length, magic, version, flags, and oversized buffering", () => {
    expect(() => new FrameDecoder().push(bytes("0b000000"))).toThrow(ProtocolError);
    const badMagic = encodeIpcFrame(IpcType.ACK, 1, new Uint8Array(8));
    badMagic[4] = 0;
    expect(() => new FrameDecoder().push(badMagic)).toThrow("magic");
    const badVersion = encodeIpcFrame(IpcType.ACK, 1, new Uint8Array(8));
    badVersion[8] = 2;
    expect(() => new FrameDecoder().push(badVersion)).toThrow("version");
    const badFlags = encodeIpcFrame(IpcType.ACK, 1, new Uint8Array(8));
    badFlags[10] = 1;
    expect(() => new FrameDecoder().push(badFlags)).toThrow("flags");
    expect(() => encodeIpcFrame(0x7f, 1)).toThrow("metadata");
    expect(() => new FrameDecoder().push(new Uint8Array(MAX_IPC_BUFFER_BYTES + 1)))
      .toThrow("buffer");
  });

  test("uses the C++ two-maximum-frame receive boundary exactly", () => {
    expect(MAX_IPC_BUFFER_BYTES).toBe(16_392);
    const payload = new Uint8Array(MAX_IPC_FRAME_BYTES - 12);
    const first = encodeIpcFrame(IpcType.ACK, 1, payload);
    const second = encodeIpcFrame(IpcType.ERROR, 2, payload);
    const combined = new Uint8Array(first.byteLength + second.byteLength);
    combined.set(first);
    combined.set(second, first.byteLength);
    expect(combined.byteLength).toBe(MAX_IPC_BUFFER_BYTES);
    expect(new FrameDecoder().push(combined)).toHaveLength(2);
  });
});

describe("daemon payload parsing", () => {
  test("parses STATUS timing, epoch, and scheduling metadata honestly", () => {
    const payload = new Uint8Array(56);
    const view = new DataView(payload.buffer);
    payload[0] = 4;
    payload[1] = 0b0111_1001;
    view.setUint16(2, 2, true);
    view.setUint16(4, 7, true);
    view.setUint16(6, 1, true);
    view.setUint32(8, 450, true);
    view.setUint32(12, 75, true);
    view.setBigUint64(16, 9n, true);
    view.setBigUint64(24, 3n, true);
    view.setBigUint64(32, 0x0102030405060708n, true);
    view.setUint32(40, 1234, true);
    view.setUint16(44, 2, true);
    payload[46] = 1;
    payload[47] = 0b0100_1010;
    view.setBigUint64(48, 99n, true);

    expect(parseMainDeviceStatus(payload, 123)).toEqual({
      aggregateState: 4,
      operational: true,
      outputsEnabled: false,
      controllerConnected: false,
      preemptRtAvailable: true,
      realtimeApplied: true,
      realtimeRequested: true,
      timingDegraded: true,
      reinitializing: false,
      schedulingMode: "fifo",
      realtimeErrorBits: 74,
      distributedClockUnlocked: true,
      activeAdapters: 2,
      subDeviceCount: 7,
      activeFaults: 1,
      maxJitterUs: 450,
      currentJitterUs: 75,
      lostFrames: "9",
      cycleOverruns: "3",
      daemonEpoch: "72623859790382856",
      controllerPid: 1234,
      configuredBuses: 2,
      monotonicUs: "99",
      updatedAt: 123,
      stale: false,
      connected: true
    });
    const reservedRealtimeBit = payload.slice();
    reservedRealtimeBit[47] = 0x80;
    expect(() => parseMainDeviceStatus(reservedRealtimeBit)).toThrow("scheduling metadata");
    const invalidState = payload.slice();
    invalidState[0] = 7;
    expect(() => parseMainDeviceStatus(invalidState)).toThrow("aggregate state");
  });

  test("rejects a HELLO_ACK with no heartbeat timeout", () => {
    const payload = new Uint8Array(20);
    new DataView(payload.buffer).setBigUint64(4, 1n, true);
    expect(() => parseHelloAck(payload)).toThrow("heartbeat timeout");
  });

  test("parses bounded BUS_INFO and path-only identity", () => {
    const fields = ["Drive", "eth9", "pci-0000:01:00.0-usb-0:1:1.0", "", "ABC", "1234", "5678"];
    const encoded = fields.map((value) => new TextEncoder().encode(value));
    const payload = new Uint8Array(30 + encoded.reduce((sum, item) => sum + item.length, 0));
    const view = new DataView(payload.buffer);
    view.setUint16(0, 3, true);
    payload[2] = 4;
    payload[3] = 1;
    payload[4] = 1;
    payload[5] = 9;
    view.setUint16(6, 5, true);
    let offset = 30;
    encoded.forEach((item, index) => {
      view.setUint16(8 + index * 2, item.length, true);
      payload.set(item, offset);
      offset += item.length;
    });
    view.setBigUint64(22, 42n, true);

    expect(parseBusInfo(payload)).toEqual({
      busIndex: 3,
      state: 4,
      linkUp: true,
      lockState: "matched",
      lockReason: 9,
      slaveCount: 5,
      logicalName: "Drive",
      physicalInterface: "eth9",
      identity: {
        id_path: "pci-0000:01:00.0-usb-0:1:1.0",
        usb_serial: "ABC",
        usb_vendor_id: "1234",
        usb_product_id: "5678"
      },
      epoch: "42"
    });
  });
});

describe("strict configuration and stable adapter identity", () => {
  test("uses the daemon's exact stable sysfs identity precedence and fallback format", () => {
    expect(deriveCanonicalSysfsIdPath({
      sysfsRoot: "/sys",
      devicePath: "/sys/devices/platform/axi/usb1/1-1",
      udevData: "E:ID_SERIAL_SHORT=ABC\nE:ID_PATH=platform-udev-usb-0:1:1.0\n",
      interfaceIdPath: "ignored-interface-path"
    })).toBe("platform-udev-usb-0:1:1.0");
    expect(deriveCanonicalSysfsIdPath({
      sysfsRoot: "/sys",
      devicePath: "/sys/devices/platform/axi/usb1/1-1",
      interfaceIdPath: "platform-axi-usb-0:1:1.0\n",
      deviceIdPath: "ignored-device-path",
      deviceUevent: "ID_PATH=ignored-uevent-path\n"
    })).toBe("platform-axi-usb-0:1:1.0");
    expect(deriveCanonicalSysfsIdPath({
      sysfsRoot: "/sys",
      devicePath: "/sys/devices/platform/axi/usb1/1-1",
      deviceUevent: "DRIVER=cdc_ether\nID_PATH=platform-axi-usb-0:1:1.0\n"
    })).toBe("platform-axi-usb-0:1:1.0");
    expect(deriveCanonicalSysfsIdPath({
      sysfsRoot: "/sys",
      devicePath: "/sys/devices/platform/axi/usb1/1-1"
    })).toBe("sysfs:devices/platform/axi/usb1/1-1");
    expect(deriveCanonicalSysfsIdPath({
      sysfsRoot: "/sys",
      devicePath: "/outside/device"
    })).toBe("");
  });

  test("accepts the daemon's exact fields, enabled=false, and stable locks", () => {
    const config = validateConfig({
      ...DEFAULT_CONFIG,
      cycle_period_us: 5_000,
      nt4_server: "10.0.0.2",
      nt4_team: 1234,
      heartbeat_timeout_ms: 400,
      output_command_timeout_ms: 80,
      realtime_memory_lock: true,
      realtime_fifo: true,
      realtime_priority: 55,
      realtime_cpu: 3,
      controller_group: "ec-systemcore-controller",
      controller_uids: [1000, 0, 1000],
      controller_gids: [999, 998, 999],
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: false,
        lock: { id_path: "usb-0:1:1.0" },
        maximum_io_map_bytes: 65_536,
        distributed_clock: true,
        distributed_clock_shift_ns: -250,
        allow_unverified_topology: true,
        expected_subdevices: [{
          vendor_id: 0x11223344,
          product_code: 0x55667788,
          revision: 3,
          output_bytes: 16,
          input_bytes: 32
        }]
      }]
    });
    expect(config.cycle_period_us).toBe(5_000);
    expect(Object.hasOwn(config, "nt4_server")).toBe(false);
    expect(Object.hasOwn(config, "nt4_team")).toBe(false);
    expect(config.interface_mappings[0]?.enabled).toBe(false);
    expect(config.interface_mappings[0]?.lock).toEqual({ id_path: "usb-0:1:1.0" });
    expect(config.interface_mappings[0]?.maximum_io_map_bytes).toBe(65_536);
    expect(config.interface_mappings[0]?.distributed_clock).toBe(true);
    expect(config.interface_mappings[0]?.allow_unverified_topology).toBe(true);
    expect(config.interface_mappings[0]?.expected_subdevices?.[0]?.product_code)
      .toBe(0x55667788);
    expect(config.heartbeat_timeout_ms).toBe(400);
    expect(config.output_command_timeout_ms).toBe(80);
    expect(config.realtime_cpu).toBe(3);
    expect(config.controller_uids).toEqual([0, 1000]);
    expect(config.controller_gids).toEqual([998, 999]);
  });

  test("rejects coercion, unsafe cycles, fixed-path changes, and unknown keys", () => {
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      allow_restricted_interfaces: "false"
    })).toThrow(ValidationError);
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      cycle_period_us: 4_999
    })).toThrow("5000");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      log_directory: "/tmp/logs"
    })).toThrow("exactly");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      controller_group: "invalid group"
    })).toThrow("controller_group");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      controller_uids: [1.5]
    })).toThrow("controller_uids");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      heartbeat_timeout_ms: 100,
      output_command_timeout_ms: 101
    })).toThrow("output_command_timeout_ms");
    expect(validateConfig({
      ...DEFAULT_CONFIG,
      cycle_period_us: 10_000,
      output_command_timeout_ms: 20
    }).output_command_timeout_ms).toBe(20);
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      cycle_period_us: 10_001,
      output_command_timeout_ms: 20
    })).toThrow("at least two cycle_period_us");
    const polluted = JSON.parse('{"__proto__":{"polluted":true}}');
    expect(() => validateConfig({ ...DEFAULT_CONFIG, ...polluted })).toThrow("forbidden");
    expect(() => validateConfig({ ...DEFAULT_CONFIG, future_root: true })).toThrow("unknown key");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: true,
        future_bus: true
      }]
    })).toThrow("unknown key");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: true,
        lock: { id_path: "path", future_lock: true }
      }]
    })).toThrow("unknown key");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: true,
        expected_subdevices: [{ vendor_id: 1, product_code: 2, future_slave: true }]
      }]
    })).toThrow("unknown key");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: Array.from({ length: MAX_INTERFACE_MAPPINGS + 1 }, (_, index) => ({
        logical_name: `Bus${index}`,
        physical_interface: `eth${index + 1}`,
        enabled: true
      }))
    })).toThrow(`at most ${MAX_INTERFACE_MAPPINGS}`);
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: true,
        lock: { id_path: "path", permanent_mac: "01:00:5e:00:00:01" }
      }]
    })).toThrow("unicast");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: true,
        lock: { id_path: "path", usb_serial: "serial-\u00e9" }
      }]
    })).toThrow("printable ASCII");
    expect(() => validateConfig({
      ...DEFAULT_CONFIG,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: true,
        expected_subdevices: [{
          vendor_id: 1,
          product_code: 2,
          output_bytes: MAX_OUTPUT_WRITE_BYTES + 1
        }]
      }]
    })).toThrow("output_bytes");
  });

  test("matches locks independently of transient interface name and fails closed", () => {
    const expected = { id_path: "usb-port-1", permanent_mac: "02:00:00:00:00:01" };
    const replacement = { id_path: "usb-port-1", permanent_mac: "02:00:00:00:00:02" };
    const reappeared = { id_path: "usb-port-1", permanent_mac: "02:00:00:00:00:01" };
    expect(adapterIdentityMatches(expected, reappeared)).toBe(true);
    expect(deriveAdapterLockState(expected, [], "eth1")).toBe("missing");
    expect(deriveAdapterLockState(expected, [{ name: "eth1", identity: replacement }], "eth1"))
      .toBe("mismatch");
    expect(deriveAdapterLockState(expected, [{ name: "enx123", identity: reappeared }], "eth1"))
      .toBe("matched");
    expect(deriveAdapterLockState(expected, [
      { name: "enx123", identity: reappeared },
      { name: "enx456", identity: reappeared }
    ], "eth1")).toBe("mismatch");
    expect(deriveAdapterLockState(expected, [{
      name: "enx456",
      identity: { id_path: "new-path", permanent_mac: "02:00:00:00:00:01" }
    }], "eth1")).toBe("mismatch");
  });
});

describe("bounded operational helpers", () => {
  test("backoff clamps invalid attempts and jitter", () => {
    expect(boundedBackoffDelay(Number.NaN)).toBe(250);
    expect(boundedBackoffDelay(-10)).toBe(250);
    expect(boundedBackoffDelay(99)).toBe(5_000);
    expect(boundedBackoffDelay(1, { jitter: Number.NaN })).toBe(500);
  });

  test("rate limiter resets only after its fixed window", () => {
    const limiter = new FixedWindowRateLimiter(2, 1000);
    expect(limiter.allow("client", 0)).toBe(true);
    expect(limiter.allow("client", 1)).toBe(true);
    expect(limiter.allow("client", 2)).toBe(false);
    expect(limiter.allow("client", 1000)).toBe(true);
  });

  test("non-overlapping task guard drops concurrent timer work and always resets", async () => {
    const guard = new NonOverlappingTask();
    let release: (() => void) | undefined;
    const first = guard.run(async () => {
      await new Promise<void>((resolve) => {
        release = resolve;
      });
    });
    expect(guard.running).toBe(true);
    expect(await guard.run(async () => undefined)).toBe(false);
    release?.();
    expect(await first).toBe(true);
    expect(guard.running).toBe(false);
    await expect(guard.run(async () => {
      throw new Error("task failure");
    })).rejects.toThrow("task failure");
    expect(guard.running).toBe(false);
  });
});
