import { isAbsolute, relative, resolve } from "node:path";

export const SYSTEM_VERSION = process.env.EC_SYSTEMCORE_VERSION ?? "development";
export const FRAME_HEADER_BYTES = 4;
export const IPC_ENVELOPE_BYTES = 12;
export const MIN_IPC_FRAME_BYTES = IPC_ENVELOPE_BYTES;
export const MAX_IPC_FRAME_BYTES = 8192;
export const MAX_IPC_BUFFER_BYTES = 2 * (FRAME_HEADER_BYTES + MAX_IPC_FRAME_BYTES);
export const IPC_VERSION = 1;
export const IPC_MAGIC = new Uint8Array([0x45, 0x43, 0x53, 0x43]); // ECSC
export const MAX_INTERFACE_MAPPINGS = 8;
export const MAX_LOG_COUNT = 1_000;
export const MAX_FREE_SPACE_THRESHOLD_MB = 1_048_576;
export const MIN_CYCLE_PERIOD_US = 5_000;
export const MAX_CYCLE_PERIOD_US = 1_000_000;
export const MAX_EXPECTED_SUBDEVICES = 199;
export const MAX_OUTPUT_WRITE_BYTES = 1_024;

export type AdapterLockIdentity = {
  id_path: string;
  permanent_mac?: string;
  usb_serial?: string;
  usb_vendor_id?: string;
  usb_product_id?: string;
};

export type ExpectedSubDevice = {
  vendor_id: number;
  product_code: number;
  revision?: number;
  output_bytes?: number;
  input_bytes?: number;
};

export type InterfaceMapping = {
  logical_name: string;
  physical_interface: string;
  enabled: boolean;
  lock?: AdapterLockIdentity;
  maximum_io_map_bytes?: number;
  distributed_clock?: boolean;
  distributed_clock_shift_ns?: number;
  allow_unverified_topology?: boolean;
  expected_subdevices?: ExpectedSubDevice[];
};

export type ConfigurationConfig = {
  allow_restricted_interfaces: boolean;
  cycle_period_us: number;
  log_directory: string;
  log_count_limit: number;
  free_space_threshold_mb: number;
  interface_mappings: InterfaceMapping[];
  heartbeat_timeout_ms: number;
  output_command_timeout_ms: number;
  realtime_memory_lock?: boolean;
  realtime_fifo?: boolean;
  realtime_priority?: number;
  realtime_cpu?: number;
  controller_group: string;
  controller_uids: number[];
  controller_gids: number[];
};

export type MainDeviceStatus = {
  aggregateState: number;
  operational: boolean;
  outputsEnabled: boolean;
  controllerConnected: boolean;
  preemptRtAvailable: boolean;
  realtimeApplied: boolean;
  realtimeRequested: boolean;
  timingDegraded: boolean;
  reinitializing: boolean;
  schedulingMode: "standard" | "fifo";
  realtimeErrorBits: number;
  distributedClockUnlocked: boolean;
  activeAdapters: number;
  subDeviceCount: number;
  activeFaults: number;
  maxJitterUs: number;
  currentJitterUs: number;
  lostFrames: string;
  cycleOverruns: string;
  daemonEpoch: string;
  controllerPid: number;
  configuredBuses: number;
  monotonicUs: string;
  updatedAt: number;
  stale: boolean;
  connected: boolean;
};

export type IpcFrame = {
  version: number;
  type: number;
  flags: number;
  requestId: number;
  payload: Uint8Array;
};

export type HelloAck = {
  grantedRole: number;
  outputsEnabled: boolean;
  heartbeatTimeoutMs: number;
  epoch: string;
  peerPid: number;
  capabilities: number;
};

export type IpcAck = {
  status: number;
  detail: number;
};

export type BusInfo = {
  busIndex: number;
  state: number;
  linkUp: boolean;
  lockState: "unlocked" | "matched" | "missing" | "mismatch" | "runtime_unlocked" | "unknown";
  slaveCount: number;
  logicalName: string;
  physicalInterface: string;
  identity: AdapterLockIdentity | null;
  lockReason: number;
  epoch: string;
};

export const IpcType = {
  HELLO: 0x01,
  HEARTBEAT: 0x02,
  OUTPUT_ENABLE: 0x03,
  OUTPUT_WRITE: 0x04,
  CLEAR_COUNTERS: 0x05,
  RELEASE_CONTROL: 0x06,
  SUBSCRIBE_INPUTS: 0x07,
  ADAPTER_UNLOCK: 0x08,
  ADAPTER_RESCAN: 0x09,
  HELLO_ACK: 0x80,
  STATUS: 0x81,
  ACK: 0x82,
  PDO_INPUT: 0x83,
  BUS_INFO: 0x84,
  ERROR: 0x85,
  OUTPUTS_DISABLED: 0x86
} as const;

const KNOWN_IPC_TYPES = new Set<number>(Object.values(IpcType));

export const RESTRICTED_INTERFACES = new Set(["eth0", "wlan0", "usb0", "lo"]);

export const DEFAULT_CONFIG: ConfigurationConfig = {
  allow_restricted_interfaces: false,
  cycle_period_us: 5_000,
  log_directory: "/var/log/ec-systemcore",
  log_count_limit: 10,
  free_space_threshold_mb: 50,
  heartbeat_timeout_ms: 250,
  output_command_timeout_ms: 100,
  controller_group: "ec-systemcore-controller",
  controller_uids: [0],
  controller_gids: [],
  interface_mappings: []
};

const KNOWN_CONFIG_KEYS = new Set([
  "allow_restricted_interfaces",
  "cycle_period_us",
  "log_directory",
  "log_count_limit",
  "free_space_threshold_mb",
  "interface_mappings",
  "heartbeat_timeout_ms",
  "output_command_timeout_ms",
  "realtime_memory_lock",
  "realtime_fifo",
  "realtime_priority",
  "realtime_cpu",
  "controller_group",
  "controller_uids",
  "controller_gids"
]);

const REMOVED_LEGACY_CONFIG_KEYS = new Set(["nt4_server", "nt4_team"]);

const KNOWN_MAPPING_KEYS = new Set([
  "logical_name",
  "physical_interface",
  "enabled",
  "lock",
  "maximum_io_map_bytes",
  "distributed_clock",
  "distributed_clock_shift_ns",
  "allow_unverified_topology",
  "expected_subdevices"
]);

const KNOWN_LOCK_KEYS = new Set([
  "id_path",
  "permanent_mac",
  "usb_serial",
  "usb_vendor_id",
  "usb_product_id"
]);

const KNOWN_EXPECTED_SUBDEVICE_KEYS = new Set([
  "vendor_id",
  "product_code",
  "revision",
  "output_bytes",
  "input_bytes"
]);

export class ValidationError extends Error {
  readonly code = "INVALID_REQUEST";

  constructor(message: string) {
    super(message);
    this.name = "ValidationError";
  }
}

export class ProtocolError extends Error {
  readonly code = "IPC_PROTOCOL_ERROR";

  constructor(message: string) {
    super(message);
    this.name = "ProtocolError";
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function requireBoolean(value: unknown, field: string, fallback?: boolean): boolean {
  if (value === undefined && fallback !== undefined) return fallback;
  if (typeof value !== "boolean") throw new ValidationError(`${field} must be a boolean`);
  return value;
}

function requireInteger(
  value: unknown,
  field: string,
  minimum: number,
  maximum: number,
  fallback?: number
): number {
  if (value === undefined && fallback !== undefined) return fallback;
  if (!Number.isInteger(value) || (value as number) < minimum || (value as number) > maximum) {
    throw new ValidationError(`${field} must be an integer from ${minimum} to ${maximum}`);
  }
  return value as number;
}

function containsControlCharacters(value: string): boolean {
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code <= 0x1f || code === 0x7f) return true;
  }
  return false;
}

function requireString(
  value: unknown,
  field: string,
  maximumLength: number,
  fallback?: string
): string {
  if (value === undefined && fallback !== undefined) return fallback;
  if (typeof value !== "string" || value.length > maximumLength) {
    throw new ValidationError(`${field} must be a string of at most ${maximumLength} characters`);
  }
  if (containsControlCharacters(value)) {
    throw new ValidationError(`${field} contains control characters`);
  }
  return value;
}

function requireIdArray(
  value: unknown,
  field: string,
  fallback: number[],
  includeRoot = false
): number[] {
  const selected = value === undefined ? fallback : value;
  if (!Array.isArray(selected) || selected.length > 64) {
    throw new ValidationError(`${field} must be an array with at most 64 entries`);
  }
  const ids = selected.map((item, index) =>
    requireInteger(item, `${field}[${index}]`, 0, 0xffff_ffff)
  );
  if (includeRoot) ids.push(0);
  return [...new Set(ids)].sort((left, right) => left - right);
}

const FORBIDDEN_JSON_KEYS = new Set(["__proto__", "prototype", "constructor"]);

function rejectUnknownKeys(
  value: Record<string, unknown>,
  known: ReadonlySet<string>,
  field: string,
  additionallyAllowed: ReadonlySet<string> = new Set()
): void {
  for (const key of Object.keys(value)) {
    if (FORBIDDEN_JSON_KEYS.has(key)) {
      throw new ValidationError(`${field} contains a forbidden key`);
    }
    if (!known.has(key) && !additionallyAllowed.has(key)) {
      throw new ValidationError(`${field} contains unknown key ${key}`);
    }
  }
}

export function normalizeMac(value: unknown, field: string): string {
  const text = requireString(value, field, 64);
  let hexadecimal = "";
  for (const character of text) {
    if (/^[0-9a-f]$/i.test(character)) hexadecimal += character.toLowerCase();
    else if (character !== ":" && character !== "-" && !/\s/.test(character)) {
      throw new ValidationError(`${field} must be a unicast MAC address`);
    }
  }
  if (hexadecimal.length !== 12) {
    throw new ValidationError(`${field} must be a unicast MAC address`);
  }
  const firstByte = Number.parseInt(hexadecimal.slice(0, 2), 16);
  if (
    (firstByte & 1) !== 0
    || hexadecimal === "000000000000"
    || hexadecimal === "ffffffffffff"
  ) {
    throw new ValidationError(`${field} must be a unicast MAC address`);
  }
  return hexadecimal.match(/.{2}/g)!.join(":");
}

function normalizeUsbId(value: unknown, field: string): string | undefined {
  if (value === undefined) return undefined;
  const raw = requireString(value, field, 6).trim();
  const text = raw.replace(/^0x/i, "").toLowerCase();
  if (!/^[0-9a-f]{4}$/.test(text)) {
    throw new ValidationError(`${field} must be a four-digit hexadecimal USB ID`);
  }
  return text;
}

export function validateAdapterLock(value: unknown, field = "lock"): AdapterLockIdentity {
  if (!isRecord(value)) throw new ValidationError(`${field} must be an object`);
  rejectUnknownKeys(value, KNOWN_LOCK_KEYS, field);
  const idPath = requireString(value.id_path, `${field}.id_path`, 1_024).trim();
  if (!idPath || [...idPath].some((character) => {
    const byte = character.charCodeAt(0);
    return byte < 0x20 || byte > 0x7e;
  })) {
    throw new ValidationError(`${field}.id_path is not a valid stable device path`);
  }
  const result: AdapterLockIdentity = {
    id_path: idPath
  };
  if (value.permanent_mac !== undefined && value.permanent_mac !== "") {
    result.permanent_mac = normalizeMac(value.permanent_mac, `${field}.permanent_mac`);
  }
  if (value.usb_serial !== undefined) {
    const serial = requireString(value.usb_serial, `${field}.usb_serial`, 256).trim();
    if (
      !serial
      || [...serial].some((character) => {
        const byte = character.charCodeAt(0);
        return byte < 0x20 || byte > 0x7e;
      })
    ) {
      throw new ValidationError(`${field}.usb_serial must contain printable ASCII`);
    }
    result.usb_serial = serial;
  }
  const vendor = normalizeUsbId(value.usb_vendor_id, `${field}.usb_vendor_id`);
  const product = normalizeUsbId(value.usb_product_id, `${field}.usb_product_id`);
  if (vendor) result.usb_vendor_id = vendor;
  if (product) result.usb_product_id = product;
  return result;
}

export function adapterIdentityMatches(
  expected: AdapterLockIdentity,
  observed: AdapterLockIdentity | null
): boolean {
  if (!observed) return false;
  if (expected.id_path !== observed.id_path) return false;
  if (
    expected.permanent_mac !== undefined
    && expected.permanent_mac.toLowerCase() !== observed.permanent_mac?.toLowerCase()
  ) return false;
  if (expected.usb_serial !== undefined && expected.usb_serial !== observed.usb_serial) return false;
  if (expected.usb_vendor_id !== undefined && expected.usb_vendor_id !== observed.usb_vendor_id) return false;
  if (expected.usb_product_id !== undefined && expected.usb_product_id !== observed.usb_product_id) return false;
  return true;
}

export function deriveAdapterLockState(
  expected: AdapterLockIdentity,
  observed: Array<{ name: string; identity: AdapterLockIdentity | null }>,
  preferredInterface: string
): "matched" | "missing" | "mismatch" {
  const pathMatches = observed.filter((item) => item.identity?.id_path === expected.id_path);
  if (pathMatches.length !== 1) {
    if (pathMatches.length > 1) return "mismatch";
    const normalizedExpectedMac = expected.permanent_mac?.toLowerCase();
    const relatedIdentity = observed.some((item) =>
      (
        normalizedExpectedMac !== undefined
        && item.identity?.permanent_mac?.toLowerCase() === normalizedExpectedMac
      )
      || (
        expected.usb_serial !== undefined
        && item.identity?.usb_serial === expected.usb_serial
      )
    );
    return observed.some((item) => item.name === preferredInterface) || relatedIdentity
      ? "mismatch"
      : "missing";
  }
  return adapterIdentityMatches(expected, pathMatches[0]!.identity) ? "matched" : "mismatch";
}

function validateMapping(
  value: unknown,
  index: number,
  allowRestricted: boolean
): InterfaceMapping {
  const field = `interface_mappings[${index}]`;
  if (!isRecord(value)) throw new ValidationError(`${field} must be an object`);
  rejectUnknownKeys(value, KNOWN_MAPPING_KEYS, field);
  const logicalName = requireString(value.logical_name, `${field}.logical_name`, 32).trim();
  if (!/^[A-Za-z0-9_-]+$/.test(logicalName)) {
    throw new ValidationError(`${field}.logical_name is invalid`);
  }
  const physicalInterface = requireString(
    value.physical_interface,
    `${field}.physical_interface`,
    15
  ).trim();
  if (!/^[A-Za-z0-9_.:-]+$/.test(physicalInterface)) {
    throw new ValidationError(`${field}.physical_interface is invalid`);
  }
  if (!allowRestricted && RESTRICTED_INTERFACES.has(physicalInterface)) {
    throw new ValidationError(
      `${field}.physical_interface is restricted; explicitly enable restricted interfaces first`
    );
  }
  const mapping: InterfaceMapping = {
    logical_name: logicalName,
    physical_interface: physicalInterface,
    enabled: requireBoolean(value.enabled, `${field}.enabled`, true)
  };
  if (value.lock !== undefined) mapping.lock = validateAdapterLock(value.lock, `${field}.lock`);
  if (value.maximum_io_map_bytes !== undefined) {
    mapping.maximum_io_map_bytes = requireInteger(
      value.maximum_io_map_bytes,
      `${field}.maximum_io_map_bytes`,
      1_024,
      1_048_576
    );
  }
  if (value.distributed_clock !== undefined) {
    mapping.distributed_clock = requireBoolean(
      value.distributed_clock,
      `${field}.distributed_clock`
    );
  }
  if (value.distributed_clock_shift_ns !== undefined) {
    mapping.distributed_clock_shift_ns = requireInteger(
      value.distributed_clock_shift_ns,
      `${field}.distributed_clock_shift_ns`,
      -2_147_483_648,
      2_147_483_647
    );
  }
  if (value.allow_unverified_topology !== undefined) {
    mapping.allow_unverified_topology = requireBoolean(
      value.allow_unverified_topology,
      `${field}.allow_unverified_topology`
    );
  }
  if (value.expected_subdevices !== undefined) {
    if (
      !Array.isArray(value.expected_subdevices)
      || value.expected_subdevices.length > MAX_EXPECTED_SUBDEVICES
    ) {
      throw new ValidationError(
        `${field}.expected_subdevices must contain at most ${MAX_EXPECTED_SUBDEVICES} entries`
      );
    }
    mapping.expected_subdevices = value.expected_subdevices.map((item, subdeviceIndex) => {
      const subField = `${field}.expected_subdevices[${subdeviceIndex}]`;
      if (!isRecord(item)) throw new ValidationError(`${subField} must be an object`);
      rejectUnknownKeys(item, KNOWN_EXPECTED_SUBDEVICE_KEYS, subField);
      const subdevice: ExpectedSubDevice = {
        vendor_id: requireInteger(item.vendor_id, `${subField}.vendor_id`, 0, 0xffff_ffff),
        product_code: requireInteger(item.product_code, `${subField}.product_code`, 0, 0xffff_ffff)
      };
      for (const optional of ["revision", "output_bytes", "input_bytes"] as const) {
        if (item[optional] !== undefined) {
          subdevice[optional] = requireInteger(
            item[optional],
            `${subField}.${optional}`,
            0,
            optional === "output_bytes" ? MAX_OUTPUT_WRITE_BYTES : 0xffff_ffff
          );
        }
      }
      return subdevice;
    });
  }
  return mapping;
}

function comparableResolvedPath(path: string): string {
  const resolved = resolve(path);
  if (resolved.startsWith("\\\\?\\UNC\\")) return `\\\\${resolved.slice(8)}`;
  if (resolved.startsWith("\\\\?\\")) return resolved.slice(4);
  return resolved;
}

export function isPathWithin(root: string, candidate: string): boolean {
  const resolvedRoot = comparableResolvedPath(root);
  const resolvedCandidate = comparableResolvedPath(candidate);
  const rel = relative(resolvedRoot, resolvedCandidate);
  return rel === "" || (!rel.startsWith("..") && !isAbsolute(rel));
}

function firstNonEmptyLine(value: string | undefined): string {
  return value?.split(/\r?\n/, 1)[0]?.trim() ?? "";
}

export function deriveCanonicalSysfsIdPath(options: {
  sysfsRoot: string;
  devicePath: string;
  udevData?: string;
  interfaceIdPath?: string;
  deviceIdPath?: string;
  deviceUevent?: string;
}): string {
  const udevPrefix = "E:ID_PATH=";
  for (const line of (options.udevData ?? "").split(/\r?\n/)) {
    if (line.startsWith(udevPrefix) && line.length > udevPrefix.length) {
      return line.slice(udevPrefix.length).trim();
    }
  }
  const interfaceIdPath = firstNonEmptyLine(options.interfaceIdPath);
  if (interfaceIdPath) return interfaceIdPath;
  const deviceIdPath = firstNonEmptyLine(options.deviceIdPath);
  if (deviceIdPath) return deviceIdPath;
  const ueventPrefix = "ID_PATH=";
  for (const line of (options.deviceUevent ?? "").split(/\r?\n/)) {
    if (line.startsWith(ueventPrefix)) {
      return line.slice(ueventPrefix.length);
    }
  }
  if (!options.devicePath || !isPathWithin(options.sysfsRoot, options.devicePath)) return "";
  const relativePath = relative(
    comparableResolvedPath(options.sysfsRoot),
    comparableResolvedPath(options.devicePath)
  )
    .replaceAll("\\", "/");
  return relativePath
    ? `sysfs:${relativePath}`
    : `sysfs:${comparableResolvedPath(options.devicePath).replaceAll("\\", "/")}`;
}

export function validateConfig(
  value: unknown,
  options: { logRoot?: string } = {}
): ConfigurationConfig {
  if (!isRecord(value)) throw new ValidationError("configuration must be a JSON object");
  rejectUnknownKeys(value, KNOWN_CONFIG_KEYS, "configuration", REMOVED_LEGACY_CONFIG_KEYS);

  const allowRestricted = requireBoolean(
    value.allow_restricted_interfaces,
    "allow_restricted_interfaces",
    DEFAULT_CONFIG.allow_restricted_interfaces
  );
  const rawMappings = value.interface_mappings ?? DEFAULT_CONFIG.interface_mappings;
  if (!Array.isArray(rawMappings) || rawMappings.length > MAX_INTERFACE_MAPPINGS) {
    throw new ValidationError(
      `interface_mappings must be an array with at most ${MAX_INTERFACE_MAPPINGS} entries`
    );
  }
  const interfaceMappings = rawMappings.map((item, index) =>
    validateMapping(item, index, allowRestricted)
  );

  const logicalNames = new Set<string>();
  const physicalNames = new Set<string>();
  for (const mapping of interfaceMappings) {
    if (logicalNames.has(mapping.logical_name)) {
      throw new ValidationError(`duplicate logical interface ${mapping.logical_name}`);
    }
    logicalNames.add(mapping.logical_name);
    if (physicalNames.has(mapping.physical_interface)) {
      throw new ValidationError(`duplicate physical interface ${mapping.physical_interface}`);
    }
    physicalNames.add(mapping.physical_interface);
  }

  const logDirectory = requireString(
    value.log_directory,
    "log_directory",
    4_096,
    DEFAULT_CONFIG.log_directory
  ).trim();
  if (!isAbsolute(logDirectory) || logDirectory === "/") {
    throw new ValidationError("log_directory must be an absolute non-root directory");
  }
  const logRoot = options.logRoot ?? DEFAULT_CONFIG.log_directory;
  if (resolve(logDirectory) !== resolve(logRoot)) {
    throw new ValidationError(`log_directory must be exactly ${logRoot}`);
  }

  const heartbeatTimeoutMs = requireInteger(
    value.heartbeat_timeout_ms,
    "heartbeat_timeout_ms",
    100,
    5_000,
    DEFAULT_CONFIG.heartbeat_timeout_ms
  );
  const cyclePeriodUs = requireInteger(
    value.cycle_period_us,
    "cycle_period_us",
    MIN_CYCLE_PERIOD_US,
    MAX_CYCLE_PERIOD_US,
    DEFAULT_CONFIG.cycle_period_us
  );
  const outputCommandTimeoutMs = requireInteger(
    value.output_command_timeout_ms,
    "output_command_timeout_ms",
    20,
    heartbeatTimeoutMs,
    DEFAULT_CONFIG.output_command_timeout_ms
  );
  if (outputCommandTimeoutMs * 1_000 < cyclePeriodUs * 2) {
    throw new ValidationError(
      "output_command_timeout_ms must cover at least two cycle_period_us intervals"
    );
  }
  const validated: ConfigurationConfig = {
    allow_restricted_interfaces: allowRestricted,
    cycle_period_us: cyclePeriodUs,
    log_directory: logDirectory,
    log_count_limit: requireInteger(
      value.log_count_limit,
      "log_count_limit",
      1,
      MAX_LOG_COUNT,
      DEFAULT_CONFIG.log_count_limit
    ),
    free_space_threshold_mb: requireInteger(
      value.free_space_threshold_mb,
      "free_space_threshold_mb",
      0,
      MAX_FREE_SPACE_THRESHOLD_MB,
      DEFAULT_CONFIG.free_space_threshold_mb
    ),
    controller_group: requireString(
      value.controller_group,
      "controller_group",
      255,
      DEFAULT_CONFIG.controller_group
    ).trim(),
    controller_uids: requireIdArray(
      value.controller_uids,
      "controller_uids",
      DEFAULT_CONFIG.controller_uids,
      true
    ),
    controller_gids: requireIdArray(
      value.controller_gids,
      "controller_gids",
      DEFAULT_CONFIG.controller_gids
    ),
    heartbeat_timeout_ms: heartbeatTimeoutMs,
    output_command_timeout_ms: outputCommandTimeoutMs,
    interface_mappings: interfaceMappings
  };
  if (!/^[A-Za-z0-9_-]+$/.test(validated.controller_group)) {
    throw new ValidationError("controller_group is invalid");
  }
  if (value.realtime_memory_lock !== undefined) {
    validated.realtime_memory_lock = requireBoolean(
      value.realtime_memory_lock,
      "realtime_memory_lock"
    );
  }
  if (value.realtime_fifo !== undefined) {
    validated.realtime_fifo = requireBoolean(value.realtime_fifo, "realtime_fifo");
  }
  if (value.realtime_priority !== undefined) {
    validated.realtime_priority = requireInteger(
      value.realtime_priority,
      "realtime_priority",
      -2_147_483_648,
      2_147_483_647
    );
  }
  if (value.realtime_cpu !== undefined) {
    validated.realtime_cpu = requireInteger(
      value.realtime_cpu,
      "realtime_cpu",
      0,
      0xffff_ffff
    );
  }
  return validated;
}

export function hasRemovedLegacyConfigKeys(value: unknown): boolean {
  return isRecord(value)
    && [...REMOVED_LEGACY_CONFIG_KEYS].some((key) =>
      Object.prototype.hasOwnProperty.call(value, key)
    );
}

export function encodeIpcFrame(
  type: number,
  requestId: number,
  payload = new Uint8Array()
): Uint8Array {
  const bodyBytes = IPC_ENVELOPE_BYTES + payload.byteLength;
  if (
    !Number.isInteger(type)
    || !KNOWN_IPC_TYPES.has(type)
    || !Number.isInteger(requestId)
    || requestId < 0
    || requestId > 0xffff_ffff
    || bodyBytes < MIN_IPC_FRAME_BYTES
    || bodyBytes > MAX_IPC_FRAME_BYTES
  ) {
    throw new ProtocolError("invalid IPC frame metadata");
  }
  const frame = new Uint8Array(FRAME_HEADER_BYTES + bodyBytes);
  const view = new DataView(frame.buffer);
  view.setUint32(0, bodyBytes, true);
  frame.set(IPC_MAGIC, FRAME_HEADER_BYTES);
  frame[8] = IPC_VERSION;
  frame[9] = type;
  view.setUint16(10, 0, true);
  view.setUint32(12, requestId, true);
  frame.set(payload, FRAME_HEADER_BYTES + IPC_ENVELOPE_BYTES);
  return frame;
}

export function makeObserverHelloFrame(lastSeenEpoch = 0n): Uint8Array {
  const payload = new Uint8Array(12);
  const view = new DataView(payload.buffer);
  payload[0] = 0; // Observer; the configuration must never acquire controller authority.
  payload[1] = 0; // No PDO input subscription.
  view.setUint16(2, 1_000, true);
  view.setBigUint64(4, lastSeenEpoch, true);
  return encodeIpcFrame(IpcType.HELLO, 0, payload);
}

export class FrameDecoder {
  #buffer = new Uint8Array();

  push(data: Uint8Array): IpcFrame[] {
    if (data.byteLength === 0) return [];
    if (this.#buffer.byteLength + data.byteLength > MAX_IPC_BUFFER_BYTES) {
      this.#buffer = new Uint8Array();
      throw new ProtocolError("IPC receive buffer exceeded its limit");
    }
    const joined = new Uint8Array(this.#buffer.byteLength + data.byteLength);
    joined.set(this.#buffer);
    joined.set(data, this.#buffer.byteLength);
    this.#buffer = joined;

    const frames: IpcFrame[] = [];
    while (this.#buffer.byteLength >= FRAME_HEADER_BYTES) {
      const length = new DataView(
        this.#buffer.buffer,
        this.#buffer.byteOffset,
        FRAME_HEADER_BYTES
      ).getUint32(0, true);
      if (length < MIN_IPC_FRAME_BYTES || length > MAX_IPC_FRAME_BYTES) {
        this.#buffer = new Uint8Array();
        throw new ProtocolError(`unexpected IPC frame length ${length}`);
      }
      const frameBytes = FRAME_HEADER_BYTES + length;
      if (this.#buffer.byteLength < frameBytes) break;
      const body = this.#buffer.slice(FRAME_HEADER_BYTES, frameBytes);
      if (!IPC_MAGIC.every((byte, index) => body[index] === byte)) {
        this.#buffer = new Uint8Array();
        throw new ProtocolError("invalid IPC frame magic");
      }
      if (body[4] !== IPC_VERSION) {
        this.#buffer = new Uint8Array();
        throw new ProtocolError(`unsupported IPC version ${body[4]}`);
      }
      const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
      const flags = view.getUint16(6, true);
      if (flags !== 0) {
        this.#buffer = new Uint8Array();
        throw new ProtocolError(`unsupported IPC flags ${flags}`);
      }
      frames.push({
        version: body[4]!,
        type: body[5]!,
        flags,
        requestId: view.getUint32(8, true),
        payload: body.slice(IPC_ENVELOPE_BYTES)
      });
      this.#buffer = this.#buffer.slice(frameBytes);
    }
    return frames;
  }

  reset(): void {
    this.#buffer = new Uint8Array();
  }

  get bufferedBytes(): number {
    return this.#buffer.byteLength;
  }
}

export function parseMainDeviceStatus(
  packet: Uint8Array,
  now = Date.now()
): MainDeviceStatus {
  if (packet.byteLength !== 56) {
    throw new ProtocolError("STATUS payload must be 56 bytes");
  }
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  const flags = view.getUint8(1);
  const schedulingMode = view.getUint8(46);
  const realtimeErrorBits = view.getUint8(47);
  const daemonEpoch = view.getBigUint64(32, true);
  if (view.getUint8(0) > 6) {
    throw new ProtocolError("STATUS aggregate state is invalid");
  }
  if (schedulingMode > 1 || (realtimeErrorBits & 0x80) !== 0) {
    throw new ProtocolError("STATUS scheduling metadata is invalid");
  }
  if (daemonEpoch === 0n) throw new ProtocolError("STATUS epoch must be non-zero");
  return {
    aggregateState: view.getUint8(0),
    operational: (flags & (1 << 0)) !== 0,
    outputsEnabled: (flags & (1 << 1)) !== 0,
    controllerConnected: (flags & (1 << 2)) !== 0,
    preemptRtAvailable: (flags & (1 << 3)) !== 0,
    realtimeApplied: (flags & (1 << 4)) !== 0,
    realtimeRequested: (flags & (1 << 5)) !== 0,
    timingDegraded: (flags & (1 << 6)) !== 0,
    reinitializing: (flags & (1 << 7)) !== 0,
    schedulingMode: schedulingMode === 1 ? "fifo" : "standard",
    realtimeErrorBits,
    distributedClockUnlocked: (realtimeErrorBits & (1 << 6)) !== 0,
    activeAdapters: view.getUint16(2, true),
    subDeviceCount: view.getUint16(4, true),
    activeFaults: view.getUint16(6, true),
    maxJitterUs: view.getUint32(8, true),
    currentJitterUs: view.getUint32(12, true),
    lostFrames: view.getBigUint64(16, true).toString(),
    cycleOverruns: view.getBigUint64(24, true).toString(),
    daemonEpoch: daemonEpoch.toString(),
    controllerPid: view.getUint32(40, true),
    configuredBuses: view.getUint16(44, true),
    monotonicUs: view.getBigUint64(48, true).toString(),
    updatedAt: now,
    stale: false,
    connected: true
  };
}

export function parseHelloAck(payload: Uint8Array): HelloAck {
  if (payload.byteLength !== 20) throw new ProtocolError("HELLO_ACK payload must be 20 bytes");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  if (view.getUint8(0) > 1 || view.getUint8(1) > 1) {
    throw new ProtocolError("HELLO_ACK boolean/role fields are invalid");
  }
  if (view.getUint16(2, true) === 0) {
    throw new ProtocolError("HELLO_ACK heartbeat timeout must be non-zero");
  }
  const epoch = view.getBigUint64(4, true);
  if (epoch === 0n) throw new ProtocolError("HELLO_ACK epoch must be non-zero");
  return {
    grantedRole: view.getUint8(0),
    outputsEnabled: view.getUint8(1) !== 0,
    heartbeatTimeoutMs: view.getUint16(2, true),
    epoch: epoch.toString(),
    peerPid: view.getUint32(12, true),
    capabilities: view.getUint32(16, true)
  };
}

export function parseIpcAck(payload: Uint8Array): IpcAck {
  if (payload.byteLength !== 8) throw new ProtocolError("ACK payload must be 8 bytes");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  if (view.getUint16(2, true) !== 0) throw new ProtocolError("ACK reserved field must be zero");
  const status = view.getUint16(0, true);
  if (status > 12) throw new ProtocolError("ACK status is invalid");
  return {
    status,
    detail: view.getUint32(4, true)
  };
}

const LOCK_STATES: BusInfo["lockState"][] = [
  "unlocked",
  "matched",
  "missing",
  "mismatch",
  "runtime_unlocked"
];

function decodeField(payload: Uint8Array, offset: number, length: number, field: string): string {
  if (offset + length > payload.byteLength) throw new ProtocolError(`BUS_INFO ${field} is truncated`);
  const value = new TextDecoder("utf-8", { fatal: true }).decode(payload.slice(offset, offset + length));
  if (containsControlCharacters(value)) {
    throw new ProtocolError(`BUS_INFO ${field} contains control characters`);
  }
  return value;
}

export function parseBusInfo(payload: Uint8Array): BusInfo {
  if (payload.byteLength < 30 || payload.byteLength > 2_048) {
    throw new ProtocolError("BUS_INFO payload length is invalid");
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const state = view.getUint8(2);
  const link = view.getUint8(3);
  const lockStateValue = view.getUint8(4);
  const lockReason = view.getUint8(5);
  const epoch = view.getBigUint64(22, true);
  if (state > 6 || link > 1 || lockStateValue > 4 || lockReason > 9 || epoch === 0n) {
    throw new ProtocolError("BUS_INFO enum or boolean field is invalid");
  }
  const lengths = [
    view.getUint16(8, true),
    view.getUint16(10, true),
    view.getUint16(12, true),
    view.getUint16(14, true),
    view.getUint16(16, true),
    view.getUint16(18, true),
    view.getUint16(20, true)
  ];
  const maxima = [32, 15, 1_024, 17, 256, 4, 4];
  for (let index = 0; index < lengths.length; index += 1) {
    if (lengths[index]! > maxima[index]!) {
      throw new ProtocolError(`BUS_INFO ${index} field is too long`);
    }
  }
  const expectedLength = 30 + lengths.reduce((sum, length) => sum + length, 0);
  if (payload.byteLength !== expectedLength) throw new ProtocolError("BUS_INFO field lengths do not match");
  const values: string[] = [];
  const names = [
    "logical_name",
    "physical_interface",
    "id_path",
    "permanent_mac",
    "usb_serial",
    "usb_vendor_id",
    "usb_product_id"
  ];
  let offset = 30;
  for (let index = 0; index < 7; index += 1) {
    values.push(decodeField(payload, offset, lengths[index]!, names[index]!));
    offset += lengths[index]!;
  }

  const idPath = values[2]!;
  const permanentMac = values[3]!.toLowerCase();
  let identity: AdapterLockIdentity | null = null;
  if (idPath) {
    identity = validateAdapterLock({
      id_path: idPath,
      permanent_mac: permanentMac,
      ...(values[4] ? { usb_serial: values[4] } : {}),
      ...(values[5] ? { usb_vendor_id: values[5] } : {}),
      ...(values[6] ? { usb_product_id: values[6] } : {})
    }, "BUS_INFO.identity");
  }
  return {
    busIndex: view.getUint16(0, true),
    state,
    linkUp: link !== 0,
    lockState: LOCK_STATES[lockStateValue] ?? "unknown",
    slaveCount: view.getUint16(6, true),
    logicalName: values[0]!,
    physicalInterface: values[1]!,
    identity,
    lockReason,
    epoch: epoch.toString()
  };
}

export function offlineStatus(options: {
  now?: number;
  stale?: boolean;
  epoch?: string;
} = {}): MainDeviceStatus {
  return {
    aggregateState: 0,
    operational: false,
    outputsEnabled: false,
    controllerConnected: false,
    preemptRtAvailable: false,
    realtimeApplied: false,
    realtimeRequested: false,
    timingDegraded: false,
    reinitializing: false,
    schedulingMode: "standard",
    realtimeErrorBits: 0,
    distributedClockUnlocked: false,
    activeAdapters: 0,
    subDeviceCount: 0,
    activeFaults: 0,
    maxJitterUs: 0,
    currentJitterUs: 0,
    lostFrames: "0",
    cycleOverruns: "0",
    daemonEpoch: options.epoch ?? "0",
    controllerPid: 0,
    configuredBuses: 0,
    monotonicUs: "0",
    updatedAt: options.now ?? Date.now(),
    stale: options.stale ?? true,
    connected: false
  };
}

export function makeClearCountersCommandFrame(requestId: number): Uint8Array {
  return encodeIpcFrame(IpcType.CLEAR_COUNTERS, requestId);
}

export function makeAdapterRescanFrame(requestId: number): Uint8Array {
  return encodeIpcFrame(IpcType.ADAPTER_RESCAN, requestId);
}

export function boundedBackoffDelay(
  attempt: number,
  options: { minimumMs?: number; maximumMs?: number; jitter?: number } = {}
): number {
  const rawMinimum = options.minimumMs ?? 250;
  const rawMaximum = options.maximumMs ?? 5_000;
  const minimum = Number.isFinite(rawMinimum) ? Math.max(1, Math.floor(rawMinimum)) : 250;
  const maximum = Number.isFinite(rawMaximum)
    ? Math.max(minimum, Math.floor(rawMaximum))
    : 5_000;
  const rawAttempt = Number.isFinite(attempt) ? Math.floor(attempt) : 0;
  const safeAttempt = Math.max(0, Math.min(rawAttempt, 16));
  const rawJitter = options.jitter ?? 0;
  const jitter = Number.isFinite(rawJitter) ? Math.max(-0.5, Math.min(rawJitter, 0.5)) : 0;
  const base = Math.min(maximum, minimum * (2 ** safeAttempt));
  return Math.max(minimum, Math.min(maximum, Math.round(base * (1 + jitter))));
}

export class FixedWindowRateLimiter {
  readonly #entries = new Map<string, { count: number; startedAt: number }>();

  constructor(
    private readonly limit: number,
    private readonly windowMs: number,
    private readonly maximumEntries = 2_048
  ) {}

  allow(key: string, now = Date.now()): boolean {
    const existing = this.#entries.get(key);
    if (!existing || now - existing.startedAt >= this.windowMs) {
      if (this.#entries.size >= this.maximumEntries) this.prune(now);
      this.#entries.set(key, { count: 1, startedAt: now });
      return true;
    }
    if (existing.count >= this.limit) return false;
    existing.count += 1;
    return true;
  }

  prune(now = Date.now()): void {
    for (const [key, entry] of this.#entries) {
      if (now - entry.startedAt >= this.windowMs) this.#entries.delete(key);
    }
    while (this.#entries.size >= this.maximumEntries) {
      const first = this.#entries.keys().next().value as string | undefined;
      if (first === undefined) break;
      this.#entries.delete(first);
    }
  }
}

export class NonOverlappingTask {
  #running = false;

  get running(): boolean {
    return this.#running;
  }

  async run(task: () => Promise<void>): Promise<boolean> {
    if (this.#running) return false;
    this.#running = true;
    try {
      await task();
      return true;
    } finally {
      this.#running = false;
    }
  }
}
