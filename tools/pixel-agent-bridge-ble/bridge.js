#!/usr/bin/env node
/** pixel-agent-bridge: HTTP hooks -> BLE NUS and/or USB serial -> T5 pixel */
const http = require("http");
const fs = require("fs");
const { spawn } = require("child_process");
const path = require("path");
const { SerialPort } = require("serialport");
const { runAction, pasteText } = require("./lib/keyboard");
const {
  BRIDGE_SERVER_HEADER,
  BRIDGE_SERVER_ID,
  DEFAULT_SERVER_PORT,
  getPortCandidates,
  writeRuntimeConfig,
  PERMISSION_PATH,
  STATE_PATH,
} = require("./lib/server-config");
const { pickAgentUart0, listWchPorts } = require("./lib/serial-ports");
const { resolveBlePython } = require("./lib/resolve-python");

const PASSTHROUGH_TOOLS = new Set([
  "TaskCreate", "TaskUpdate", "TaskGet", "TaskList", "TaskStop", "TaskOutput",
]);
const PERMISSION_AGENTS = new Set(["claude-code", "codebuddy"]);
const STATE_TO_SERIAL = {
  idle: "idle", thinking: "thinking", working: "working", juggling: "juggling",
  conducting: "juggling", error: "error", attention: "happy", notification: "notify",
  sweeping: "working", carrying: "working", sleeping: "idle",
};

const USE_BLE = process.argv.includes("--ble");
const BLE_ONLY = process.argv.includes("--ble-only");
const BLE_BIND_FILE = path.join(__dirname, ".pixel-ble-address");
const BLE_BIND_JSON = path.join(__dirname, ".pixel-ble-bind.json");

function loadBleBind() {
  const argAddr = process.argv.find((a) => a.startsWith("--ble-address="));
  const argChip = process.argv.find((a) => a.startsWith("--ble-chip="));
  const argName = process.argv.find((a) => a.startsWith("--ble-name="));
  if (argAddr) {
    return {
      address: argAddr.split("=")[1].trim(),
      chip: argChip ? argChip.split("=")[1].trim() : "",
      name: argName ? argName.split("=")[1].trim() : "",
    };
  }
  const envAddr = (process.env.PIXEL_BLE_ADDRESS || "").trim();
  if (envAddr) {
    return {
      address: envAddr,
      chip: (process.env.PIXEL_BLE_CHIP || "").trim(),
      name: (process.env.PIXEL_BLE_NAME || "").trim(),
    };
  }
  try {
    if (fs.existsSync(BLE_BIND_JSON)) {
      const rec = JSON.parse(fs.readFileSync(BLE_BIND_JSON, "utf8"));
      if (rec && rec.address) {
        return rec;
      }
    }
  } catch (_) {
    /* ignore */
  }
  try {
    if (fs.existsSync(BLE_BIND_FILE)) {
      return { address: fs.readFileSync(BLE_BIND_FILE, "utf8").trim(), chip: "", name: "" };
    }
  } catch (_) {
    /* ignore */
  }
  return { address: "", chip: "", name: "" };
}

function loadBleBindAddress() {
  return loadBleBind().address || "";
}

let serialPort = null;
let bleChild = null;
let bleRestartTimer = null;
let pendingPerm = null;
let rxLineBuf = "";
let lastTransportState = "S:idle";
let lastSentMappedState = "idle";
let bleLinked = false;
let bleGattTransport = null;
let activeTransport = "none";

function log(msg) {
  console.log(`[pixel-bridge] ${msg}`);
}

function mapState(state) {
  return STATE_TO_SERIAL[state] || "idle";
}

function serialWrite(line) {
  if (!serialPort || !serialPort.isOpen) {
    return false;
  }
  serialPort.write(line.endsWith("\n") ? line : `${line}\n`, (err) => {
    if (err) log(`serial write err: ${err.message}`);
  });
  return true;
}

function bleWrite(line) {
  const out = line.endsWith("\n") ? line : `${line}\n`;
  if (!bleChild || !bleChild.stdin || bleChild.stdin.destroyed) {
    return false;
  }
  bleChild.stdin.write(out);
  return true;
}

/**
 * Dual mode: BLE when linked, serial when BLE is down.
 * Serial-only / BLE-only still follow USE_BLE and BLE_ONLY flags.
 */
function pickOutboundTransport() {
  if (USE_BLE && bleLinked) {
    return "ble";
  }
  if (!BLE_ONLY && serialPort && serialPort.isOpen) {
    return "serial";
  }
  if (USE_BLE && bleChild && bleChild.stdin && !bleChild.stdin.destroyed) {
    return "ble-connecting";
  }
  return "none";
}

function resyncLastState() {
  if (!lastTransportState || lastTransportState === "S:idle") {
    return;
  }
  const via = pickOutboundTransport();
  if (via === "ble") {
    if (!bleWrite(lastTransportState)) {
      serialWrite(lastTransportState);
    }
  } else if (via === "serial") {
    serialWrite(lastTransportState);
  }
}

function setBleLinked(linked) {
  if (linked === bleLinked) {
    return;
  }
  bleLinked = linked;
  if (linked) {
    activeTransport = "ble";
    log("transport switch: serial -> BLE (notify ready)");
    /* L:1 is sent by ble_bridge.py on LINK: up — do not duplicate here */
    resyncLastState();
    return;
  }
  refreshActiveTransport();
  log(`transport switch: BLE -> ${activeTransport}`);
  if (activeTransport === "none" && !BLE_ONLY) {
    ensureSerialFallback().catch((e) => log(`serial fallback: ${e.message}`));
  }
}

function refreshActiveTransport() {
  if (USE_BLE && bleLinked) {
    activeTransport = "ble";
    return;
  }
  if (!BLE_ONLY && serialPort && serialPort.isOpen) {
    activeTransport = "serial";
    return;
  }
  activeTransport = "none";
}

function transportWrite(line) {
  if (line.startsWith("S:")) {
    lastTransportState = line.trim();
  }
  const via = pickOutboundTransport();
  if (via === "ble") {
    if (bleWrite(line)) {
      return;
    }
    if (serialWrite(line)) {
      log(`transport fallback: BLE -> serial for ${line.trim()}`);
      return;
    }
    log(`transport skip (ble write failed): ${line}`);
    return;
  }
  if (via === "serial") {
    serialWrite(line);
    return;
  }
  if (via === "ble-connecting" && !BLE_ONLY && serialPort && serialPort.isOpen) {
    serialWrite(line);
    return;
  }
  log(`transport skip (${via}): ${line}`);
}

function sendPermissionResponse(res, decision) {
  res.writeHead(200, {
    "Content-Type": "application/json",
    [BRIDGE_SERVER_HEADER]: BRIDGE_SERVER_ID,
  });
  res.end(JSON.stringify({
    hookSpecificOutput: { hookEventName: "PermissionRequest", decision },
  }));
}

function resolvePending(behavior) {
  if (!pendingPerm) return;
  const { res, suggestions } = pendingPerm;
  pendingPerm = null;
  transportWrite("P:");
  if (!res || res.writableEnded || res.destroyed) return;

  if (behavior === "deny") {
    sendPermissionResponse(res, { behavior: "deny", message: "Denied via pixel display" });
    return;
  }
  if (behavior === "always") {
    const addRules = (suggestions || []).find((s) => s && s.type === "addRules");
    if (addRules) {
      const rules = Array.isArray(addRules.rules)
        ? addRules.rules
        : [{ toolName: addRules.toolName, ruleContent: addRules.ruleContent }];
      sendPermissionResponse(res, {
        behavior: "allow",
        updatedPermissions: [{
          type: "addRules",
          destination: addRules.destination || "localSettings",
          behavior: addRules.behavior || "allow",
          rules,
        }],
      });
      return;
    }
  }
  sendPermissionResponse(res, { behavior: "allow" });
}

const recentDeviceLines = new Map();

function handleSerialLine(line, source) {
  const t = line.trim();
  const now = Date.now();
  const dedupeMs = t.startsWith("T:") ? 5000 : 120;
  const prev = recentDeviceLines.get(t);
  if (prev && now - prev < dedupeMs) {
    return;
  }
  recentDeviceLines.set(t, now);
  if (recentDeviceLines.size > 64) {
    for (const [k, ts] of recentDeviceLines) {
      if (now - ts > 5000) recentDeviceLines.delete(k);
    }
  }
  if (t.startsWith("K:") || t.startsWith("B:")) {
    log(`<- device (${source}): ${t}`);
  }
  if (t === "B:deny") resolvePending("deny");
  else if (t === "B:allow") resolvePending("allow");
  else if (t === "B:always") resolvePending("always");
  else if (t === "K:clear") runAction("clear");
  else if (t === "K:backspace") runAction("backspace");
  else if (t === "K:enter") runAction("enter");
  else if (t.startsWith("T:")) {
    const text = t.slice(2);
    log(`STT from device (${text.length} chars): ${text.slice(0, 60)}${text.length > 60 ? "…" : ""}`);
    pasteText(text);
  }
}

let bleRxLineBuf = "";

function feedDeviceRx(chunk, source) {
  if (source === "ble" && !bleLinked) {
    return;
  }
  /* BLE linked: uplink via notify only (serial is downlink mirror / fallback) */
  if (source === "serial" && bleLinked) {
    return;
  }
  const bufRef = source === "ble" ? bleRxLineBuf : rxLineBuf;
  let buf = bufRef + chunk.toString("utf8");
  let idx;
  while ((idx = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, idx);
    buf = buf.slice(idx + 1);
    if (line.trim()) handleSerialLine(line, source);
  }
  if (source === "ble") {
    bleRxLineBuf = buf;
  } else {
    rxLineBuf = buf;
  }
}

function feedBleRx(chunk) {
  feedDeviceRx(chunk, "ble");
}

function feedUartRx(chunk) {
  feedDeviceRx(chunk, "serial");
}

function handleStatePost(data) {
  const state = mapState(data.state);
  if (state === lastSentMappedState) {
    if (pendingPerm && ["PostToolUse", "PostToolUseFailure", "Stop"].includes(data.event)) {
      resolvePending("deny");
    }
    return;
  }
  lastSentMappedState = state;
  log(`state ${data.agent_id || "?"}: ${data.state} -> S:${state}`);
  transportWrite(`S:${state}`);
  if (pendingPerm && ["PostToolUse", "PostToolUseFailure", "Stop"].includes(data.event)) {
    resolvePending("deny");
  }
}

function getHealth() {
  const serialOpen = !!(serialPort && serialPort.isOpen);
  const bleProcess = !!(bleChild && !bleChild.killed);
  const transport = pickOutboundTransport();
  return {
    ok: true,
    ble_process: bleProcess,
    ble_linked: bleLinked,
    ble_gatt: bleGattTransport,
    serial: serialOpen,
    transport,
    active_transport: activeTransport,
    last_state: lastSentMappedState,
    pending_permission: !!pendingPerm,
  };
}

function handlePermissionPost(data, res) {
  const toolName = typeof data.tool_name === "string" ? data.tool_name : "Tool";
  if (PASSTHROUGH_TOOLS.has(toolName)) {
    sendPermissionResponse(res, { behavior: "allow" });
    return;
  }
  if (pendingPerm && pendingPerm.res && !pendingPerm.res.writableEnded) {
    try { pendingPerm.res.destroy(); } catch {}
  }
  pendingPerm = {
    res,
    suggestions: Array.isArray(data.permission_suggestions) ? data.permission_suggestions : [],
  };
  const safeTool = toolName.replace(/[^A-Za-z0-9_]/g, "").slice(0, 12) || "Tool";
  transportWrite(`P:tool=${safeTool}`);
  lastSentMappedState = "notify";
  transportWrite("S:notify");
  res.on("close", () => {
    if (pendingPerm && pendingPerm.res === res) {
      pendingPerm = null;
      transportWrite("P:");
    }
  });
}

function createServer() {
  return http.createServer((req, res) => {
    if (req.method === "GET" && req.url === "/health") {
      res.writeHead(200, {
        "Content-Type": "application/json",
        [BRIDGE_SERVER_HEADER]: BRIDGE_SERVER_ID,
      });
      res.end(JSON.stringify(getHealth()));
      return;
    }
    let body = "";
    req.on("data", (c) => { body += c; if (body.length > 524288) body = body.slice(0, 524288); });
    req.on("end", () => {
      try {
        if (req.method === "POST" && req.url === STATE_PATH) {
          handleStatePost(JSON.parse(body || "{}"));
          res.writeHead(200, { [BRIDGE_SERVER_HEADER]: BRIDGE_SERVER_ID });
          res.end("ok");
          return;
        }
        if (req.method === "POST" && req.url === PERMISSION_PATH) {
          const data = JSON.parse(body || "{}");
          if (!PERMISSION_AGENTS.has(data.agent_id || "claude-code")) {
            res.writeHead(200, { [BRIDGE_SERVER_HEADER]: BRIDGE_SERVER_ID });
            res.end("ok");
            return;
          }
          handlePermissionPost(data, res);
          return;
        }
        res.writeHead(404);
        res.end("not found");
      } catch {
        res.writeHead(400);
        res.end("bad request");
      }
    });
  });
}

function startHttp() {
  const ports = getPortCandidates(process.env.PIXEL_BRIDGE_PORT || DEFAULT_SERVER_PORT);
  let i = 0;
  const next = () => {
    if (i >= ports.length) {
      log("FATAL: no free port");
      process.exit(1);
    }
    const p = ports[i++];
    const server = createServer();
    server.on("error", () => next());
    server.listen(p, "127.0.0.1", () => {
      writeRuntimeConfig(p);
      log(`HTTP listening on 127.0.0.1:${p}`);
    });
  };
  next();
}

function openBle() {
  const nameArg = process.argv.find((a) => a.startsWith("--ble-name="));
  const bleName = (nameArg && nameArg.split("=")[1]) || process.env.PIXEL_BLE_NAME || "TYBLE";
  const bind = loadBleBind();
  const bleAddress = bind.address;
  const bleChip = bind.chip || "";
  const script = path.join(__dirname, "ble_bridge.py");
  const py = resolveBlePython();
  if (!py) {
    log("bleak not found — run ONE of:");
    log("  python3 -m pip install bleak          (if in venv / TuyaOpen)");
    log("  /usr/bin/python3 -m pip install bleak --user   (system Python)");
    log("  npm run setup:ble");
    return;
  }
  if (!bleAddress) {
    log("WARN: no BLE bind — run: npm run scan:ble && npm run bind:ble -- <address>");
  } else if (!bleChip) {
    log("WARN: bind has no chip tag — re-run: npm run bind:ble -- " + bleAddress);
  }
  const args = [script, "--name", bind.name || bleName];
  if (bleAddress) {
    args.push("--address", bleAddress);
  }
  if (bleChip) {
    args.push("--chip", bleChip);
  }
  bleChild = spawn(py, args, {
    stdio: ["pipe", "pipe", "pipe"],
  });
  bleChild.stdout.on("data", feedBleRx);
  bleChild.stderr.on("data", (chunk) => {
    const text = chunk.toString();
    if (text.includes("LINK: up")) {
      setBleLinked(true);
    }
    if (text.includes("LINK: down")) {
      setBleLinked(false);
    }
    const m = text.match(/transport=(\w+)/);
    if (m) {
      bleGattTransport = m[1];
    }
    process.stderr.write(chunk);
  });
  bleChild.on("error", (e) => log(`BLE spawn failed: ${e.message}`));
  bleChild.on("exit", (code, signal) => {
    bleChild = null;
    setBleLinked(false);
    if (signal === "SIGINT" || signal === "SIGTERM" || code === 0 || code === 130) {
      return;
    }
    log(`BLE bridge exited (${code ?? signal}) — restarting in 3s`);
    if (bleRestartTimer) {
      clearTimeout(bleRestartTimer);
    }
    bleRestartTimer = setTimeout(() => {
      openBle();
      resyncLastState();
    }, 3000);
  });
  const addrHint = bleAddress ? ` address=${bleAddress}` : "";
  const chipHint = bleChip ? ` chip=${bleChip}` : "";
  log(`BLE mode: ${py} ble_bridge.py --name ${bind.name || bleName}${addrHint}${chipHint}`);
}

async function ensureSerialFallback() {
  if (BLE_ONLY) {
    return;
  }
  if (serialPort && serialPort.isOpen) {
    refreshActiveTransport();
    return;
  }
  await openSerial();
  refreshActiveTransport();
  if (activeTransport === "serial") {
    log("serial fallback ready (BLE down)");
  }
}

async function openSerial() {
  const argPort = process.argv.find((a) => a.startsWith("--port="));
  const preferred = (argPort && argPort.split("=")[1]) || process.env.PIXEL_SERIAL_PORT;
  let pathToOpen = preferred;

  if (!pathToOpen) {
    const list = await SerialPort.list();
    const wch = listWchPorts(list);
    const uart0 = pickAgentUart0(list);
    if (uart0) {
      pathToOpen = uart0.path;
      if (process.platform === "darwin" && pathToOpen.includes("/tty.")) {
        pathToOpen = pathToOpen.replace("/tty.", "/cu.");
      }
      if (wch.length >= 2) {
        log(`UART0: ${pathToOpen} | log UART: ${wch[1].path.replace("/tty.", "/cu.")}`);
      }
    }
  }

  if (!pathToOpen) {
    log("WARN: no WCH port — run: npm run list-ports");
    return;
  }

  if (serialPort && serialPort.isOpen) {
    refreshActiveTransport();
    return;
  }

  await new Promise((resolve, reject) => {
    if (serialPort) {
      try {
        serialPort.removeAllListeners("data");
        serialPort.removeAllListeners("error");
        serialPort.close();
      } catch {
        /* ignore */
      }
    }
    serialPort = new SerialPort({ path: pathToOpen, baudRate: 115200 }, (err) => {
      if (err) {
        reject(err);
        return;
      }
      resolve();
    });
  });

  log(`Serial open: ${pathToOpen}`);
  serialPort.on("data", feedUartRx);
  serialPort.on("error", (e) => {
    log(`serial error: ${e.message}`);
    refreshActiveTransport();
    if (!BLE_ONLY) {
      setTimeout(() => {
        ensureSerialFallback().catch(() => {});
      }, 2000);
    }
  });
  refreshActiveTransport();
  if (!BLE_ONLY && !USE_BLE) {
    transportWrite("S:idle");
  } else if (!BLE_ONLY) {
    log("defer initial S:idle until BLE notify ready (or serial fallback)");
  }
}

async function main() {
  if (!BLE_ONLY) {
    await openSerial().catch((e) => log(`Serial failed: ${e.message}`));
  }
  if (USE_BLE) {
    openBle();
  }
  startHttp();
  const mode = USE_BLE ? (BLE_ONLY ? "BLE-only" : "BLE+serial (auto switch)") : "serial";
  log(`mode: ${mode} | outbound: BLE when linked else serial | perm: B=deny A=allow OK=always`);
  if (USE_BLE && !BLE_ONLY && serialPort && serialPort.isOpen) {
    log("hint: unplug USB from extra pixel boards — only the BLE target should be wireless");
    log("hint: macOS BLE address is not hardware MAC; bind by scan name TYBLE_XXXX after reflash");
  }
}

main();
