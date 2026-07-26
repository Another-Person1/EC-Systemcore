import { constants, type Dirent } from "node:fs";
import {
  lstat,
  mkdir,
  open,
  opendir,
  realpath,
  rename,
  unlink
} from "node:fs/promises";
import { randomBytes, timingSafeEqual } from "node:crypto";
import {
  basename,
  dirname,
  extname,
  isAbsolute,
  join,
  resolve
} from "node:path";
import {
  DEFAULT_CONFIG,
  FixedWindowRateLimiter,
  FrameDecoder,
  IpcType,
  NonOverlappingTask,
  ProtocolError,
  SYSTEM_VERSION,
  ValidationError,
  boundedBackoffDelay,
  deriveAdapterLockState,
  deriveCanonicalSysfsIdPath,
  hasRemovedLegacyConfigKeys,
  isPathWithin,
  makeAdapterRescanFrame,
  makeClearCountersCommandFrame,
  makeObserverHelloFrame,
  offlineStatus,
  parseBusInfo,
  parseHelloAck,
  parseIpcAck,
  parseMainDeviceStatus,
  normalizeMac,
  validateAdapterLock,
  validateConfig,
  type AdapterLockIdentity,
  type BusInfo,
  type ConfigurationConfig,
  type MainDeviceStatus
} from "./core";
import {
  MAX_ZIP_ENTRIES,
  MAX_ZIP_ENTRY_BYTES,
  MAX_ZIP_TOTAL_BYTES,
  createStoredZipStream,
  type OpenZipEntry
} from "./zip";

const DEFAULT_HOST = "0.0.0.0";
const DEFAULT_PORT = 8080;
const DEFAULT_CONFIG_PATH = "/etc/ec-systemcore/ec-systemcore.json";
const DEFAULT_SOCKET_PATH = "/run/ec-systemcore/ec-systemcore.sock";
const DEFAULT_LOG_ROOT = "/var/log/ec-systemcore";
const MAX_LOG_SELECTION_COUNT = 32;
const MAX_WEBSOCKET_CLIENTS = 16;
const STATUS_STALE_MS = 1_500;
const CSRF_NONCE_BYTES = 32;
const CSRF_NONCE_TTL_MS = 30 * 60 * 1000;
const MAX_CSRF_NONCES = 128;
const IPC_REQUEST_TIMEOUT_MS = 2_000;
const IPC_HELLO_TIMEOUT_MS = 1_000;
const IPC_MINIMUM_LIVENESS_TIMEOUT_MS = 3_000;
const MAX_IPC_OUTGOING_BYTES = 64 * 1024;
const MAX_CONFIG_FILE_BYTES = 1024 * 1024;
const MAX_SYSFS_TEXT_BYTES = 4096;
const MAX_ADAPTER_ENTRIES = 256;
const MAX_LOG_DIRECTORY_ENTRIES = 4_096;
const LIVE_LOG_CHUNK_BYTES = 64 * 1024;
const LIVE_LOG_LINE_BYTES = 8 * 1024;
const STATIC_FILES = new Map([
  ["/", "index.html"],
  ["/index.html", "index.html"],
  ["/app.js", "app.js"],
  ["/tailwind.css", "tailwind.css"]
]);

type ServerType = ReturnType<typeof Bun.serve>;
type ConfigurationWebSocket = Bun.ServerWebSocket<{ csrfNonce: string }>;
type IpcSocket = Bun.Socket<undefined>;

type ConfigurationSettings = {
  host: string;
  port: number;
  configPath: string;
  socketPath: string;
  logRoot: string;
  publicDir: string;
  sysfsRoot: string;
  udevDataRoot: string;
  allowedOrigins: Set<string>;
  allowedHosts: Set<string>;
  allowedClientCidrs: Ipv4Cidr[];
  remoteBind: boolean;
  disableIpc: boolean;
};

type ConfigState = {
  config: ConfigurationConfig;
  isConfigLoaded: boolean;
  regeneratedFailsafe: boolean;
  configError: string | null;
  invalidBackupCreated: boolean;
};

type AdapterInfo = {
  name: string;
  configuredPhysicalInterface: string;
  logicalName: string;
  currentMac: string;
  identity: AdapterLockIdentity | null;
  detected: boolean;
  configured: boolean;
  enabled: boolean;
  restricted: boolean;
  lockConfigured: boolean;
  lockState: BusInfo["lockState"];
  rxBytesPerSecond: number;
  txBytesPerSecond: number;
  busIndex: number | null;
  linkUp: boolean | null;
};

type LogInfo = {
  name: string;
  size: number;
  modifiedAt: number;
  format: "text" | "wpilog";
};

type SafeLog = LogInfo & {
  path: string;
};

type RequestContext = {
  clientIp: string;
  server?: ServerType;
};

type Ipv4Cidr = {
  network: number;
  mask: number;
};

type PendingIpcRequest = {
  resolve: (value: { status: number; detail: number }) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

const SECURITY_HEADERS: Record<string, string> = {
  "content-security-policy":
    "default-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; "
    + "form-action 'self'; script-src 'self'; style-src 'self'; connect-src 'self'",
  "cross-origin-opener-policy": "same-origin",
  "cross-origin-resource-policy": "same-origin",
  "permissions-policy": "camera=(), microphone=(), geolocation=(), usb=()",
  "referrer-policy": "no-referrer",
  "x-content-type-options": "nosniff",
  "x-frame-options": "DENY"
};

function envBoolean(value: string | undefined): boolean {
  return value === "1" || value?.toLowerCase() === "true";
}

function isLoopbackHost(host: string): boolean {
  const normalized = host.replace(/^\[|\]$/g, "").toLowerCase();
  return normalized === "127.0.0.1" || normalized === "::1" || normalized === "localhost";
}

function isWildcardHost(host: string): boolean {
  const normalized = host.replace(/^\[|\]$/g, "").toLowerCase();
  return normalized === "0.0.0.0" || normalized === "::";
}

function formatOriginHost(host: string): string {
  return host.includes(":") && !host.startsWith("[") ? `[${host}]` : host;
}

function splitList(value: string | undefined): string[] {
  return (value ?? "").split(",").map((item) => item.trim()).filter(Boolean);
}

function ipv4ToNumber(value: string): number | null {
  const pieces = value.split(".");
  if (pieces.length !== 4) return null;
  let result = 0;
  for (const piece of pieces) {
    if (!/^\d{1,3}$/.test(piece)) return null;
    const byte = Number(piece);
    if (byte < 0 || byte > 255) return null;
    result = ((result << 8) | byte) >>> 0;
  }
  return result >>> 0;
}

function parseIpv4Cidr(value: string): Ipv4Cidr {
  const [address, prefixText] = value.split("/");
  const numeric = ipv4ToNumber(address ?? "");
  const prefix = Number(prefixText);
  if (numeric === null || !Number.isInteger(prefix) || prefix < 1 || prefix > 32) {
    throw new Error(`Invalid EC_SYSTEMCORE_ALLOWED_CLIENT_SUBNETS entry: ${value}`);
  }
  const mask = (0xffff_ffff << (32 - prefix)) >>> 0;
  return { network: numeric & mask, mask };
}

function clientIpAllowed(ip: string, settings: ConfigurationSettings): boolean {
  if (!settings.remoteBind) {
    return (
      ip === "127.0.0.1"
      || ip === "::1"
      || ip === "::ffff:127.0.0.1"
      || ip === "unknown"
    );
  }
  if (settings.allowedClientCidrs.length === 0) return ip !== "unknown";
  const numeric = ipv4ToNumber(ip.replace(/^::ffff:/, ""));
  return numeric !== null
    && settings.allowedClientCidrs.some((cidr) => (numeric & cidr.mask) === cidr.network);
}

function canonicalHostHeader(value: string): string | null {
  if (
    value.length === 0
    || value.length > 255
    || [...value].some((character) => {
      const byte = character.charCodeAt(0);
      return byte <= 0x20 || byte === 0x7f;
    })
  ) return null;
  try {
    const parsed = new URL(`http://${value}`);
    if (
      parsed.username
      || parsed.password
      || parsed.pathname !== "/"
      || parsed.search
      || parsed.hash
    ) return null;
    return parsed.host.toLowerCase();
  } catch {
    return null;
  }
}

export function loadSettings(
  environment: Record<string, string | undefined> = process.env
): ConfigurationSettings {
  const host = environment.EC_SYSTEMCORE_HOST ?? DEFAULT_HOST;
  const requestedPort = Number(environment.EC_SYSTEMCORE_PORT ?? environment.PORT ?? DEFAULT_PORT);
  if (!Number.isInteger(requestedPort) || requestedPort < 0 || requestedPort > 65535) {
    throw new Error("EC_SYSTEMCORE_PORT must be an integer from 0 to 65535");
  }
  const remoteBind = !isLoopbackHost(host);
  const cidrValues = splitList(environment.EC_SYSTEMCORE_ALLOWED_CLIENT_SUBNETS);
  const originValues = splitList(environment.EC_SYSTEMCORE_ALLOWED_ORIGINS);
  const allowedOrigins = new Set(originValues);
  for (const origin of allowedOrigins) {
    const parsed = new URL(origin);
    if (parsed.origin !== origin || (parsed.protocol !== "http:" && parsed.protocol !== "https:")) {
      throw new Error(`Invalid allowed origin: ${origin}`);
    }
  }
  const allowedHosts = new Set(splitList(environment.EC_SYSTEMCORE_ALLOWED_HOSTS));
  if (!isWildcardHost(host)) {
    allowedHosts.add(`${formatOriginHost(host)}:${requestedPort}`.toLowerCase());
  }
  for (const origin of allowedOrigins) allowedHosts.add(new URL(origin).host);
  if (!remoteBind) {
    allowedHosts.add(`localhost:${requestedPort}`);
    allowedHosts.add(`127.0.0.1:${requestedPort}`);
  }
  return {
    host,
    port: requestedPort,
    configPath: environment.EC_SYSTEMCORE_CONFIG ?? DEFAULT_CONFIG_PATH,
    socketPath: environment.EC_SYSTEMCORE_SOCKET ?? DEFAULT_SOCKET_PATH,
    logRoot: environment.EC_SYSTEMCORE_LOG_ROOT ?? DEFAULT_LOG_ROOT,
    publicDir: environment.EC_SYSTEMCORE_PUBLIC_DIR ?? join(import.meta.dir, "public"),
    sysfsRoot: environment.EC_SYSTEMCORE_SYSFS_ROOT ?? "/sys",
    udevDataRoot: environment.EC_SYSTEMCORE_UDEV_DATA_ROOT ?? "/run/udev/data",
    allowedOrigins,
    allowedHosts,
    allowedClientCidrs: cidrValues.map(parseIpv4Cidr),
    remoteBind,
    disableIpc: envBoolean(environment.EC_SYSTEMCORE_DISABLE_IPC)
  };
}

function responseHeaders(extra: HeadersInit = {}): Headers {
  const headers = new Headers(SECURITY_HEADERS);
  new Headers(extra).forEach((value, key) => headers.set(key, value));
  return headers;
}

function json(data: unknown, init: ResponseInit = {}): Response {
  return new Response(JSON.stringify(data), {
    ...init,
    headers: responseHeaders({
      "cache-control": "no-store",
      "content-type": "application/json; charset=utf-8",
      ...(init.headers ?? {})
    })
  });
}

function text(value: string, status = 200): Response {
  return new Response(value, {
    status,
    headers: responseHeaders({
      "cache-control": "no-store",
      "content-type": "text/plain; charset=utf-8"
    })
  });
}

function apiError(status: number, code: string, message: string): Response {
  return json({ ok: false, code, message }, { status });
}

function safeErrorForLog(error: unknown): string {
  if (error instanceof ValidationError || error instanceof ProtocolError) return error.message;
  if (error instanceof Error) return error.name;
  return "UnknownError";
}

function parseStrictJson(text: string): unknown {
  let offset = 0;

  const skipWhitespace = (): void => {
    while (
      offset < text.length
      && (
        text[offset] === " "
        || text[offset] === "\t"
        || text[offset] === "\r"
        || text[offset] === "\n"
      )
    ) offset += 1;
  };

  const parseString = (): string => {
    const start = offset;
    if (text[offset] !== "\"") throw new SyntaxError("Expected JSON string");
    offset += 1;
    while (offset < text.length) {
      const character = text[offset]!;
      offset += 1;
      if (character === "\"") {
        return JSON.parse(text.slice(start, offset)) as string;
      }
      if (character === "\\") {
        if (offset >= text.length) throw new SyntaxError("Truncated JSON escape");
        if (text[offset] === "u") offset += 5;
        else offset += 1;
      } else if (character.charCodeAt(0) <= 0x1f) {
        throw new SyntaxError("Control character in JSON string");
      }
    }
    throw new SyntaxError("Unterminated JSON string");
  };

  const parseValue = (depth: number): void => {
    if (depth > 32) throw new SyntaxError("JSON nesting is too deep");
    skipWhitespace();
    const character = text[offset];
    if (character === "{") {
      offset += 1;
      skipWhitespace();
      const keys = new Set<string>();
      if (text[offset] === "}") {
        offset += 1;
        return;
      }
      while (true) {
        skipWhitespace();
        const key = parseString();
        if (keys.has(key)) throw new SyntaxError(`Duplicate JSON key: ${key}`);
        keys.add(key);
        skipWhitespace();
        if (text[offset] !== ":") throw new SyntaxError("Expected JSON colon");
        offset += 1;
        parseValue(depth + 1);
        skipWhitespace();
        if (text[offset] === "}") {
          offset += 1;
          return;
        }
        if (text[offset] !== ",") throw new SyntaxError("Expected JSON object separator");
        offset += 1;
      }
    }
    if (character === "[") {
      offset += 1;
      skipWhitespace();
      if (text[offset] === "]") {
        offset += 1;
        return;
      }
      while (true) {
        parseValue(depth + 1);
        skipWhitespace();
        if (text[offset] === "]") {
          offset += 1;
          return;
        }
        if (text[offset] !== ",") throw new SyntaxError("Expected JSON array separator");
        offset += 1;
      }
    }
    if (character === "\"") {
      parseString();
      return;
    }
    const start = offset;
    while (
      offset < text.length
      && text[offset] !== ","
      && text[offset] !== "]"
      && text[offset] !== "}"
      && text[offset] !== " "
      && text[offset] !== "\t"
      && text[offset] !== "\r"
      && text[offset] !== "\n"
    ) offset += 1;
    if (offset === start) throw new SyntaxError("Expected JSON value");
    JSON.parse(text.slice(start, offset));
  };

  parseValue(0);
  skipWhitespace();
  if (offset !== text.length) throw new SyntaxError("Trailing JSON data");
  return JSON.parse(text);
}

function constantTimeEqualHex(left: string, right: string): boolean {
  if (!/^[0-9a-f]{64}$/.test(left) || !/^[0-9a-f]{64}$/.test(right)) return false;
  const leftBytes = Buffer.from(left, "hex");
  const rightBytes = Buffer.from(right, "hex");
  return leftBytes.byteLength === rightBytes.byteLength && timingSafeEqual(leftBytes, rightBytes);
}

export async function readBoundedText(path: string, maximumBytes: number): Promise<string> {
  if (!Number.isSafeInteger(maximumBytes) || maximumBytes < 0) {
    throw new ValidationError("Invalid text-file size limit");
  }
  const flags = constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0);
  const handle = await open(path, flags);
  try {
    const info = await handle.stat();
    if (!info.isFile()) throw new ValidationError(`${path} is not a regular file`);
    const buffer = Buffer.allocUnsafe(maximumBytes + 1);
    let total = 0;
    while (total < buffer.byteLength) {
      const { bytesRead } = await handle.read(
        buffer,
        total,
        buffer.byteLength - total,
        null
      );
      if (bytesRead === 0) break;
      total += bytesRead;
    }
    if (total > maximumBytes) throw new ValidationError(`${path} exceeds its size limit`);
    try {
      return new TextDecoder("utf-8", { fatal: true }).decode(buffer.subarray(0, total));
    } catch {
      throw new ValidationError(`${path} is not valid UTF-8`);
    }
  } finally {
    await handle.close();
  }
}

async function readTextOr(path: string, fallback = ""): Promise<string> {
  try {
    return await readBoundedText(path, MAX_SYSFS_TEXT_BYTES);
  } catch {
    return fallback;
  }
}

export async function readDirectoryEntriesBounded(
  path: string,
  maximumEntries: number
): Promise<Dirent[]> {
  const entries: Dirent[] = [];
  const directory = await opendir(path);
  for await (const entry of directory) {
    if (entries.length >= maximumEntries) {
      throw new Error(`${path} has too many entries`);
    }
    entries.push(entry);
  }
  return entries;
}

async function readNumberOr(path: string, fallback = 0): Promise<number> {
  const value = Number((await readTextOr(path, String(fallback))).trim());
  return Number.isFinite(value) ? value : fallback;
}

function isLogName(name: string): boolean {
  const extension = extname(name).toLowerCase();
  if (
    name.length === 0
    || name.length > 255
    || basename(name) !== name
    || name.includes("\\")
  ) return false;
  for (let index = 0; index < name.length; index += 1) {
    const code = name.charCodeAt(index);
    if (code <= 0x1f || code === 0x7f) return false;
  }
  return extension === ".log" || extension === ".wpilog";
}

function contentType(path: string): string {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
  if (path.endsWith(".css")) return "text/css; charset=utf-8";
  return "application/octet-stream";
}

class DaemonClient {
  #socket: IpcSocket | null = null;
  #decoder = new FrameDecoder();
  #stopped = true;
  #connected = false;
  #helloComplete = false;
  #generation = 0;
  #disconnectHandledGeneration = -1;
  #reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  #helloTimer: ReturnType<typeof setTimeout> | null = null;
  #livenessTimer: ReturnType<typeof setTimeout> | null = null;
  #attempt = 0;
  #lastEpoch = 0n;
  #requestId = 1;
  #pending = new Map<number, PendingIpcRequest>();
  #outgoing: Uint8Array[] = [];
  #outgoingOffset = 0;
  #outgoingBytes = 0;

  constructor(
    private readonly socketPath: string,
    private readonly callbacks: {
      status: (status: MainDeviceStatus) => void;
      busInfo: (info: BusInfo) => void;
      epoch: (epoch: string) => void;
      disconnected: (epoch: string) => void;
      warning: (message: string) => void;
    }
  ) {}

  start(): void {
    if (!this.#stopped) return;
    this.#stopped = false;
    this.#scheduleConnect(0);
  }

  stop(): void {
    this.#stopped = true;
    this.#generation += 1;
    if (this.#reconnectTimer) clearTimeout(this.#reconnectTimer);
    this.#reconnectTimer = null;
    this.#clearIpcDeadlines();
    this.#socket?.end();
    this.#socket = null;
    this.#connected = false;
    this.#helloComplete = false;
    this.#clearOutgoing();
    this.#rejectPending(new Error("IPC client stopped"));
  }

  get connected(): boolean {
    return this.#connected && this.#helloComplete;
  }

  async clearCounters(): Promise<void> {
    const result = await this.#sendRequest((requestId) => makeClearCountersCommandFrame(requestId));
    if (result.status !== 0) throw new Error(`daemon rejected clear counters (${result.status})`);
  }

  async rescanAdapters(): Promise<void> {
    const result = await this.#sendRequest((requestId) => makeAdapterRescanFrame(requestId));
    if (result.status !== 0) throw new Error(`daemon rejected adapter rescan (${result.status})`);
  }

  #scheduleConnect(delay: number): void {
    if (this.#stopped || this.#reconnectTimer) return;
    this.#reconnectTimer = setTimeout(() => {
      this.#reconnectTimer = null;
      void this.#connect().catch((error) => {
        this.callbacks.warning(`IPC connect failed: ${safeErrorForLog(error)}`);
        this.#handleDisconnect(this.#generation);
      });
    }, delay);
  }

  async #connect(): Promise<void> {
    if (this.#stopped || this.#socket) return;
    const generation = ++this.#generation;
    this.#disconnectHandledGeneration = -1;
    this.#decoder.reset();
    this.#helloComplete = false;

    const socket = await Bun.connect({
      unix: this.socketPath,
      socket: {
        open: (opened) => {
          if (this.#stopped || generation !== this.#generation) {
            opened.end();
            return;
          }
          this.#socket = opened;
          this.#connected = true;
          const hello = makeObserverHelloFrame(this.#lastEpoch);
          this.#queueWrite(hello);
          this.#helloTimer = setTimeout(() => {
            if (generation !== this.#generation || this.#helloComplete) return;
            this.callbacks.warning("IPC HELLO_ACK timed out");
            opened.end();
            this.#handleDisconnect(generation);
          }, IPC_HELLO_TIMEOUT_MS);
        },
        data: (opened, data) => {
          if (generation !== this.#generation) return;
          try {
            for (const frame of this.#decoder.push(new Uint8Array(data))) this.#handleFrame(frame);
          } catch (error) {
            this.callbacks.warning(`IPC protocol error: ${safeErrorForLog(error)}`);
            opened.end();
          }
        },
        close: () => this.#handleDisconnect(generation),
        end: () => this.#handleDisconnect(generation),
        error: () => this.#handleDisconnect(generation),
        connectError: () => this.#handleDisconnect(generation),
        drain: () => {
          if (generation === this.#generation) this.#flushWrites();
        }
      }
    });
    if (
      !this.#stopped
      && generation === this.#generation
      && this.#connected
      && this.#disconnectHandledGeneration !== generation
      && !this.#socket
    ) {
      this.#socket = socket;
    }
  }

  #handleFrame(frame: ReturnType<FrameDecoder["push"]>[number]): void {
    if (!this.#helloComplete) {
      if (frame.type !== IpcType.HELLO_ACK || frame.requestId !== 0) {
        throw new ProtocolError("HELLO_ACK must be the daemon's first frame");
      }
      const hello = parseHelloAck(frame.payload);
      if (hello.grantedRole !== 0 || hello.outputsEnabled) {
        throw new ProtocolError("daemon granted unsafe controller authority to configuration");
      }
      const nextEpoch = BigInt(hello.epoch);
      if (nextEpoch !== this.#lastEpoch) this.callbacks.epoch(hello.epoch);
      this.#lastEpoch = nextEpoch;
      this.#helloComplete = true;
      if (this.#helloTimer) clearTimeout(this.#helloTimer);
      this.#helloTimer = null;
      this.#armLivenessDeadline(hello.heartbeatTimeoutMs);
      this.#attempt = 0;
      return;
    }

    this.#armLivenessDeadline();
    if (frame.type === IpcType.STATUS) {
      const status = parseMainDeviceStatus(frame.payload);
      const epoch = BigInt(status.daemonEpoch);
      if (epoch !== this.#lastEpoch) {
        this.#lastEpoch = epoch;
        this.callbacks.epoch(status.daemonEpoch);
      }
      this.callbacks.status(status);
      return;
    }
    if (frame.type === IpcType.BUS_INFO) {
      const info = parseBusInfo(frame.payload);
      if (BigInt(info.epoch) !== this.#lastEpoch) return;
      this.callbacks.busInfo(info);
      return;
    }
    if (frame.type === IpcType.ACK || frame.type === IpcType.ERROR) {
      const pending = this.#pending.get(frame.requestId);
      if (!pending) return;
      this.#pending.delete(frame.requestId);
      clearTimeout(pending.timer);
      const ack = parseIpcAck(frame.payload);
      pending.resolve(ack);
      return;
    }
    if (
      frame.type === IpcType.PDO_INPUT
      || frame.type === IpcType.OUTPUTS_DISABLED
    ) {
      return;
    }
    throw new ProtocolError(`unexpected IPC frame type ${frame.type}`);
  }

  #handleDisconnect(generation: number): void {
    if (generation !== this.#generation || this.#disconnectHandledGeneration === generation) return;
    this.#disconnectHandledGeneration = generation;
    this.#connected = false;
    this.#helloComplete = false;
    this.#socket = null;
    this.#decoder.reset();
    this.#clearIpcDeadlines();
    this.#clearOutgoing();
    this.#rejectPending(new Error("daemon IPC disconnected"));
    this.callbacks.disconnected(this.#lastEpoch.toString());
    if (!this.#stopped) {
      const delay = boundedBackoffDelay(this.#attempt, {
        jitter: ((Math.random() * 0.2) - 0.1)
      });
      this.#attempt += 1;
      this.#scheduleConnect(delay);
    }
  }

  #rejectPending(error: Error): void {
    for (const pending of this.#pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.#pending.clear();
  }

  #clearIpcDeadlines(): void {
    if (this.#helloTimer) clearTimeout(this.#helloTimer);
    if (this.#livenessTimer) clearTimeout(this.#livenessTimer);
    this.#helloTimer = null;
    this.#livenessTimer = null;
  }

  #armLivenessDeadline(heartbeatTimeoutMs = 0): void {
    if (this.#livenessTimer) clearTimeout(this.#livenessTimer);
    const generation = this.#generation;
    const timeout = Math.max(
      IPC_MINIMUM_LIVENESS_TIMEOUT_MS,
      heartbeatTimeoutMs * 4
    );
    this.#livenessTimer = setTimeout(() => {
      if (
        generation !== this.#generation
        || this.#stopped
        || !this.#helloComplete
      ) return;
      this.callbacks.warning("IPC daemon became unresponsive");
      this.#socket?.end();
      this.#handleDisconnect(generation);
    }, timeout);
  }

  #clearOutgoing(): void {
    this.#outgoing = [];
    this.#outgoingOffset = 0;
    this.#outgoingBytes = 0;
  }

  #queueWrite(frame: Uint8Array): void {
    if (!this.#socket || !this.#connected) throw new Error("daemon is offline");
    if (frame.byteLength > MAX_IPC_OUTGOING_BYTES - this.#outgoingBytes) {
      this.#socket.end();
      this.#handleDisconnect(this.#generation);
      throw new Error("daemon IPC output queue is full");
    }
    this.#outgoing.push(frame);
    this.#outgoingBytes += frame.byteLength;
    this.#flushWrites();
  }

  #flushWrites(): void {
    const socket = this.#socket;
    if (!socket) return;
    while (this.#outgoing.length > 0) {
      const frame = this.#outgoing[0]!;
      const remaining = frame.byteLength - this.#outgoingOffset;
      const written = socket.write(frame, this.#outgoingOffset, remaining);
      if (written < 0) {
        socket.end();
        this.#handleDisconnect(this.#generation);
        return;
      }
      if (written > remaining) {
        socket.end();
        this.#handleDisconnect(this.#generation);
        return;
      }
      if (written === 0) return;
      this.#outgoingOffset += written;
      this.#outgoingBytes -= written;
      if (this.#outgoingOffset < frame.byteLength) continue;
      this.#outgoing.shift();
      this.#outgoingOffset = 0;
    }
  }

  #sendRequest(build: (requestId: number) => Uint8Array): Promise<{ status: number; detail: number }> {
    if (!this.connected || !this.#socket) return Promise.reject(new Error("daemon is offline"));
    const requestId = this.#requestId;
    this.#requestId = this.#requestId >= 0xffff_fffe ? 1 : this.#requestId + 1;
    const frame = build(requestId);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(requestId);
        reject(new Error("daemon command timed out"));
      }, IPC_REQUEST_TIMEOUT_MS);
      this.#pending.set(requestId, { resolve, reject, timer });
      try {
        this.#queueWrite(frame);
      } catch (error) {
        clearTimeout(timer);
        this.#pending.delete(requestId);
        reject(error instanceof Error ? error : new Error("daemon command write failed"));
      }
    });
  }
}

export class ConfigurationRuntime {
  readonly settings: ConfigurationSettings;
  #configState: ConfigState = {
    config: DEFAULT_CONFIG,
    isConfigLoaded: false,
    regeneratedFailsafe: false,
    configError: null,
    invalidBackupCreated: false
  };
  #latestStatus = offlineStatus();
  #busInfo = new Map<number, BusInfo>();
  #daemonClient: DaemonClient;
  #csrfNonces = new Map<string, number>();
  #webSockets = new Set<ConfigurationWebSocket>();
  #webSocketBackpressure = new WeakMap<ConfigurationWebSocket, number>();
  #generalLimiter = new FixedWindowRateLimiter(120, 60_000);
  #mutationLimiter = new FixedWindowRateLimiter(30, 60_000);
  #downloadLimiter = new FixedWindowRateLimiter(5, 60_000);
  #bootstrapLimiter = new FixedWindowRateLimiter(20, 60_000);
  #adapterCounters = new Map<string, { rx: number; tx: number; at: number }>();
  #adapterRates = new Map<string, { rx: number; tx: number }>();
  #timers: ReturnType<typeof setInterval>[] = [];
  #configMutation: Promise<void> = Promise.resolve();
  #logPoll = new NonOverlappingTask();
  #adapterSample = new NonOverlappingTask();
  #activeTextLog: string | null = null;
  #activeTextOffset = 0;
  #activeTextTail = "";
  #lastBinaryNoticePath: string | null = null;
  #publicRoot = "";

  constructor(settings: ConfigurationSettings) {
    this.settings = settings;
    this.#daemonClient = new DaemonClient(settings.socketPath, {
      status: (status) => {
        this.#latestStatus = status;
        this.broadcastStatus();
      },
      busInfo: (info) => {
        this.#busInfo.set(info.busIndex, info);
        this.broadcastStatus();
      },
      epoch: (epoch) => {
        this.#busInfo.clear();
        this.#latestStatus = offlineStatus({ epoch, stale: true });
        this.broadcastStatus();
      },
      disconnected: (epoch) => {
        this.#busInfo.clear();
        this.#latestStatus = offlineStatus({ epoch, stale: true });
        this.broadcastStatus();
      },
      warning: (message) => console.warn(message)
    });
  }

  async initialize(startBackground = true): Promise<void> {
    this.#publicRoot = await realpath(this.settings.publicDir);
    await this.#loadConfig();
    await this.#sampleAdaptersSafely();
    if (startBackground) {
      if (!this.settings.disableIpc) this.#daemonClient.start();
      this.#timers.push(setInterval(() => void this.#pollActiveLogSafely(), 1_000));
      this.#timers.push(setInterval(() => void this.#sampleAdaptersSafely(), 1_000));
      this.#timers.push(setInterval(() => {
        this.#expireStatus();
        this.#pruneCsrfNonces();
      }, 500));
    }
  }

  stop(): void {
    for (const timer of this.#timers) clearInterval(timer);
    this.#timers = [];
    this.#daemonClient.stop();
    for (const ws of this.#webSockets) {
      try {
        ws.close(1001, "Configuration shutting down");
      } catch {
        // Socket is already closed.
      }
    }
    this.#webSockets.clear();
  }

  async handleRequest(req: Request, context: RequestContext): Promise<Response | undefined> {
    const url = new URL(req.url);
    if (!this.#requestBoundaryAllowed(req, context.clientIp, url.pathname.startsWith("/api/"))) {
      return apiError(403, "REQUEST_BOUNDARY_REJECTED", "Request rejected");
    }
    if (!this.#generalLimiter.allow(context.clientIp)) {
      return apiError(429, "RATE_LIMITED", "Too many requests");
    }
    if (url.pathname === "/ws/logs") return this.#upgradeWebSocket(req, context);
    if (url.pathname.startsWith("/api/")) return this.#handleApi(req, url, context);
    if (req.method !== "GET" && req.method !== "HEAD") {
      return text("Method not allowed", 405);
    }
    return this.#serveStatic(url.pathname);
  }

  webSocketHandler(): Bun.WebSocketHandler<{ csrfNonce: string }> {
    return {
      maxPayloadLength: 1_024,
      backpressureLimit: 64 * 1024,
      closeOnBackpressureLimit: true,
      idleTimeout: 30,
      open: (ws) => {
        this.#webSockets.add(ws);
        this.#sendWebSocket(ws, {
          type: "status",
          status: this.#effectiveStatus(),
          buses: [...this.#busInfo.values()]
        });
        this.#sendWebSocket(ws, { type: "config_loaded", configState: this.#configState });
      },
      close: (ws) => {
        this.#webSockets.delete(ws);
      },
      message: (ws) => {
        ws.close(1008, "Receive-only diagnostic stream");
      }
    };
  }

  broadcastStatus(): void {
    this.#broadcast({
      type: "status",
      status: this.#effectiveStatus(),
      buses: [...this.#busInfo.values()]
    });
  }

  #requestBoundaryAllowed(
    req: Request,
    clientIp: string,
    apiOrWebSocket: boolean
  ): boolean {
    if (!clientIpAllowed(clientIp, this.settings)) return false;
    const host = canonicalHostHeader(req.headers.get("host") ?? "");
    if (!host) return false;
    if (
      this.settings.allowedHosts.size > 0
      && !this.settings.allowedHosts.has(host)
    ) return false;
    const origin = req.headers.get("origin");
    if (origin && !this.#originMatchesHost(origin, host)) return false;
    const fetchSite = req.headers.get("sec-fetch-site");
    if (fetchSite && fetchSite !== "same-origin" && fetchSite !== "none") return false;
    if (apiOrWebSocket && fetchSite === "cross-site") return false;
    return true;
  }

  #originRequiredAndAllowed(req: Request): boolean {
    const origin = req.headers.get("origin");
    const host = canonicalHostHeader(req.headers.get("host") ?? "");
    return origin !== null && host !== null && this.#originMatchesHost(origin, host);
  }

  #originMatchesHost(origin: string, host: string): boolean {
    try {
      const parsed = new URL(origin);
      return parsed.origin === origin
        && (parsed.protocol === "http:" || parsed.protocol === "https:")
        && parsed.host.toLowerCase() === host
        && (
          this.settings.allowedOrigins.size === 0
          || this.settings.allowedOrigins.has(origin)
        );
    } catch {
      return false;
    }
  }

  #upgradeWebSocket(req: Request, context: RequestContext): Response | undefined {
    if (!context.server || req.method !== "GET" || !this.#originRequiredAndAllowed(req)) {
      return apiError(403, "WEBSOCKET_REJECTED", "WebSocket request rejected");
    }
    if (this.#webSockets.size >= MAX_WEBSOCKET_CLIENTS) {
      return apiError(503, "WEBSOCKET_LIMIT", "Too many diagnostic streams");
    }
    const protocols = (req.headers.get("sec-websocket-protocol") ?? "")
      .split(",")
      .map((value) => value.trim())
      .filter(Boolean);
    const nonceProtocol = protocols.find((value) => value.startsWith("csrf."));
    const nonce = nonceProtocol?.slice(5) ?? "";
    if (
      protocols.length !== 2
      || protocols[0] !== "ec-systemcore-v1"
      || !this.#csrfNonceValid(nonce)
    ) {
      return apiError(403, "WEBSOCKET_CSRF_REJECTED", "WebSocket request rejected");
    }
    if (context.server.upgrade(req, {
      data: { csrfNonce: nonce },
      headers: { "sec-websocket-protocol": "ec-systemcore-v1" }
    })) {
      return undefined;
    }
    return apiError(400, "WEBSOCKET_UPGRADE_FAILED", "WebSocket upgrade failed");
  }

  async #handleApi(req: Request, url: URL, context: RequestContext): Promise<Response> {
    try {
      if (url.pathname === "/api/bootstrap" && req.method === "POST") {
        if (
          !this.#bootstrapLimiter.allow(context.clientIp)
          || !this.#originRequiredAndAllowed(req)
          || req.headers.get("x-ec-systemcore-bootstrap") !== "1"
          || (req.headers.get("content-length") ?? "0") !== "0"
        ) {
          return apiError(403, "BOOTSTRAP_REJECTED", "Bootstrap request rejected");
        }
        const csrfToken = this.#issueCsrfNonce();
        return json({
          ok: true,
          csrfToken,
          version: SYSTEM_VERSION,
          capabilities: this.#capabilities()
        });
      }

      if (req.method !== "GET") {
        if (!this.#mutationLimiter.allow(context.clientIp)) {
          return apiError(429, "RATE_LIMITED", "Too many changes");
        }
        const readOnlyPost = url.pathname === "/api/logs/download";
        if (!(readOnlyPost ? this.#sameOriginJsonAllowed(req) : this.#mutationAllowed(req))) {
          return apiError(403, "MUTATION_REJECTED", "Configuration request rejected");
        }
      }

      if (url.pathname === "/api/status" && req.method === "GET") {
        const [adapters, logs, system] = await Promise.all([
          this.#listAdapters(),
          this.#listLogs(),
          this.#systemInfo()
        ]);
        return json({
          version: SYSTEM_VERSION,
          status: this.#effectiveStatus(),
          buses: [...this.#busInfo.values()],
          adapters,
          logCount: logs.length,
          system,
          configState: this.#configState,
          capabilities: this.#capabilities()
        });
      }
      if (url.pathname === "/api/config" && req.method === "GET") {
        return json({ configState: this.#configState, capabilities: this.#capabilities() });
      }
      if (url.pathname === "/api/config" && req.method === "POST") {
        const body = await this.#readJson(req);
        const next = validateConfig(body, { logRoot: this.settings.logRoot });
        await this.#serializeConfigMutation(async () => {
          await this.#persistConfig(next);
        });
        this.#broadcast({ type: "config_loaded", configState: this.#configState });
        return json({ ok: true, configState: this.#configState });
      }
      if (url.pathname === "/api/config/apply-defaults" && req.method === "POST") {
        await this.#readJson(req);
        const defaults = this.#defaultConfig();
        await this.#serializeConfigMutation(async () => this.#persistConfig(defaults));
        this.#broadcast({ type: "config_loaded", configState: this.#configState });
        return json({ ok: true, configState: this.#configState });
      }
      if (url.pathname === "/api/adapters/lock" && req.method === "POST") {
        const body = await this.#readJson(req);
        return await this.#lockAdapter(body);
      }
      if (url.pathname === "/api/adapters/unlock" && req.method === "POST") {
        const body = await this.#readJson(req);
        return await this.#unlockAdapter(body);
      }
      if (url.pathname === "/api/adapters/rescan" && req.method === "POST") {
        await this.#readJson(req);
        await this.#daemonClient.rescanAdapters();
        return json({ ok: true });
      }
      if (url.pathname === "/api/logs" && req.method === "GET") {
        const logs = await this.#listLogs();
        const newest = logs[0] ?? null;
        return json({
          logs,
          liveConsole: newest?.format === "text"
            ? { available: true, message: "Streaming the active text log." }
            : {
                available: false,
                message: newest
                  ? "A client-provided .wpilog is opaque and download-only; the daemon does not decode it."
                  : "No text log is active."
              }
        });
      }
      if (url.pathname === "/api/logs/delete" && req.method === "POST") {
        await this.#readJson(req);
        return apiError(
          410,
          "READ_ONLY_LOGS",
          "Configuration log deletion is disabled; log retention is daemon-owned"
        );
      }
      if (url.pathname === "/api/logs/delete-archived" && req.method === "POST") {
        await this.#readJson(req);
        return apiError(
          410,
          "READ_ONLY_LOGS",
          "Configuration log deletion is disabled; log retention is daemon-owned"
        );
      }
      if (url.pathname === "/api/logs/download" && req.method === "POST") {
        if (!this.#downloadLimiter.allow(context.clientIp)) {
          return apiError(429, "DOWNLOAD_RATE_LIMITED", "Too many downloads");
        }
        const body = await this.#readJson(req);
        const names = this.#validatedLogNames(body);
        const entries = await this.#openZipEntries(names);
        return new Response(createStoredZipStream(entries), {
          headers: responseHeaders({
            "cache-control": "no-store",
            "content-disposition": `attachment; filename="ec-systemcore-logs-${Date.now()}.zip"`,
            "content-type": "application/zip"
          })
        });
      }
      if (url.pathname === "/api/faults/clear" && req.method === "POST") {
        await this.#readJson(req);
        await this.#daemonClient.clearCounters();
        return json({ ok: true });
      }
      if (
        url.pathname.startsWith("/api/system/restart")
        || url.pathname.startsWith("/api/subdevices/station-id")
        || url.pathname.startsWith("/api/esi/")
      ) {
        return apiError(410, "UNSUPPORTED_OPERATION", "This unsafe operation is not available");
      }
      return apiError(404, "NOT_FOUND", "Not found");
    } catch (error) {
      if (error instanceof ValidationError) {
        return apiError(400, error.code, error.message);
      }
      console.error(`Configuration request failed: ${safeErrorForLog(error)}`);
      return apiError(500, "REQUEST_FAILED", "Request failed");
    }
  }

  #capabilities(): {
    readOnly: boolean;
    configWrite: boolean;
    logDelete: boolean;
    adapterLock: boolean;
    daemonDiagnostics: boolean;
    restart: false;
    rawActuation: false;
    esiUpload: false;
  } {
    return {
      readOnly: false,
      configWrite: true,
      logDelete: false,
      adapterLock: true,
      daemonDiagnostics: true,
      restart: false,
      rawActuation: false,
      esiUpload: false
    };
  }

  #mutationAllowed(req: Request): boolean {
    return this.#sameOriginJsonAllowed(req);
  }

  #sameOriginJsonAllowed(req: Request): boolean {
    if (!this.#originRequiredAndAllowed(req)) return false;
    if (req.headers.get("content-type") !== "application/json") return false;
    const nonce = req.headers.get("x-ec-systemcore-csrf") ?? "";
    return this.#csrfNonceValid(nonce);
  }

  async #readJson(req: Request): Promise<Record<string, unknown>> {
    const contentLengthHeader = req.headers.get("content-length");
    if (
      (
        contentLengthHeader !== null
        && (
          !/^\d+$/.test(contentLengthHeader)
          || Number(contentLengthHeader) > MAX_CONFIG_FILE_BYTES
        )
      )
      || req.headers.get("content-type") !== "application/json"
    ) {
      throw new ValidationError("Invalid JSON request body");
    }
    const chunks: Uint8Array[] = [];
    let bodyBytes = 0;
    if (req.body) {
      const reader = req.body.getReader();
      try {
        while (true) {
          const next = await reader.read();
          if (next.done) break;
          bodyBytes += next.value.byteLength;
          if (bodyBytes > MAX_CONFIG_FILE_BYTES) {
            await reader.cancel();
            throw new ValidationError("JSON request body is too large");
          }
          chunks.push(next.value);
        }
      } finally {
        reader.releaseLock();
      }
    }
    if (
      contentLengthHeader !== null
      && Number(contentLengthHeader) !== bodyBytes
    ) {
      throw new ValidationError("JSON request body length does not match Content-Length");
    }
    const body = new Uint8Array(bodyBytes);
    let offset = 0;
    for (const chunk of chunks) {
      body.set(chunk, offset);
      offset += chunk.byteLength;
    }
    let textBody: string;
    try {
      textBody = new TextDecoder("utf-8", { fatal: true }).decode(body);
    } catch {
      throw new ValidationError("JSON request body is not valid UTF-8");
    }
    let parsed: unknown;
    try {
      parsed = parseStrictJson(textBody || "{}");
    } catch {
      throw new ValidationError("Malformed JSON request body");
    }
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
      throw new ValidationError("JSON request body must be an object");
    }
    return parsed as Record<string, unknown>;
  }

  #issueCsrfNonce(): string {
    this.#pruneCsrfNonces();
    while (this.#csrfNonces.size >= MAX_CSRF_NONCES) {
      const oldest = this.#csrfNonces.keys().next().value as string | undefined;
      if (!oldest) break;
      this.#csrfNonces.delete(oldest);
    }
    const nonce = randomBytes(CSRF_NONCE_BYTES).toString("hex");
    this.#csrfNonces.set(nonce, Date.now() + CSRF_NONCE_TTL_MS);
    return nonce;
  }

  #csrfNonceValid(candidate: string): boolean {
    if (!/^[0-9a-f]{64}$/.test(candidate)) return false;
    const now = Date.now();
    for (const [nonce, expiresAt] of this.#csrfNonces) {
      if (expiresAt <= now) continue;
      if (constantTimeEqualHex(candidate, nonce)) {
        this.#csrfNonces.set(nonce, now + CSRF_NONCE_TTL_MS);
        return true;
      }
    }
    return false;
  }

  #pruneCsrfNonces(): void {
    const now = Date.now();
    for (const [nonce, expiresAt] of this.#csrfNonces) {
      if (expiresAt <= now) this.#csrfNonces.delete(nonce);
    }
  }

  #defaultConfig(): ConfigurationConfig {
    return validateConfig({
      ...DEFAULT_CONFIG,
      log_directory: this.settings.logRoot
    }, { logRoot: this.settings.logRoot });
  }

  async #loadConfig(): Promise<void> {
    await mkdir(dirname(this.settings.configPath), { recursive: true });
    try {
      const info = await lstat(this.settings.configPath);
      if (info.isSymbolicLink() || !info.isFile()) throw new ValidationError("config path is not a regular file");
      const parsed = JSON.parse(
        await readBoundedText(this.settings.configPath, MAX_CONFIG_FILE_BYTES)
      ) as unknown;
      const validated = validateConfig(parsed, { logRoot: this.settings.logRoot });
      if (hasRemovedLegacyConfigKeys(parsed)) {
        await this.#persistConfig(validated);
        this.#configState.configError =
          "Removed unsupported legacy NT4 settings; robot integration uses versioned IPC.";
        return;
      }
      this.#configState = {
        config: validated,
        isConfigLoaded: true,
        regeneratedFailsafe: false,
        configError: null,
        invalidBackupCreated: false
      };
      return;
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code;
      if (code === "ENOENT") {
        await this.#persistConfig(this.#defaultConfig(), true);
        return;
      }
      if (error instanceof SyntaxError || error instanceof ValidationError) {
        const backup = `${this.settings.configPath}.invalid-${Date.now()}`;
        try {
          await rename(this.settings.configPath, backup);
          await this.#persistConfig(this.#defaultConfig(), true);
          this.#configState.invalidBackupCreated = true;
          this.#configState.configError = "Invalid configuration was preserved as a backup.";
          return;
        } catch {
          this.#configState = {
            config: this.#defaultConfig(),
            isConfigLoaded: false,
            regeneratedFailsafe: true,
            configError: "Configuration is invalid and could not be safely replaced.",
            invalidBackupCreated: false
          };
          return;
        }
      }
      this.#configState = {
        config: this.#defaultConfig(),
        isConfigLoaded: false,
        regeneratedFailsafe: true,
        configError: "Configuration could not be read.",
        invalidBackupCreated: false
      };
    }
  }

  async #persistConfig(config: ConfigurationConfig, regeneratedFailsafe = false): Promise<void> {
    const validated = validateConfig(config, { logRoot: this.settings.logRoot });
    const directory = dirname(this.settings.configPath);
    await mkdir(directory, { recursive: true });
    try {
      const existing = await lstat(this.settings.configPath);
      if (existing.isSymbolicLink() || !existing.isFile()) {
        throw new Error("Refusing to replace a non-regular config path");
      }
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    }
    const tempPath = join(directory, `.ec-systemcore.json.${process.pid}.${randomBytes(8).toString("hex")}.tmp`);
    let handle: Awaited<ReturnType<typeof open>> | null = null;
    try {
      handle = await open(tempPath, "wx", 0o640);
      await handle.writeFile(`${JSON.stringify(validated, null, 2)}\n`, "utf8");
      await handle.sync();
      await handle.close();
      handle = null;
      await rename(tempPath, this.settings.configPath);
      if (process.platform !== "win32") {
        const directoryHandle = await open(directory, constants.O_RDONLY);
        try {
          await directoryHandle.sync();
        } finally {
          await directoryHandle.close();
        }
      }
      this.#configState = {
        config: validated,
        isConfigLoaded: !regeneratedFailsafe,
        regeneratedFailsafe,
        configError: regeneratedFailsafe ? "A failsafe configuration was generated." : null,
        invalidBackupCreated: false
      };
    } catch (error) {
      if (handle) await handle.close().catch(() => undefined);
      await unlink(tempPath).catch(() => undefined);
      throw error;
    }
  }

  async #serializeConfigMutation(operation: () => Promise<void>): Promise<void> {
    const task = this.#configMutation.then(operation, operation);
    this.#configMutation = task.then(() => undefined, () => undefined);
    await task;
  }

  async #lockAdapter(body: Record<string, unknown>): Promise<Response> {
    const logicalName = typeof body.logicalName === "string" ? body.logicalName : "";
    const physicalInterface = typeof body.physicalInterface === "string" ? body.physicalInterface : "";
    const confirmation = typeof body.confirmation === "string" ? body.confirmation : "";
    const adapters = await this.#listAdapters();
    const adapter = adapters.find((item) => item.detected && item.name === physicalInterface);
    if (!adapter?.identity) throw new ValidationError("Adapter does not expose a stable lock identity");
    const expectedConfirmation =
      `LOCK:${logicalName}:${adapter.identity.id_path}:${adapter.identity.permanent_mac ?? ""}`;
    if (confirmation !== expectedConfirmation) {
      throw new ValidationError("Adapter lock confirmation does not match the observed identity");
    }
    const mappings = this.#configState.config.interface_mappings.map((item) => ({ ...item }));
    const existing = mappings.find((item) => item.logical_name === logicalName);
    if (existing) {
      existing.physical_interface = physicalInterface;
      existing.lock = adapter.identity;
      existing.enabled = true;
    } else {
      mappings.push({
        logical_name: logicalName,
        physical_interface: physicalInterface,
        enabled: true,
        lock: adapter.identity
      });
    }
    const next = validateConfig({
      ...this.#configState.config,
      interface_mappings: mappings
    }, { logRoot: this.settings.logRoot });
    await this.#serializeConfigMutation(async () => this.#persistConfig(next));
    this.#broadcast({ type: "config_loaded", configState: this.#configState });
    return json({ ok: true, configState: this.#configState });
  }

  async #unlockAdapter(body: Record<string, unknown>): Promise<Response> {
    const logicalName = typeof body.logicalName === "string" ? body.logicalName : "";
    const confirmation = typeof body.confirmation === "string" ? body.confirmation : "";
    if (confirmation !== `UNLOCK:${logicalName}`) {
      throw new ValidationError("Adapter unlock confirmation is invalid");
    }
    const mappings = this.#configState.config.interface_mappings.map((item) => ({ ...item }));
    const mapping = mappings.find((item) => item.logical_name === logicalName);
    if (!mapping?.lock) throw new ValidationError("Adapter mapping is not locked");
    delete mapping.lock;
    const next = validateConfig({
      ...this.#configState.config,
      interface_mappings: mappings
    }, { logRoot: this.settings.logRoot });
    await this.#serializeConfigMutation(async () => this.#persistConfig(next));
    this.#broadcast({ type: "config_loaded", configState: this.#configState });
    return json({ ok: true, configState: this.#configState });
  }

  async #readableLogDirectory(): Promise<string> {
    const directory = resolve(this.#configState.config.log_directory);
    if (!isPathWithin(this.settings.logRoot, directory)) throw new Error("Unsafe log directory");
    const root = await realpath(this.settings.logRoot);
    const actual = await realpath(directory);
    if (!isPathWithin(root, actual)) throw new Error("Log directory escapes configured root");
    return actual;
  }

  async #listLogs(): Promise<LogInfo[]> {
    let directory: string;
    try {
      directory = await this.#readableLogDirectory();
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") return [];
      throw error;
    }
    const entries = await readDirectoryEntriesBounded(
      directory,
      MAX_LOG_DIRECTORY_ENTRIES
    );
    const logs: LogInfo[] = [];
    for (const entry of entries) {
      if (!entry.isFile() || entry.isSymbolicLink() || !isLogName(entry.name)) continue;
      const path = join(directory, entry.name);
      const info = await lstat(path);
      if (!info.isFile() || info.isSymbolicLink()) continue;
      const actual = await realpath(path);
      if (!isPathWithin(directory, actual)) continue;
      logs.push({
        name: entry.name,
        size: info.size,
        modifiedAt: info.mtimeMs,
        format: extname(entry.name).toLowerCase() === ".log" ? "text" : "wpilog"
      });
    }
    return logs.sort((left, right) => right.modifiedAt - left.modifiedAt);
  }

  async #safeLog(name: string): Promise<SafeLog | null> {
    if (!isLogName(name)) return null;
    let directory: string;
    try {
      directory = await this.#readableLogDirectory();
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") return null;
      throw error;
    }
    const path = resolve(directory, name);
    if (!isPathWithin(directory, path)) return null;
    try {
      const info = await lstat(path);
      if (!info.isFile() || info.isSymbolicLink()) return null;
      const actual = await realpath(path);
      if (!isPathWithin(directory, actual)) return null;
      return {
        name,
        path: actual,
        size: info.size,
        modifiedAt: info.mtimeMs,
        format: extname(name).toLowerCase() === ".log" ? "text" : "wpilog"
      };
    } catch {
      return null;
    }
  }

  #validatedLogNames(body: Record<string, unknown>): string[] {
    if (!Array.isArray(body.names)) throw new ValidationError("names must be an array");
    if (body.names.length === 0 || body.names.length > MAX_LOG_SELECTION_COUNT) {
      throw new ValidationError(`Select 1 to ${MAX_LOG_SELECTION_COUNT} log files`);
    }
    const names: string[] = [];
    const seen = new Set<string>();
    for (const value of body.names) {
      if (typeof value !== "string" || !isLogName(value) || seen.has(value)) {
        throw new ValidationError("Log filenames must be unique valid basenames");
      }
      seen.add(value);
      names.push(value);
    }
    return names;
  }

  async #openZipEntries(names: string[]): Promise<OpenZipEntry[]> {
    if (names.length > MAX_ZIP_ENTRIES) throw new ValidationError("Too many archive entries");
    const entries: OpenZipEntry[] = [];
    let total = 0;
    try {
      for (const name of names) {
        const safe = await this.#safeLog(name);
        if (!safe) throw new ValidationError(`Log file is unavailable: ${name}`);
        if (safe.size > MAX_ZIP_ENTRY_BYTES) {
          throw new ValidationError(`Log file exceeds the download limit: ${name}`);
        }
        total += safe.size;
        if (total > MAX_ZIP_TOTAL_BYTES) throw new ValidationError("Selected logs exceed the download limit");
        const flags = constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0);
        const handle = await open(safe.path, flags);
        const openedInfo = await handle.stat();
        if (!openedInfo.isFile() || openedInfo.size !== safe.size) {
          await handle.close();
          throw new ValidationError(`Log file changed before download: ${name}`);
        }
        entries.push({
          name,
          size: safe.size,
          modifiedAt: safe.modifiedAt,
          handle
        });
      }
      return entries;
    } catch (error) {
      await Promise.allSettled(entries.map((entry) => entry.handle.close()));
      throw error;
    }
  }

  async #pollActiveLogSafely(): Promise<void> {
    await this.#logPoll.run(async () => {
      try {
        await this.#pollActiveLog();
      } catch (error) {
        console.warn(`Live log poll skipped: ${safeErrorForLog(error)}`);
        this.#activeTextLog = null;
        this.#activeTextOffset = 0;
        this.#activeTextTail = "";
      }
    });
  }

  async #pollActiveLog(): Promise<void> {
    const logs = await this.#listLogs();
    const newest = logs[0] ?? null;
    if (!newest) return;
    if (newest.format === "wpilog") {
      if (this.#lastBinaryNoticePath !== newest.name) {
        this.#lastBinaryNoticePath = newest.name;
        this.#broadcast({
          type: "log_notice",
          message:
            "The newest file is an opaque client-provided .wpilog. Download it for analysis; text rendering is disabled."
        });
      }
      this.#activeTextLog = null;
      this.#activeTextOffset = 0;
      this.#activeTextTail = "";
      return;
    }
    const safe = await this.#safeLog(newest.name);
    if (!safe) return;
    if (safe.path !== this.#activeTextLog) {
      this.#activeTextLog = safe.path;
      this.#activeTextOffset = safe.size;
      this.#activeTextTail = "";
      return;
    }
    if (safe.size < this.#activeTextOffset) {
      this.#activeTextOffset = 0;
      this.#activeTextTail = "";
    }
    if (safe.size === this.#activeTextOffset) return;
    const end = Math.min(safe.size, this.#activeTextOffset + LIVE_LOG_CHUNK_BYTES);
    const file = Bun.file(safe.path);
    const chunk = await file.slice(this.#activeTextOffset, end).text();
    this.#activeTextOffset = end;
    const combined = this.#activeTextTail + chunk;
    const parts = combined.split(/\r?\n/);
    this.#activeTextTail = parts.pop() ?? "";
    if (new TextEncoder().encode(this.#activeTextTail).byteLength > LIVE_LOG_LINE_BYTES) {
      this.#activeTextTail = this.#activeTextTail.slice(-LIVE_LOG_LINE_BYTES);
    }
    for (const line of parts) {
      if (line) this.#broadcast({ type: "log", line: line.slice(0, LIVE_LOG_LINE_BYTES), at: Date.now() });
    }
  }

  async #discoverAdapterIdentity(name: string): Promise<{
    currentMac: string;
    identity: AdapterLockIdentity | null;
  }> {
    const sysfsRoot = this.settings.sysfsRoot;
    const base = join(sysfsRoot, "class", "net", name);
    const currentMac = (await readTextOr(`${base}/address`, "00:00:00:00:00:00")).trim().toLowerCase();
    let permanentMac = (await readTextOr(`${base}/perm_address`)).trim().toLowerCase();
    const assignmentType = (await readTextOr(`${base}/addr_assign_type`)).trim();
    if (!permanentMac && assignmentType === "0") permanentMac = currentMac;
    if (permanentMac) {
      try {
        permanentMac = normalizeMac(permanentMac, "adapter.permanent_mac");
      } catch {
        permanentMac = "";
      }
    }
    let idPath = "";
    let serial = "";
    let vendor = "";
    let product = "";
    try {
      let cursor = await realpath(`${base}/device`);
      if (!isPathWithin(sysfsRoot, cursor)) throw new Error("adapter device escapes sysfs");
      const ifindex = (await readTextOr(join(base, "ifindex"))).trim();
      const udevData = /^\d+$/.test(ifindex)
        ? await readTextOr(join(this.settings.udevDataRoot, `n${ifindex}`))
        : "";
      idPath = deriveCanonicalSysfsIdPath({
        sysfsRoot,
        devicePath: cursor,
        udevData,
        interfaceIdPath: await readTextOr(join(base, "id_path")),
        deviceIdPath: await readTextOr(join(cursor, "id_path")),
        deviceUevent: await readTextOr(join(cursor, "uevent"))
      });
      for (
        let depth = 0;
        depth < 16 && isAbsolute(cursor) && isPathWithin(sysfsRoot, cursor);
        depth += 1
      ) {
        vendor ||= (await readTextOr(join(cursor, "idVendor"))).trim().toLowerCase();
        product ||= (await readTextOr(join(cursor, "idProduct"))).trim().toLowerCase();
        serial ||= (await readTextOr(join(cursor, "serial"))).trim();
        const parent = dirname(cursor);
        if (parent === cursor) break;
        cursor = parent;
      }
    } catch {
      // A virtual/non-USB adapter has no stable sysfs device identity.
    }
    try {
      const identity = validateAdapterLock({
        id_path: idPath,
        permanent_mac: permanentMac,
        ...(serial ? { usb_serial: serial } : {}),
        ...(vendor ? { usb_vendor_id: vendor } : {}),
        ...(product ? { usb_product_id: product } : {})
      });
      return { currentMac, identity };
    } catch {
      return { currentMac, identity: null };
    }
  }

  async #listAdapters(): Promise<AdapterInfo[]> {
    let entries: Dirent[] = [];
    try {
      entries = await readDirectoryEntriesBounded(
        join(this.settings.sysfsRoot, "class", "net"),
        MAX_ADAPTER_ENTRIES
      );
    } catch {
      return this.#configState.config.interface_mappings.map((mapping) => ({
        name: mapping.physical_interface,
        configuredPhysicalInterface: mapping.physical_interface,
        logicalName: mapping.logical_name,
        currentMac: "",
        identity: null,
        detected: false,
        configured: true,
        enabled: mapping.enabled,
        restricted: false,
        lockConfigured: Boolean(mapping.lock),
        lockState: mapping.lock ? "missing" : "unlocked",
        rxBytesPerSecond: 0,
        txBytesPerSecond: 0,
        busIndex: null,
        linkUp: null
      }));
    }
    const discoveredCandidates = await Promise.all(entries.map(async (entry) => {
      const networkType = (
        await readTextOr(
          join(this.settings.sysfsRoot, "class", "net", entry.name, "type")
        )
      ).trim();
      if (entry.name === "lo" || networkType !== "1") return null;
      const identity = await this.#discoverAdapterIdentity(entry.name);
      return {
        name: entry.name,
        ...identity,
        restricted: ["eth0", "wlan0", "usb0", "lo"].includes(entry.name),
        rates: this.#adapterRates.get(entry.name) ?? { rx: 0, tx: 0 }
      };
    }));
    const discovered = discoveredCandidates.filter(
      (item): item is NonNullable<typeof item> => item !== null
    );
    const result: AdapterInfo[] = [];
    const consumed = new Set<string>();
    for (const mapping of this.#configState.config.interface_mappings) {
      let observed: typeof discovered[number] | undefined;
      if (mapping.lock) {
        const pathMatches = discovered.filter(
          (item) => item.identity?.id_path === mapping.lock!.id_path
        );
        observed = pathMatches.length === 1
          ? pathMatches[0]
          : (
            pathMatches.length === 0
              ? discovered.find((item) => item.name === mapping.physical_interface)
              : undefined
          );
      } else {
        observed = discovered.find((item) => item.name === mapping.physical_interface);
      }
      const bus = [...this.#busInfo.values()].find((item) => item.logicalName === mapping.logical_name);
      const lockState: BusInfo["lockState"] = mapping.lock
        ? (bus?.lockState
          ?? deriveAdapterLockState(
            mapping.lock,
            discovered.map((item) => ({ name: item.name, identity: item.identity })),
            mapping.physical_interface
          ))
        : "unlocked";
      if (observed) consumed.add(observed.name);
      result.push({
        name: observed?.name ?? mapping.physical_interface,
        configuredPhysicalInterface: mapping.physical_interface,
        logicalName: mapping.logical_name,
        currentMac: observed?.currentMac ?? "",
        identity: observed?.identity ?? bus?.identity ?? null,
        detected: Boolean(observed),
        configured: true,
        enabled: mapping.enabled,
        restricted: observed?.restricted ?? false,
        lockConfigured: Boolean(mapping.lock),
        lockState,
        rxBytesPerSecond: observed?.rates.rx ?? 0,
        txBytesPerSecond: observed?.rates.tx ?? 0,
        busIndex: bus?.busIndex ?? null,
        linkUp: bus?.linkUp ?? null
      });
    }
    for (const adapter of discovered) {
      if (consumed.has(adapter.name)) continue;
      if (adapter.restricted && !this.#configState.config.allow_restricted_interfaces) continue;
      result.push({
        name: adapter.name,
        configuredPhysicalInterface: adapter.name,
        logicalName: adapter.name,
        currentMac: adapter.currentMac,
        identity: adapter.identity,
        detected: true,
        configured: false,
        enabled: false,
        restricted: adapter.restricted,
        lockConfigured: false,
        lockState: "unlocked",
        rxBytesPerSecond: adapter.rates.rx,
        txBytesPerSecond: adapter.rates.tx,
        busIndex: null,
        linkUp: null
      });
    }
    return result;
  }

  async #sampleAdaptersSafely(): Promise<void> {
    await this.#adapterSample.run(async () => {
      try {
        await this.#sampleAdapterCounters();
      } catch (error) {
        console.warn(`Adapter sampling skipped: ${safeErrorForLog(error)}`);
      }
    });
  }

  async #sampleAdapterCounters(): Promise<void> {
    let names: string[];
    try {
      names = (
        await readDirectoryEntriesBounded(
          join(this.settings.sysfsRoot, "class", "net"),
          MAX_ADAPTER_ENTRIES
        )
      ).map((entry) => entry.name);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") return;
      throw error;
    }
    const now = Date.now();
    const present = new Set(names);
    await Promise.all(names.map(async (name) => {
      const [rx, tx] = await Promise.all([
        readNumberOr(join(this.settings.sysfsRoot, "class", "net", name, "statistics", "rx_bytes")),
        readNumberOr(join(this.settings.sysfsRoot, "class", "net", name, "statistics", "tx_bytes"))
      ]);
      const previous = this.#adapterCounters.get(name);
      if (previous) {
        const seconds = Math.max((now - previous.at) / 1000, 0.001);
        this.#adapterRates.set(name, {
          rx: Math.max(0, Math.round((rx - previous.rx) / seconds)),
          tx: Math.max(0, Math.round((tx - previous.tx) / seconds))
        });
      }
      this.#adapterCounters.set(name, { rx, tx, at: now });
    }));
    for (const name of this.#adapterCounters.keys()) {
      if (!present.has(name)) {
        this.#adapterCounters.delete(name);
        this.#adapterRates.delete(name);
      }
    }
  }

  async #systemInfo(): Promise<{
    model: string;
    memTotalMb: number;
    memAvailableMb: number;
    schedulingMode: "realtime-enhanced" | "standard-kernel";
    realtimeEnhancementsAvailable: boolean;
    realtimeEnhancementsApplied: boolean;
    realtimeEnhancementsRequested: boolean;
    timingDegraded: boolean;
    reinitializing: boolean;
    realtimeErrorBits: number;
    distributedClockUnlocked: boolean;
    cycleOverruns: string;
  }> {
    const [cpuInfo, memInfo] = await Promise.all([
      readTextOr("/proc/cpuinfo"),
      readTextOr("/proc/meminfo")
    ]);
    const model =
      cpuInfo.match(/^Model\s*:\s*(.+)$/m)?.[1]
      ?? cpuInfo.match(/^model name\s*:\s*(.+)$/m)?.[1]
      ?? cpuInfo.match(/^Hardware\s*:\s*(.+)$/m)?.[1]
      ?? "ARM64 Linux controller";
    const total = Number(memInfo.match(/^MemTotal:\s+(\d+)/m)?.[1] ?? 0);
    const available = Number(memInfo.match(/^MemAvailable:\s+(\d+)/m)?.[1] ?? 0);
    const status = this.#effectiveStatus();
    return {
      model,
      memTotalMb: Math.round(total / 1024),
      memAvailableMb: Math.round(available / 1024),
      schedulingMode: status.realtimeApplied ? "realtime-enhanced" : "standard-kernel",
      realtimeEnhancementsAvailable: status.preemptRtAvailable,
      realtimeEnhancementsApplied: status.realtimeApplied,
      realtimeEnhancementsRequested: status.realtimeRequested,
      timingDegraded: status.timingDegraded,
      reinitializing: status.reinitializing,
      realtimeErrorBits: status.realtimeErrorBits,
      distributedClockUnlocked: status.distributedClockUnlocked,
      cycleOverruns: status.cycleOverruns
    };
  }

  #effectiveStatus(now = Date.now()): MainDeviceStatus {
    if (
      !this.#latestStatus.connected
      || now - this.#latestStatus.updatedAt > STATUS_STALE_MS
    ) {
      return offlineStatus({
        now,
        stale: true,
        epoch: this.#latestStatus.daemonEpoch
      });
    }
    return this.#latestStatus;
  }

  #expireStatus(): void {
    if (
      this.#latestStatus.connected
      && Date.now() - this.#latestStatus.updatedAt > STATUS_STALE_MS
    ) {
      this.#latestStatus = offlineStatus({
        stale: true,
        epoch: this.#latestStatus.daemonEpoch
      });
      this.#busInfo.clear();
      this.broadcastStatus();
    }
  }

  async #serveStatic(pathname: string): Promise<Response> {
    const filename = STATIC_FILES.get(pathname);
    if (!filename) return text("Not found", 404);
    const candidate = resolve(this.#publicRoot, filename);
    if (!isPathWithin(this.#publicRoot, candidate)) return text("Forbidden", 403);
    try {
      const info = await lstat(candidate);
      if (!info.isFile() || info.isSymbolicLink()) return text("Not found", 404);
      const actual = await realpath(candidate);
      if (!isPathWithin(this.#publicRoot, actual)) return text("Forbidden", 403);
      return new Response(Bun.file(actual), {
        headers: responseHeaders({
          "cache-control": filename === "index.html"
            ? "no-store"
            : "public, max-age=3600, must-revalidate",
          "content-type": contentType(actual)
        })
      });
    } catch {
      return text("Not found", 404);
    }
  }

  #broadcast(payload: unknown): void {
    for (const ws of this.#webSockets) this.#sendWebSocket(ws, payload);
  }

  #sendWebSocket(ws: ConfigurationWebSocket, payload: unknown): void {
    try {
      const result = ws.send(JSON.stringify(payload));
      if (result === -1 || result === 0) {
        const count = (this.#webSocketBackpressure.get(ws) ?? 0) + 1;
        this.#webSocketBackpressure.set(ws, count);
        if (count >= 3) ws.close(1013, "Client is too slow");
      } else {
        this.#webSocketBackpressure.set(ws, 0);
      }
    } catch {
      this.#webSockets.delete(ws);
    }
  }
}

export async function startConfiguration(
  settings = loadSettings(),
  options: { startBackground?: boolean } = {}
): Promise<{ runtime: ConfigurationRuntime; server: ServerType }> {
  const runtime = new ConfigurationRuntime(settings);
  await runtime.initialize(options.startBackground ?? true);
  const server = Bun.serve({
    hostname: settings.host,
    port: settings.port,
    maxRequestBodySize: MAX_CONFIG_FILE_BYTES,
    idleTimeout: 10,
    async fetch(req, bunServer) {
      bunServer.timeout(req, 10);
      const clientIp = bunServer.requestIP(req)?.address ?? "unknown";
      try {
        return await runtime.handleRequest(req, { clientIp, server: bunServer });
      } catch (error) {
        console.error(`Configuration fetch failed: ${safeErrorForLog(error)}`);
        return apiError(500, "REQUEST_FAILED", "Request failed");
      }
    },
    websocket: runtime.webSocketHandler()
  });
  if (settings.port === 0) {
    const actualHost = `${formatOriginHost(settings.host)}:${server.port}`;
    settings.allowedHosts.add(actualHost);
    settings.allowedOrigins.add(`http://${actualHost}`);
  }
  return { runtime, server };
}

if (import.meta.main) {
  try {
    const { runtime, server } = await startConfiguration();
    const shutdown = () => {
      runtime.stop();
      void server.stop(true);
    };
    process.once("SIGINT", shutdown);
    process.once("SIGTERM", shutdown);
    console.log(
      `ec-systemcore configuration ${SYSTEM_VERSION} listening on ${server.hostname}:${server.port}`
    );
  } catch (error) {
    console.error(`ec-systemcore configuration failed to start: ${safeErrorForLog(error)}`);
    process.exitCode = 1;
  }
}
