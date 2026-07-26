const state = {
  activeTab: "topology",
  activeSettingsSection: "general",
  status: null,
  buses: [],
  adapters: [],
  configState: null,
  config: null,
  logs: [],
  system: null,
  version: "development",
  capabilities: {
    readOnly: true,
    configWrite: false,
    logDelete: false,
    adapterLock: false,
    daemonDiagnostics: false
  },
  csrfToken: "",
  isSaving: false,
  socketAttempt: 0,
  socketTimer: null
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (character) => ({
  "&": "&amp;",
  "<": "&lt;",
  ">": "&gt;",
  "\"": "&quot;",
  "'": "&#39;"
}[character]));

const levelClass = (level) => ({
  good: "status-good",
  warning: "status-warning",
  error: "status-error",
  muted: "status-muted"
}[level] || "status-muted");

function toast(message, level = "good") {
  const node = $("#toast");
  node.textContent = message;
  node.className = `fixed top-4 right-4 z-50 rounded px-4 py-2 text-sm font-semibold shadow-lg ${levelClass(level)}`;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => node.classList.add("hidden"), 3000);
}

async function bootstrap() {
  const response = await fetch("/api/bootstrap", {
    method: "POST",
    headers: { "x-ec-systemcore-bootstrap": "1" },
    credentials: "same-origin"
  });
  if (!response.ok) throw new Error("Configuration bootstrap was rejected");
  const data = await response.json();
  state.csrfToken = data.csrfToken;
  state.version = data.version;
  state.capabilities = data.capabilities;
  renderMaintenanceState();
}

async function api(path, options = {}, retryCsrf = true) {
  const mutation = options.method && options.method !== "GET";
  const headers = new Headers(options.headers);
  if (mutation) {
    headers.set("content-type", "application/json");
    headers.set("x-ec-systemcore-csrf", state.csrfToken);
  }
  const response = await fetch(path, {
    ...options,
    headers,
    credentials: "same-origin"
  });
  if (response.status === 403 && mutation && retryCsrf && state.capabilities.readOnly === false) {
    await bootstrap();
    return api(path, options, false);
  }
  if (!response.ok) {
    let message = `Request failed (${response.status})`;
    try {
      const body = await response.json();
      if (body.message) message = body.message;
    } catch {
      // Keep the sanitized status message.
    }
    throw new Error(message);
  }
  return response;
}

async function runAction(operation) {
  try {
    await operation();
  } catch (error) {
    toast(error instanceof Error ? error.message : "Action failed", "error");
  }
}

function renderMaintenanceState() {
  $("#readOnlyBanner").classList.toggle("hidden", !state.capabilities.readOnly);
  $$(".mutation-control").forEach((node) => {
    node.disabled = state.capabilities.readOnly;
    node.title = state.capabilities.readOnly
      ? "Read-only network mode"
      : "";
  });
}

async function loadStatus() {
  const data = await (await api("/api/status")).json();
  state.status = data.status;
  state.buses = data.buses || [];
  state.adapters = data.adapters || [];
  state.configState = data.configState;
  state.config = data.configState.config;
  state.system = data.system;
  state.version = data.version;
  state.capabilities = data.capabilities;
  renderAll();
}

async function loadConfig() {
  const data = await (await api("/api/config")).json();
  state.configState = data.configState;
  state.config = data.configState.config;
  state.capabilities = data.capabilities;
  renderSettings();
  renderMaintenanceState();
}

async function loadLogs() {
  const data = await (await api("/api/logs")).json();
  state.logs = data.logs;
  $("#liveLogNotice").textContent = data.liveConsole.message;
  renderLogs();
}

function renderAll() {
  renderMaintenanceState();
  renderFailsafe();
  renderHeader();
  renderTopology();
  if (!state.isSaving) renderSettings();
}

function renderFailsafe() {
  const failed = !state.configState?.isConfigLoaded || state.configState?.regeneratedFailsafe;
  $("#globalFailsafeBanner").classList.toggle("hidden", !failed);
  $("#failsafeBanner").classList.toggle("hidden", !failed);
}

function renderHeader() {
  const status = state.status || {};
  const daemonLevel = status.connected && !status.stale
    ? status.operational ? "good" : "warning"
    : "error";
  const adapterLevel = status.activeAdapters > 0 ? "good" : status.connected ? "warning" : "muted";
  const deviceLevel = status.subDeviceCount > 0 ? "good" : status.connected ? "warning" : "muted";
  const faultLevel = status.activeFaults > 0 ? "error" : status.connected ? "good" : "muted";
  const timingLevel = status.distributedClockUnlocked
    ? "error"
    : Number(status.cycleOverruns || 0) > 0 ? "warning" : status.connected ? "good" : "muted";
  const dots = [
    ["Daemon", daemonLevel, status.stale ? "Daemon status is stale/offline" : `Aggregate state ${status.aggregateState}`],
    ["Adapters", adapterLevel, `${status.activeAdapters || 0} active daemon adapters`],
    ["Devices", deviceLevel, `${status.subDeviceCount || 0} daemon-reported SubDevices`],
    ["Faults", faultLevel, `${status.activeFaults || 0} active aggregate faults`, "faults"],
    [
      "Timing",
      timingLevel,
      status.distributedClockUnlocked
        ? "Distributed clock is not phase-locked; outputs remain fail-closed"
        : `${status.cycleOverruns || "0"} deadline misses`
    ]
  ];
  $("#statusDots").innerHTML = dots.map(([label, level, title, action]) =>
    `<button class="status-pill ${levelClass(level)}" title="${escapeHtml(title)}" data-action="${escapeHtml(action || "")}" type="button">● ${escapeHtml(label)}</button>`
  ).join("");

  const jitter = $("#jitterIndicator");
  if (!status.connected || status.stale) {
    jitter.textContent = "Jitter unavailable";
    jitter.className = "rounded border px-3 py-1 text-sm font-semibold status-muted";
  } else {
    const current = status.currentJitterUs || 0;
    const maximum = status.maxJitterUs || 0;
    const level = current < 100 ? "good" : current <= 500 ? "warning" : "error";
    jitter.textContent = `Jitter ${current}µs current · ${maximum}µs max`;
    jitter.className = `rounded border px-3 py-1 text-sm font-semibold ${levelClass(level)}`;
  }
}

function busStateLabel(value) {
  return ({
    0: "Starting",
    1: "Waiting for link",
    2: "Initializing",
    3: "Safe operational",
    4: "Operational",
    5: "Error",
    6: "Stopping"
  })[value] || `State ${value}`;
}

function lockStateLevel(value) {
  if (value === "matched" || value === "unlocked") return "good";
  if (value === "missing" || value === "mismatch") return "error";
  return "warning";
}

function renderTopology() {
  const map = $("#topologyMap");
  if (!state.status?.connected || state.status?.stale) {
    map.innerHTML = `<div class="empty-state">Daemon status is offline or stale. The configuration will reconnect automatically.</div>`;
    return;
  }
  if (!state.buses.length) {
    map.innerHTML = `<div class="empty-state">${state.status.subDeviceCount || 0} aggregate SubDevices reported; per-bus detail has not arrived yet.</div>`;
    return;
  }
  map.innerHTML = state.buses.map((bus) => `
    <div class="topology-node">
      <span class="node-kicker">Bus ${bus.busIndex}</span>
      <strong>${escapeHtml(bus.logicalName || "Unnamed bus")}</strong>
      <small>${escapeHtml(bus.physicalInterface || "No active interface")}</small>
      <div class="${levelClass(bus.state === 4 ? "good" : bus.state === 5 ? "error" : "warning")} rounded px-2 py-1 text-xs">${escapeHtml(busStateLabel(bus.state))}</div>
      <div class="${levelClass(lockStateLevel(bus.lockState))} rounded px-2 py-1 text-xs">Lock: ${escapeHtml(bus.lockState)}</div>
      <small>${bus.slaveCount} SubDevices reported; individual state is unavailable in this view.</small>
    </div>
  `).join("");
}

function formatBytes(bytes) {
  const safe = Number.isFinite(bytes) && bytes >= 0 ? bytes : 0;
  if (safe < 1024) return `${safe} B`;
  if (safe < 1024 * 1024) return `${Math.round(safe / 1024)} KB`;
  return `${(safe / 1024 / 1024).toFixed(1)} MB`;
}

function renderLogs() {
  $("#logFileList").innerHTML = state.logs.map((log) => `
    <label class="log-row">
      <input type="checkbox" value="${escapeHtml(log.name)}" />
      <span class="min-w-0 flex-1 truncate">${escapeHtml(log.name)}</span>
      <span class="text-xs text-slate-500">${escapeHtml(log.format)} · ${formatBytes(log.size)}</span>
    </label>
  `).join("") || `<div class="empty-state">No .log or .wpilog files found.</div>`;
}

function selectedLogNames() {
  return $$("#logFileList input:checked").map((item) => item.value);
}

function setValueIfIdle(selector, value) {
  const node = $(selector);
  if (node && document.activeElement !== node) node.value = value;
}

function setCheckedIfIdle(selector, checked) {
  const node = $(selector);
  if (node && document.activeElement !== node) node.checked = checked;
}

function identitySummary(identity) {
  if (!identity) return "Stable identity unavailable; this adapter cannot be locked.";
  return [
    `ID_PATH ${identity.id_path}`,
    identity.permanent_mac ? `permanent MAC ${identity.permanent_mac}` : null,
    identity.usb_serial ? `serial ${identity.usb_serial}` : null,
    identity.usb_vendor_id && identity.usb_product_id
      ? `USB ${identity.usb_vendor_id}:${identity.usb_product_id}`
      : null
  ].filter(Boolean).join(" · ");
}

function renderSettings() {
  const config = state.config;
  if (!config) return;
  setValueIfIdle("#cyclePeriod", String(config.cycle_period_us));
  setValueIfIdle("#heartbeatTimeout", String(config.heartbeat_timeout_ms));
  setValueIfIdle("#outputCommandTimeout", String(config.output_command_timeout_ms));
  setValueIfIdle("#logDirectory", config.log_directory);
  setValueIfIdle("#logLimit", String(config.log_count_limit));
  setValueIfIdle("#freeSpaceThreshold", String(config.free_space_threshold_mb));
  setCheckedIfIdle("#allowRestricted", Boolean(config.allow_restricted_interfaces));
  const advancedEditor = $("#advancedConfigJson");
  if (
    advancedEditor
    && document.activeElement !== advancedEditor
    && advancedEditor.dataset.dirty !== "true"
  ) {
    advancedEditor.value = `${JSON.stringify(config, null, 2)}\n`;
  }

  if (!document.activeElement?.closest("#networkList")) {
    $("#networkList").innerHTML = state.adapters.map((adapter, index) => {
      const stateLevel = lockStateLevel(adapter.lockState);
      const canLock = adapter.detected && adapter.identity && !adapter.lockConfigured;
      const mapping = adapter.configured
        ? config.interface_mappings.find((item) => item.logical_name === adapter.logicalName)
        : null;
      return `
        <div class="network-row" data-adapter-index="${index}" data-mapping-logical="${escapeHtml(mapping?.logical_name || "")}">
          <div class="min-w-0 grid gap-2">
            <label class="field-label">Logical name
              <input class="field-input network-logical-name" maxlength="32" value="${escapeHtml(adapter.logicalName)}" ${adapter.lockConfigured ? "readonly" : ""} />
            </label>
            ${adapter.configured ? `<label class="toggle-row"><input class="network-enabled" type="checkbox" ${adapter.enabled ? "checked" : ""} /> Enabled</label>` : ""}
            <strong>${escapeHtml(adapter.name)}</strong>
            <div class="text-xs text-slate-400">${escapeHtml(identitySummary(adapter.identity))}</div>
            <div class="${levelClass(stateLevel)} rounded px-2 py-1 text-xs">Lock state: ${escapeHtml(adapter.lockState)}${adapter.detected ? "" : " · adapter not present"}</div>
          </div>
          <div class="grid gap-2 text-right text-xs text-slate-400">
            <div>RX ${formatBytes(adapter.rxBytesPerSecond)}/s</div>
            <div>TX ${formatBytes(adapter.txBytesPerSecond)}/s</div>
            <button class="btn-secondary" data-adapter="${index}" type="button">Identity</button>
            ${canLock ? `<button class="btn-primary mutation-control" data-lock-adapter="${index}" type="button">Lock stable identity</button>` : ""}
            ${adapter.lockConfigured ? `<button class="btn-danger mutation-control" data-unlock-adapter="${index}" type="button">Unlock / recover</button>` : ""}
          </div>
        </div>
      `;
    }).join("") || `<div class="empty-state">No adapters or configured mappings detected.</div>`;
  }

  $("#subDeviceList").innerHTML = state.buses.map((bus) => `
    <div class="dense-row">
      <span>Bus ${bus.busIndex}: ${escapeHtml(bus.logicalName)}</span>
      <span>${escapeHtml(busStateLabel(bus.state))} · ${bus.slaveCount} aggregate SubDevices · ${escapeHtml(bus.lockState)}</span>
    </div>
  `).join("") || `<div class="empty-state">No per-bus status available. Aggregate count: ${state.status?.subDeviceCount || 0}.</div>`;

  const system = state.system || {};
  const schedulingDescription = system.schedulingMode === "realtime-enhanced"
    ? "Optional real-time enhancements are active."
    : system.realtimeEnhancementsRequested && system.timingDegraded
      ? `Standard-kernel operation remains supported; requested optional real-time enhancements were degraded (diagnostic bits ${system.realtimeErrorBits || 0}).`
    : system.realtimeEnhancementsAvailable
      ? "Standard-kernel operation is supported; optional real-time enhancements are available but not active."
      : "Standard-kernel operation is supported; PREEMPT_RT is not required.";
  $("#aboutInfo").innerHTML = `
    <div>ec-systemcore version ${escapeHtml(state.version)}</div>
    <div>CPU: ${escapeHtml(system.model || "Unknown")}</div>
    <div>RAM: ${system.memAvailableMb || 0} MB available / ${system.memTotalMb || 0} MB total</div>
    <div>Scheduling: ${escapeHtml(schedulingDescription)}</div>
    <div>Distributed clock: ${state.status?.distributedClockUnlocked
      ? "Unlocked — configured DC buses remain fail-closed"
      : "No aggregate unlock reported"}</div>
    <div>Deadline misses: ${escapeHtml(system.cycleOverruns || "0")}</div>
    <div>Management mode: ${state.capabilities.readOnly ? "Read-only" : "Trusted robot-management network"}</div>
  `;
  renderMaintenanceState();
}

function appendConsoleLine(line) {
  const output = $("#consoleOutput");
  const lowered = String(line).toLowerCase();
  const level = lowered.includes("error")
    ? "console-error"
    : lowered.includes("warning") ? "console-warning" : "console-line";
  const row = document.createElement("div");
  row.className = level;
  row.textContent = String(line);
  output.appendChild(row);
  output.scrollTop = output.scrollHeight;
  while (output.children.length > 800) output.firstChild.remove();
}

function connectLogs() {
  if (!state.csrfToken) return;
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  let socket;
  try {
    socket = new WebSocket(
      `${protocol}//${location.host}/ws/logs`,
      ["ec-systemcore-v1", `csrf.${state.csrfToken}`]
    );
  } catch {
    scheduleSocketReconnect();
    return;
  }
  socket.addEventListener("open", () => {
    state.socketAttempt = 0;
    $("#socketState").textContent = "Streaming";
  });
  socket.addEventListener("close", () => {
    $("#socketState").textContent = "Disconnected · retrying";
    scheduleSocketReconnect();
  });
  socket.addEventListener("error", () => {
    $("#socketState").textContent = "Connection error";
  });
  socket.addEventListener("message", (event) => {
    try {
      const message = JSON.parse(event.data);
      if (message.type === "log") appendConsoleLine(message.line);
      if (message.type === "log_notice") appendConsoleLine(`notice: ${message.message}`);
      if (message.type === "status") {
        state.status = message.status;
        state.buses = message.buses || [];
        renderHeader();
        renderTopology();
        renderSettings();
      }
      if (message.type === "config_loaded") {
        state.configState = message.configState;
        state.config = message.configState.config;
        renderFailsafe();
        renderSettings();
      }
    } catch {
      appendConsoleLine("warning: malformed diagnostic stream message ignored");
    }
  });
}

function scheduleSocketReconnect() {
  if (state.socketTimer) return;
  const delay = Math.min(5000, 250 * (2 ** Math.min(state.socketAttempt, 5)));
  state.socketAttempt += 1;
  state.socketTimer = setTimeout(() => {
    state.socketTimer = null;
    connectLogs();
  }, delay);
}

function showFaultModal() {
  const status = state.status || {};
  $("#faultList").innerHTML = `
    <div class="dense-row"><span>Active aggregate faults</span><strong>${status.activeFaults || 0}</strong></div>
    <div class="dense-row"><span>Lost frames</span><strong>${escapeHtml(status.lostFrames || "0")}</strong></div>
    <div class="dense-row"><span>Deadline misses</span><strong>${escapeHtml(status.cycleOverruns || "0")}</strong></div>
  `;
  $("#faultModal").showModal();
}

function showAdapterModal(index) {
  const adapter = state.adapters[index];
  if (!adapter) return;
  $("#adapterDetails").innerHTML = `
    <div class="dense-row"><span>Logical name</span><strong>${escapeHtml(adapter.logicalName)}</strong></div>
    <div class="dense-row"><span>Observed interface</span><strong>${escapeHtml(adapter.name)}</strong></div>
    <div class="dense-row"><span>Configured interface hint</span><strong>${escapeHtml(adapter.configuredPhysicalInterface)}</strong></div>
    <div class="dense-row"><span>Current MAC</span><strong>${escapeHtml(adapter.currentMac || "Unavailable")}</strong></div>
    <div class="dense-row"><span>Stable identity</span><strong>${escapeHtml(identitySummary(adapter.identity))}</strong></div>
    <div class="dense-row"><span>Lock state</span><strong>${escapeHtml(adapter.lockState)}</strong></div>
    <div class="dense-row"><span>Detected now</span><strong>${adapter.detected ? "Yes" : "No"}</strong></div>
  `;
  $("#adapterModal").showModal();
}

function collectMappings() {
  return $$("#networkList .network-row").flatMap((row) => {
    const originalLogical = row.dataset.mappingLogical;
    if (!originalLogical) return [];
    const original = state.config.interface_mappings.find((item) => item.logical_name === originalLogical);
    if (!original) return [];
    return [{
      ...original,
      logical_name: row.querySelector(".network-logical-name").value.trim(),
      enabled: row.querySelector(".network-enabled").checked
    }];
  });
}

function bindEvents() {
  $$(".tab-button").forEach((button) => button.addEventListener("click", () => {
    state.activeTab = button.dataset.tab;
    $$(".tab-button").forEach((item) => item.classList.toggle("active", item === button));
    $$(".view-block").forEach((view) => view.classList.add("hidden"));
    $(`#${state.activeTab}View`).classList.remove("hidden");
    if (state.activeTab === "logs") void runAction(loadLogs);
  }));

  $$(".settings-nav").forEach((button) => button.addEventListener("click", () => {
    state.activeSettingsSection = button.dataset.settingsSection;
    $$(".settings-nav").forEach((item) => item.classList.toggle("active", item === button));
    $$(".settings-panel").forEach((panel) => panel.classList.add("hidden"));
    const suffix = state.activeSettingsSection[0].toUpperCase() + state.activeSettingsSection.slice(1);
    $(`#settingsPanel${suffix}`).classList.remove("hidden");
  }));

  document.addEventListener("click", (event) => {
    const target = event.target.closest(
      "[data-action], [data-adapter], [data-lock-adapter], [data-unlock-adapter], [data-close-modal]"
    );
    if (!target) return;
    if (target.dataset.action === "faults") showFaultModal();
    if (target.dataset.adapter !== undefined) showAdapterModal(Number(target.dataset.adapter));
    if (target.dataset.closeModal !== undefined) target.closest("dialog")?.close();
    if (target.dataset.lockAdapter !== undefined) {
      void runAction(async () => {
        const adapter = state.adapters[Number(target.dataset.lockAdapter)];
        const row = target.closest(".network-row");
        const logicalName = row.querySelector(".network-logical-name").value.trim();
        if (!adapter?.identity || !logicalName) throw new Error("Stable identity and logical name are required");
        const summary = identitySummary(adapter.identity);
        if (!window.confirm(`Lock ${logicalName} to this stable identity?\n\n${summary}\n\nA missing or replaced adapter will fail closed.`)) return;
        const confirmation =
          `LOCK:${logicalName}:${adapter.identity.id_path}:${adapter.identity.permanent_mac || ""}`;
        await api("/api/adapters/lock", {
          method: "POST",
          body: JSON.stringify({
            logicalName,
            physicalInterface: adapter.name,
            confirmation
          })
        });
        toast("Stable adapter lock saved; daemon will restart safely");
        await loadStatus();
      });
    }
    if (target.dataset.unlockAdapter !== undefined) {
      void runAction(async () => {
        const adapter = state.adapters[Number(target.dataset.unlockAdapter)];
        if (!adapter) return;
        if (!window.confirm(`Remove the stable lock for ${adapter.logicalName}? This is the recovery path for a missing or replaced adapter.`)) return;
        await api("/api/adapters/unlock", {
          method: "POST",
          body: JSON.stringify({
            logicalName: adapter.logicalName,
            confirmation: `UNLOCK:${adapter.logicalName}`
          })
        });
        toast("Adapter lock removed; daemon will restart safely");
        await loadStatus();
      });
    }
  });

  $("#refreshTopology").addEventListener("click", () => void runAction(loadStatus));
  $("#refreshLogs").addEventListener("click", () => void runAction(loadLogs));
  $("#rescanAdapters").addEventListener("click", () => void runAction(async () => {
    await api("/api/adapters/rescan", { method: "POST", body: "{}" });
    toast("Adapter rescan requested");
  }));

  $("#downloadSelected").addEventListener("click", () => void runAction(async () => {
    const names = selectedLogNames();
    if (!names.length) return;
    const response = await api("/api/logs/download", {
      method: "POST",
      body: JSON.stringify({ names })
    });
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `ec-systemcore-logs-${Date.now()}.zip`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }));

  $("#saveConfig").addEventListener("click", () => void runAction(async () => {
    if (!window.confirm("Validate and save this configuration? The daemon will safe-disable and restart.")) return;
    state.isSaving = true;
    renderMaintenanceState();
    try {
      await api("/api/config", {
        method: "POST",
        body: JSON.stringify({
          ...state.config,
          allow_restricted_interfaces: $("#allowRestricted").checked,
          cycle_period_us: Number($("#cyclePeriod").value),
          heartbeat_timeout_ms: Number($("#heartbeatTimeout").value),
          output_command_timeout_ms: Number($("#outputCommandTimeout").value),
          log_directory: $("#logDirectory").value.trim(),
          log_count_limit: Number($("#logLimit").value),
          free_space_threshold_mb: Number($("#freeSpaceThreshold").value),
          interface_mappings: collectMappings()
        })
      });
      toast("Configuration saved atomically");
      await loadConfig();
    } finally {
      state.isSaving = false;
      renderMaintenanceState();
    }
  }));

  $("#advancedConfigJson").addEventListener("input", (event) => {
    event.currentTarget.dataset.dirty = "true";
    $("#advancedConfigError").textContent = "";
  });

  $("#resetAdvancedConfig").addEventListener("click", () => {
    const editor = $("#advancedConfigJson");
    editor.value = `${JSON.stringify(state.config, null, 2)}\n`;
    editor.dataset.dirty = "false";
    $("#advancedConfigError").textContent = "";
  });

  $("#applyAdvancedConfig").addEventListener("click", () => void runAction(async () => {
    const editor = $("#advancedConfigJson");
    const errorNode = $("#advancedConfigError");
    let candidate;
    try {
      candidate = JSON.parse(editor.value);
    } catch (error) {
      const message = error instanceof Error ? error.message : "Malformed JSON";
      errorNode.textContent = `JSON syntax error: ${message}`;
      throw new Error("Fix the highlighted JSON syntax error before applying");
    }
    if (!window.confirm(
      "Validate and atomically apply the complete configuration? "
      + "The daemon will safe-disable outputs and restart."
    )) return;
    state.isSaving = true;
    renderMaintenanceState();
    try {
      await api("/api/config", {
        method: "POST",
        body: JSON.stringify(candidate)
      });
      errorNode.textContent = "";
      editor.dataset.dirty = "false";
      toast("Advanced configuration validated and saved atomically");
      await loadConfig();
    } catch (error) {
      const message = error instanceof Error ? error.message : "Validation failed";
      errorNode.textContent = `Validation failed: ${message}`;
      throw error;
    } finally {
      state.isSaving = false;
      renderMaintenanceState();
    }
  }));

  $("#applyDefaults").addEventListener("click", () => void runAction(async () => {
    if (!window.confirm("Replace the active configuration with explicit safe defaults?")) return;
    await api("/api/config/apply-defaults", { method: "POST", body: "{}" });
    toast("Defaults applied");
    await loadStatus();
  }));

  $("#clearFaults").addEventListener("click", () => void runAction(async () => {
    if (!window.confirm("Clear daemon diagnostic counters? This does not write process outputs.")) return;
    await api("/api/faults/clear", { method: "POST", body: "{}" });
    toast("Counter clear requested");
    $("#faultModal").close();
  }));
}

async function start() {
  bindEvents();
  await bootstrap();
  await Promise.all([loadStatus(), loadLogs()]);
  connectLogs();
}

void start().catch((error) => {
  appendConsoleLine(`error: ${error instanceof Error ? error.message : "Configuration startup failed"}`);
  toast("Configuration startup failed", "error");
});
