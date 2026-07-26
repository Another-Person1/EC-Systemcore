import { afterEach, describe, expect, test } from "bun:test";
import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  realpath,
  rm,
  symlink,
  utimes,
  writeFile
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import {
  ConfigurationRuntime,
  loadSettings,
  readBoundedText,
  readDirectoryEntriesBounded,
  startConfiguration
} from "../server";
import { isPathWithin } from "../core";

type StartedFixture = Awaited<ReturnType<typeof startConfiguration>> & {
  root: string;
  baseUrl: string;
  origin: string;
  host: string;
};

const fixtures: StartedFixture[] = [];
const looseRoots: string[] = [];

function publicDirectory(): string {
  return resolve(import.meta.dir, "..", "public");
}

async function createFixture(options: {
  configText?: string | ((root: string) => string);
  prepareEnvironment?: (root: string) => Promise<Record<string, string>>;
} = {}): Promise<StartedFixture> {
  const root = await realpath(await mkdtemp(join(tmpdir(), "ec-systemcore-configuration-")));
  const configDirectory = join(root, "etc");
  const logRoot = join(root, "logs");
  await Promise.all([
    mkdir(configDirectory, { recursive: true }),
    mkdir(logRoot, { recursive: true })
  ]);
  const configPath = join(configDirectory, "ec-systemcore.json");
  if (options.configText !== undefined) {
    const configText = typeof options.configText === "function"
      ? options.configText(root)
      : options.configText;
    await writeFile(configPath, configText);
  }
  const preparedEnvironment = await options.prepareEnvironment?.(root) ?? {};
  const settings = loadSettings({
    EC_SYSTEMCORE_HOST: "127.0.0.1",
    EC_SYSTEMCORE_PORT: "0",
    EC_SYSTEMCORE_CONFIG: configPath,
    EC_SYSTEMCORE_SOCKET: join(root, "run", "ec-systemcore.sock"),
    EC_SYSTEMCORE_LOG_ROOT: logRoot,
    EC_SYSTEMCORE_PUBLIC_DIR: publicDirectory(),
    EC_SYSTEMCORE_DISABLE_IPC: "true",
    ...preparedEnvironment
  });
  const started = await startConfiguration(settings, { startBackground: false });
  const host = `127.0.0.1:${started.server.port}`;
  const fixture: StartedFixture = {
    ...started,
    root,
    baseUrl: `http://${host}`,
    origin: `http://${host}`,
    host
  };
  fixtures.push(fixture);
  return fixture;
}

async function bootstrap(fixture: StartedFixture): Promise<string> {
  const response = await fetch(`${fixture.baseUrl}/api/bootstrap`, {
    method: "POST",
    headers: {
      "content-length": "0",
      "origin": fixture.origin,
      "sec-fetch-site": "same-origin",
      "x-ec-systemcore-bootstrap": "1"
    }
  });
  expect(response.status).toBe(200);
  const body = await response.json() as { csrfToken: string };
  expect(body.csrfToken).toMatch(/^[0-9a-f]{64}$/);
  return body.csrfToken;
}

function mutationHeaders(fixture: StartedFixture, csrfToken: string): HeadersInit {
  return {
    "content-type": "application/json",
    "origin": fixture.origin,
    "sec-fetch-site": "same-origin",
    "x-ec-systemcore-csrf": csrfToken
  };
}

async function postJson(
  fixture: StartedFixture,
  path: string,
  csrfToken: string,
  body: unknown
): Promise<Response> {
  return fetch(`${fixture.baseUrl}${path}`, {
    method: "POST",
    headers: mutationHeaders(fixture, csrfToken),
    body: JSON.stringify(body)
  });
}

afterEach(async () => {
  await Promise.allSettled(fixtures.splice(0).map(async (fixture) => {
    fixture.runtime.stop();
    await fixture.server.stop(true);
    await rm(fixture.root, { recursive: true, force: true });
  }));
  await Promise.allSettled(
    looseRoots.splice(0).map((root) => rm(root, { recursive: true, force: true }))
  );
});

describe("configuration HTTP security boundary", () => {
  test("serves only the public shell with strict headers and reports honest stale state", async () => {
    const fixture = await createFixture();
    const shell = await fetch(`${fixture.baseUrl}/`);
    expect(shell.status).toBe(200);
    expect(shell.headers.get("content-security-policy")).toContain("frame-ancestors 'none'");
    expect(shell.headers.get("x-content-type-options")).toBe("nosniff");
    expect(await shell.text()).toContain("ec-systemcore");

    const statusResponse = await fetch(`${fixture.baseUrl}/api/status`);
    expect(statusResponse.status).toBe(200);
    const statusBody = await statusResponse.json() as {
      status: { connected: boolean; stale: boolean; activeAdapters: number };
      buses: unknown[];
      capabilities: { restart: boolean; rawActuation: boolean };
    };
    expect(statusBody.status).toMatchObject({
      connected: false,
      stale: true,
      activeAdapters: 0
    });
    expect(statusBody.buses).toEqual([]);
    expect(statusBody.capabilities).toMatchObject({
      restart: false,
      rawActuation: false
    });

    expect((await fetch(`${fixture.baseUrl}/not-whitelisted`)).status).toBe(404);
  });

  test("requires the exact Host, Origin, client boundary, content type, and CSRF nonce", async () => {
    const fixture = await createFixture();
    const csrfToken = await bootstrap(fixture);

    const wrongHost = await fixture.runtime.handleRequest(
      new Request(`${fixture.baseUrl}/api/status`, {
        headers: {
          host: "attacker.invalid",
          origin: fixture.origin,
          "sec-fetch-site": "same-origin"
        }
      }),
      { clientIp: "127.0.0.1" }
    );
    expect(wrongHost?.status).toBe(403);

    const wrongOrigin = await fixture.runtime.handleRequest(
      new Request(`${fixture.baseUrl}/api/status`, {
        headers: {
          host: fixture.host,
          origin: "http://attacker.invalid",
          "sec-fetch-site": "same-origin"
        }
      }),
      { clientIp: "127.0.0.1" }
    );
    expect(wrongOrigin?.status).toBe(403);

    const wrongClient = await fixture.runtime.handleRequest(
      new Request(`${fixture.baseUrl}/api/status`, {
        headers: { host: fixture.host }
      }),
      { clientIp: "192.168.1.50" }
    );
    expect(wrongClient?.status).toBe(403);

    expect((await fetch(`${fixture.baseUrl}/api/bootstrap`, {
      method: "POST",
      headers: {
        "content-length": "0",
        origin: fixture.origin,
        "sec-fetch-site": "same-origin"
      }
    })).status).toBe(403);

    expect((await fetch(`${fixture.baseUrl}/api/config`, {
      method: "POST",
      headers: {
        ...mutationHeaders(fixture, "0".repeat(64))
      },
      body: "{}"
    })).status).toBe(403);

    expect((await fetch(`${fixture.baseUrl}/api/config`, {
      method: "POST",
      headers: {
        ...mutationHeaders(fixture, csrfToken),
        "content-type": "application/json; charset=utf-8"
      },
      body: "{}"
    })).status).toBe(403);
  });

  test("serves writable zero-auth configuration on the LAN with optional subnet narrowing", async () => {
    const defaults = loadSettings({});
    expect(defaults.host).toBe("0.0.0.0");
    expect(defaults.remoteBind).toBe(true);
    expect(defaults.allowedClientCidrs).toEqual([]);

    const root = await mkdtemp(join(tmpdir(), "ec-systemcore-configuration-remote-"));
    looseRoots.push(root);
    const settings = loadSettings({
      EC_SYSTEMCORE_HOST: "192.168.10.2",
      EC_SYSTEMCORE_PORT: "8080",
      EC_SYSTEMCORE_ALLOWED_CLIENT_SUBNETS: "192.168.10.0/24",
      EC_SYSTEMCORE_CONFIG: join(root, "etc", "ec-systemcore.json"),
      EC_SYSTEMCORE_LOG_ROOT: join(root, "logs"),
      EC_SYSTEMCORE_PUBLIC_DIR: publicDirectory(),
      EC_SYSTEMCORE_DISABLE_IPC: "true"
    });
    const runtime = new ConfigurationRuntime(settings);
    await runtime.initialize(false);
    const headers = {
      host: "192.168.10.2:8080",
      origin: "http://192.168.10.2:8080",
      "sec-fetch-site": "same-origin",
      "x-ec-systemcore-bootstrap": "1",
      "content-length": "0"
    };
    const bootstrapResponse = await runtime.handleRequest(
      new Request("http://192.168.10.2:8080/api/bootstrap", {
        method: "POST",
        headers
      }),
      { clientIp: "192.168.10.50" }
    );
    const bootstrapBody = await bootstrapResponse?.json() as {
      csrfToken: string;
      capabilities: { readOnly: boolean };
    };
    expect(bootstrapBody.capabilities.readOnly).toBe(false);
    const mutation = await runtime.handleRequest(
      new Request("http://192.168.10.2:8080/api/config", {
        method: "POST",
        headers: {
          host: "192.168.10.2:8080",
          origin: "http://192.168.10.2:8080",
          "sec-fetch-site": "same-origin",
          "content-type": "application/json",
          "x-ec-systemcore-csrf": bootstrapBody.csrfToken
        },
        body: JSON.stringify({ log_directory: join(root, "logs") })
      }),
      { clientIp: "192.168.10.50" }
    );
    expect(mutation?.status).toBe(200);
    const outsideSubnet = await runtime.handleRequest(
      new Request("http://192.168.10.2:8080/api/status", {
        headers: { host: "192.168.10.2:8080" }
      }),
      { clientIp: "192.168.11.50" }
    );
    expect(outsideSubnet?.status).toBe(403);
    runtime.stop();
  });

  test("accepts canonical same-origin robot hostnames when no Host allowlist is configured", async () => {
    const root = await mkdtemp(join(tmpdir(), "ec-systemcore-configuration-hostname-"));
    looseRoots.push(root);
    const runtime = new ConfigurationRuntime(loadSettings({
      EC_SYSTEMCORE_CONFIG: join(root, "etc", "ec-systemcore.json"),
      EC_SYSTEMCORE_LOG_ROOT: join(root, "logs"),
      EC_SYSTEMCORE_PUBLIC_DIR: publicDirectory(),
      EC_SYSTEMCORE_DISABLE_IPC: "true"
    }));
    await runtime.initialize(false);
    const response = await runtime.handleRequest(
      new Request("http://ec-systemcore.local:8080/api/bootstrap", {
        method: "POST",
        headers: {
          host: "ec-systemcore.local:8080",
          origin: "http://ec-systemcore.local:8080",
          "sec-fetch-site": "same-origin",
          "x-ec-systemcore-bootstrap": "1",
          "content-length": "0"
        }
      }),
      { clientIp: "10.20.27.5" }
    );
    expect(response?.status).toBe(200);
    runtime.stop();
  });
});

describe("configuration persistence and unsafe route removal", () => {
  test("atomically writes exact daemon configuration and leaves disk unchanged on errors", async () => {
    const fixture = await createFixture();
    const csrfToken = await bootstrap(fixture);
    const configResponse = await fetch(`${fixture.baseUrl}/api/config`);
    const initial = await configResponse.json() as {
      configState: { config: Record<string, unknown> };
    };
    const next = {
      ...initial.configState.config,
      heartbeat_timeout_ms: 350,
      realtime_memory_lock: true,
      realtime_fifo: true,
      realtime_priority: 55,
      realtime_cpu: 2,
      cycle_period_us: 5_000,
      interface_mappings: [{
        logical_name: "Drive",
        physical_interface: "eth9",
        enabled: false,
        maximum_io_map_bytes: 65_536,
        distributed_clock: true,
        distributed_clock_shift_ns: -100,
        allow_unverified_topology: true,
        expected_subdevices: [{
          vendor_id: 0x11223344,
          product_code: 0x55667788,
          revision: 2,
          output_bytes: 16,
          input_bytes: 32
        }]
      }]
    };
    const writeResponse = await postJson(fixture, "/api/config", csrfToken, next);
    expect(writeResponse.status).toBe(200);
    const configPath = join(fixture.root, "etc", "ec-systemcore.json");
    const persistedText = await readFile(configPath, "utf8");
    const persisted = JSON.parse(persistedText) as typeof next;
    expect(persisted.heartbeat_timeout_ms).toBe(350);
    expect(persisted.interface_mappings[0]?.distributed_clock).toBe(true);
    expect(persisted.interface_mappings[0]?.allow_unverified_topology).toBe(true);
    expect(persisted.interface_mappings[0]?.expected_subdevices?.[0]?.product_code)
      .toBe(0x55667788);

    const invalidResponse = await postJson(fixture, "/api/config", csrfToken, {
      ...next,
      future_root: true
    });
    expect(invalidResponse.status).toBe(400);
    expect(await readFile(configPath, "utf8")).toBe(persistedText);
  });

  test("accepts a valid full-scale topology larger than 64 KiB", async () => {
    const fixture = await createFixture();
    const csrfToken = await bootstrap(fixture);
    const expectedSubdevices = Array.from({ length: 199 }, (_, index) => ({
      vendor_id: 0xffff_0000 + index,
      product_code: 0xff00_0000 + index,
      revision: index,
      output_bytes: 1_024,
      input_bytes: 0xffff_0000 + index
    }));
    const config = {
      allow_restricted_interfaces: false,
      cycle_period_us: 5_000,
      heartbeat_timeout_ms: 250,
      output_command_timeout_ms: 100,
      log_directory: join(fixture.root, "logs"),
      log_count_limit: 10,
      free_space_threshold_mb: 50,
      realtime_memory_lock: true,
      realtime_fifo: true,
      realtime_priority: 55,
      controller_group: "ec-systemcore-controller",
      controller_uids: [0],
      controller_gids: [],
      interface_mappings: Array.from({ length: 8 }, (_, index) => ({
        logical_name: `EC_${index}`,
        physical_interface: `eth${index + 1}`,
        enabled: false,
        maximum_io_map_bytes: 1_048_576,
        distributed_clock: true,
        distributed_clock_shift_ns: 0,
        allow_unverified_topology: false,
        expected_subdevices: expectedSubdevices
      }))
    };
    expect(JSON.stringify(config).length).toBeGreaterThan(64 * 1_024);

    const response = await postJson(
      fixture,
      "/api/config",
      csrfToken,
      config
    );
    expect(response.status).toBe(200);
    const persisted = JSON.parse(
      await readFile(join(fixture.root, "etc", "ec-systemcore.json"), "utf8")
    ) as { interface_mappings: Array<{ expected_subdevices: unknown[] }> };
    expect(persisted.interface_mappings).toHaveLength(8);
    expect(persisted.interface_mappings[7]?.expected_subdevices).toHaveLength(199);
  });

  test("backs up malformed configuration and regenerates explicit failsafe defaults", async () => {
    const fixture = await createFixture({ configText: "{ malformed" });
    const files = await readdir(join(fixture.root, "etc"));
    expect(files).toContain("ec-systemcore.json");
    expect(files.some((name) => name.startsWith("ec-systemcore.json.invalid-"))).toBe(true);
    const response = await fetch(`${fixture.baseUrl}/api/config`);
    const body = await response.json() as {
      configState: {
        isConfigLoaded: boolean;
        regeneratedFailsafe: boolean;
        invalidBackupCreated: boolean;
      };
    };
    expect(body.configState).toMatchObject({
      isConfigLoaded: false,
      regeneratedFailsafe: true,
      invalidBackupCreated: true
    });
  });

  test("rejects malformed UTF-8 and duplicate JSON keys like the daemon", async () => {
    const invalidUtf8 = await createFixture({
      prepareEnvironment: async (root) => {
        await writeFile(
          join(root, "etc", "ec-systemcore.json"),
          new Uint8Array([0x7b, 0x22, 0x78, 0x22, 0x3a, 0xff, 0x7d])
        );
        return {};
      }
    });
    const invalidUtf8State = await (
      await fetch(`${invalidUtf8.baseUrl}/api/config`)
    ).json() as {
      configState: { regeneratedFailsafe: boolean; invalidBackupCreated: boolean };
    };
    expect(invalidUtf8State.configState).toMatchObject({
      regeneratedFailsafe: true,
      invalidBackupCreated: true
    });

    const duplicate = await createFixture({
      configText:
        '{"interface_mappings":[],"interface_mappings":[{"logical_name":"Drive",'
        + '"physical_interface":"eth9","enabled":true}]}'
    });
    const duplicateState = await (
      await fetch(`${duplicate.baseUrl}/api/config`)
    ).json() as {
      configState: { regeneratedFailsafe: boolean; invalidBackupCreated: boolean };
    };
    expect(duplicateState.configState).toMatchObject({
      regeneratedFailsafe: true,
      invalidBackupCreated: true
    });
  });

  test("atomically migrates only rejected legacy NT4 keys out of an otherwise valid config", async () => {
    const fixture = await createFixture({
      configText: (root) => JSON.stringify({
        allow_restricted_interfaces: false,
        cycle_period_us: 5_000,
        nt4_server: "10.0.0.2",
        nt4_team: 1234,
        log_directory: join(root, "logs"),
        log_count_limit: 10,
        free_space_threshold_mb: 50,
        controller_group: "ec-systemcore-controller",
        controller_uids: [0],
        controller_gids: [],
        interface_mappings: [{
          logical_name: "EC_Trunk",
          physical_interface: "eth1",
          enabled: true
        }]
      })
    });
    const persisted = JSON.parse(
      await readFile(join(fixture.root, "etc", "ec-systemcore.json"), "utf8")
    ) as Record<string, unknown>;
    expect(Object.hasOwn(persisted, "nt4_server")).toBe(false);
    expect(Object.hasOwn(persisted, "nt4_team")).toBe(false);
    const response = await fetch(`${fixture.baseUrl}/api/config`);
    const body = await response.json() as {
      configState: { isConfigLoaded: boolean; configError: string | null };
    };
    expect(body.configState.isConfigLoaded).toBe(true);
    expect(body.configState.configError).toContain("legacy NT4");
  });

  test("returns permanent removal for restart, raw station-ID, and ESI operations", async () => {
    const fixture = await createFixture();
    const csrfToken = await bootstrap(fixture);
    for (const path of [
      "/api/system/restart",
      "/api/subdevices/station-id",
      "/api/esi/upload"
    ]) {
      const response = await postJson(fixture, path, csrfToken, {});
      expect(response.status).toBe(410);
      expect((await response.json() as { code: string }).code).toBe("UNSUPPORTED_OPERATION");
    }
  });

  test("adapter lock and unlock operations fail closed without stable observed identity", async () => {
    const fixture = await createFixture();
    const csrfToken = await bootstrap(fixture);
    const lock = await postJson(fixture, "/api/adapters/lock", csrfToken, {
      logicalName: "EC_Trunk",
      physicalInterface: "missing0",
      confirmation: "LOCK:EC_Trunk:path:mac"
    });
    expect(lock.status).toBe(400);
    const unlock = await postJson(fixture, "/api/adapters/unlock", csrfToken, {
      logicalName: "EC_Trunk",
      confirmation: "UNLOCK:EC_Trunk"
    });
    expect(unlock.status).toBe(400);
  });

  test("locks and unlocks the same udev-first stable identity emitted by the daemon", async () => {
    const fixture = await createFixture({
      prepareEnvironment: async (root) => {
        const sysfsRoot = join(root, "sys");
        const adapterRoot = join(sysfsRoot, "class", "net", "enx123");
        const deviceRoot = join(adapterRoot, "device");
        const udevRoot = join(root, "udev");
        await Promise.all([
          mkdir(join(adapterRoot, "statistics"), { recursive: true }),
          mkdir(deviceRoot, { recursive: true }),
          mkdir(udevRoot, { recursive: true })
        ]);
        await Promise.all([
          writeFile(join(adapterRoot, "address"), "02:00:00:00:00:01\n"),
          writeFile(join(adapterRoot, "perm_address"), "02:00:00:00:00:01\n"),
          writeFile(join(adapterRoot, "addr_assign_type"), "0\n"),
          writeFile(join(adapterRoot, "type"), "1\n"),
          writeFile(join(adapterRoot, "ifindex"), "17\n"),
          writeFile(join(adapterRoot, "statistics", "rx_bytes"), "0\n"),
          writeFile(join(adapterRoot, "statistics", "tx_bytes"), "0\n"),
          writeFile(join(deviceRoot, "id_path"), "ignored-explicit-device-path\n"),
          writeFile(join(deviceRoot, "serial"), "SERIAL-1\n"),
          writeFile(join(deviceRoot, "idVendor"), "1234\n"),
          writeFile(join(deviceRoot, "idProduct"), "5678\n"),
          writeFile(
            join(udevRoot, "n17"),
            "E:ID_SERIAL_SHORT=SERIAL-1\nE:ID_PATH=platform-axi-usb-0:1:1.0\n"
          )
        ]);
        return {
          EC_SYSTEMCORE_SYSFS_ROOT: sysfsRoot,
          EC_SYSTEMCORE_UDEV_DATA_ROOT: udevRoot
        };
      }
    });
    const csrfToken = await bootstrap(fixture);
    const fakeSysfsRoot = join(fixture.root, "sys");
    const fakeDevice = await realpath(join(fakeSysfsRoot, "class", "net", "enx123", "device"));
    expect({
      root: fakeSysfsRoot,
      device: fakeDevice,
      within: isPathWithin(fakeSysfsRoot, fakeDevice)
    }).toMatchObject({ within: true });
    const status = await (await fetch(`${fixture.baseUrl}/api/status`)).json() as {
      adapters: Array<{ name: string; identity: unknown }>;
    };
    expect(status.adapters.find((adapter) => adapter.name === "enx123")).toMatchObject({
      identity: {
        id_path: "platform-axi-usb-0:1:1.0",
        permanent_mac: "02:00:00:00:00:01"
      }
    });
    const confirmation =
      "LOCK:EC_USB:platform-axi-usb-0:1:1.0:02:00:00:00:00:01";
    const lock = await postJson(fixture, "/api/adapters/lock", csrfToken, {
      logicalName: "EC_USB",
      physicalInterface: "enx123",
      confirmation
    });
    const lockBody = await lock.json() as { ok: boolean; message?: string };
    expect({ status: lock.status, body: lockBody }).toMatchObject({
      status: 200,
      body: { ok: true }
    });
    const lockedConfig = JSON.parse(
      await readFile(join(fixture.root, "etc", "ec-systemcore.json"), "utf8")
    ) as {
      interface_mappings: Array<{
        logical_name: string;
        lock?: {
          id_path: string;
          permanent_mac?: string;
          usb_serial?: string;
          usb_vendor_id?: string;
          usb_product_id?: string;
        };
      }>;
    };
    expect(
      lockedConfig.interface_mappings.find((item) => item.logical_name === "EC_USB")?.lock
    ).toEqual({
      id_path: "platform-axi-usb-0:1:1.0",
      permanent_mac: "02:00:00:00:00:01",
      usb_serial: "SERIAL-1",
      usb_vendor_id: "1234",
      usb_product_id: "5678"
    });

    const unlock = await postJson(fixture, "/api/adapters/unlock", csrfToken, {
      logicalName: "EC_USB",
      confirmation: "UNLOCK:EC_USB"
    });
    expect(unlock.status).toBe(200);
    const unlockedConfig = JSON.parse(
      await readFile(join(fixture.root, "etc", "ec-systemcore.json"), "utf8")
    ) as {
      interface_mappings: Array<{ logical_name: string; lock?: unknown }>;
    };
    expect(
      unlockedConfig.interface_mappings.find((item) => item.logical_name === "EC_USB")?.lock
    ).toBeUndefined();
  });

  test("locks an ID_PATH-only adapter when no permanent MAC is available", async () => {
    const fixture = await createFixture({
      prepareEnvironment: async (root) => {
        const sysfsRoot = join(root, "sys");
        const adapterRoot = join(sysfsRoot, "class", "net", "enxpath");
        const deviceRoot = join(adapterRoot, "device");
        await Promise.all([
          mkdir(join(adapterRoot, "statistics"), { recursive: true }),
          mkdir(deviceRoot, { recursive: true })
        ]);
        await Promise.all([
          writeFile(join(adapterRoot, "address"), "02:00:00:00:00:99\n"),
          writeFile(join(adapterRoot, "addr_assign_type"), "1\n"),
          writeFile(join(adapterRoot, "type"), "1\n"),
          writeFile(join(adapterRoot, "ifindex"), "18\n"),
          writeFile(join(adapterRoot, "statistics", "rx_bytes"), "0\n"),
          writeFile(join(adapterRoot, "statistics", "tx_bytes"), "0\n"),
          writeFile(join(deviceRoot, "id_path"), "platform-usb-port-only\n")
        ]);
        return { EC_SYSTEMCORE_SYSFS_ROOT: sysfsRoot };
      }
    });
    const csrfToken = await bootstrap(fixture);
    const status = await (await fetch(`${fixture.baseUrl}/api/status`)).json() as {
      adapters: Array<{
        name: string;
        identity: { id_path: string; permanent_mac?: string } | null;
      }>;
    };
    const adapter = status.adapters.find((item) => item.name === "enxpath");
    expect(adapter?.identity).toEqual({ id_path: "platform-usb-port-only" });
    const lock = await postJson(fixture, "/api/adapters/lock", csrfToken, {
      logicalName: "EC_PATH",
      physicalInterface: "enxpath",
      confirmation: "LOCK:EC_PATH:platform-usb-port-only:"
    });
    expect(lock.status).toBe(200);
    const persisted = JSON.parse(
      await readFile(join(fixture.root, "etc", "ec-systemcore.json"), "utf8")
    ) as {
      interface_mappings: Array<{ logical_name: string; lock?: unknown }>;
    };
    expect(
      persisted.interface_mappings.find((item) => item.logical_name === "EC_PATH")?.lock
    ).toEqual({ id_path: "platform-usb-port-only" });
  });
});

describe("log and static-file containment", () => {
  test("keeps logs read-only, rejects symlink log/static files, and downloads only selected regular logs", async () => {
    const fixture = await createFixture();
    const csrfToken = await bootstrap(fixture);
    const logRoot = join(fixture.root, "logs");
    const archivedPath = join(logRoot, "archived.log");
    const activePath = join(logRoot, "active.log");
    await writeFile(archivedPath, "archived\n");
    await writeFile(activePath, "active\n");
    await utimes(archivedPath, new Date(1_700_000_000_000), new Date(1_700_000_000_000));
    await utimes(activePath, new Date(1_800_000_000_000), new Date(1_800_000_000_000));

    const outside = join(fixture.root, "outside.log");
    await writeFile(outside, "outside\n");
    let logSymlinkCreated = false;
    try {
      await symlink(outside, join(logRoot, "escape.log"), "file");
      logSymlinkCreated = true;
    } catch {
      // Windows may require Developer Mode for unprivileged symbolic links.
    }

    const logsResponse = await fetch(`${fixture.baseUrl}/api/logs`);
    const logsBody = await logsResponse.json() as { logs: Array<{ name: string }> };
    expect(logsBody.logs.map((entry) => entry.name)).toEqual(["active.log", "archived.log"]);
    if (logSymlinkCreated) {
      expect(logsBody.logs.some((entry) => entry.name === "escape.log")).toBe(false);
    }

    const activeDelete = await postJson(fixture, "/api/logs/delete", csrfToken, {
      names: ["active.log"]
    });
    expect(activeDelete.status).toBe(410);
    expect(await activeDelete.json()).toMatchObject({ code: "READ_ONLY_LOGS" });
    expect(await readFile(activePath, "utf8")).toBe("active\n");

    const download = await postJson(fixture, "/api/logs/download", csrfToken, {
      names: ["archived.log"]
    });
    expect(download.status).toBe(200);
    expect(download.headers.get("content-type")).toBe("application/zip");
    const archive = new Uint8Array(await download.arrayBuffer());
    expect([...archive.slice(0, 4)]).toEqual([0x50, 0x4b, 0x03, 0x04]);
    expect(new TextDecoder().decode(archive)).toContain("archived");

    const publicRoot = await mkdtemp(join(tmpdir(), "ec-systemcore-public-"));
    looseRoots.push(publicRoot);
    await Promise.all([
      writeFile(join(publicRoot, "index.html"), "safe shell"),
      writeFile(join(publicRoot, "tailwind.css"), ""),
      writeFile(join(publicRoot, "outside.js"), "throw new Error('outside')")
    ]);
    let staticSymlinkCreated = false;
    try {
      await symlink(join(publicRoot, "outside.js"), join(publicRoot, "app.js"), "file");
      staticSymlinkCreated = true;
    } catch {
      await mkdir(join(publicRoot, "app.js"));
    }
    const settings = loadSettings({
      EC_SYSTEMCORE_HOST: "127.0.0.1",
      EC_SYSTEMCORE_PORT: "8080",
      EC_SYSTEMCORE_CONFIG: join(publicRoot, "config", "ec-systemcore.json"),
      EC_SYSTEMCORE_LOG_ROOT: join(publicRoot, "logs"),
      EC_SYSTEMCORE_PUBLIC_DIR: publicRoot,
      EC_SYSTEMCORE_DISABLE_IPC: "true"
    });
    const runtime = new ConfigurationRuntime(settings);
    await runtime.initialize(false);
    const staticResponse = await runtime.handleRequest(
      new Request("http://127.0.0.1:8080/app.js", {
        headers: { host: "127.0.0.1:8080" }
      }),
      { clientIp: "127.0.0.1" }
    );
    expect(staticResponse?.status).toBe(404);
    if (staticSymlinkCreated) {
      expect(await staticResponse?.text()).toBe("Not found");
    }
    runtime.stop();
  });

  test("bounds directory enumeration and rejects invalid UTF-8 text files", async () => {
    const root = await mkdtemp(join(tmpdir(), "ec-systemcore-configuration-bounds-"));
    looseRoots.push(root);
    await Promise.all([
      writeFile(join(root, "one.log"), "one"),
      writeFile(join(root, "two.log"), "two"),
      writeFile(join(root, "three.log"), "three")
    ]);
    await expect(readDirectoryEntriesBounded(root, 2)).rejects.toThrow("too many entries");
    const invalidPath = join(root, "invalid.json");
    await writeFile(invalidPath, new Uint8Array([0xc3, 0x28]));
    await expect(readBoundedText(invalidPath, 16)).rejects.toThrow("valid UTF-8");
  });
});
