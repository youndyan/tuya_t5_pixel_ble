const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");

const BRIDGE_SERVER_ID = "pixel-agent-bridge-ble";
const BRIDGE_SERVER_HEADER = "x-pixel-bridge-ble";
const DEFAULT_SERVER_PORT = 23340;
const SERVER_PORT_COUNT = 5;
const SERVER_PORTS = Array.from({ length: SERVER_PORT_COUNT }, (_, i) => DEFAULT_SERVER_PORT + i);
const STATE_PATH = "/state";
const PERMISSION_PATH = "/permission";
const RUNTIME_CONFIG_PATH = path.join(os.homedir(), ".pixel-agent-bridge-ble", "runtime.json");

function normalizePort(value) {
  const port = Number(value);
  return Number.isInteger(port) && SERVER_PORTS.includes(port) ? port : null;
}

function readRuntimePort() {
  try {
    const raw = JSON.parse(fs.readFileSync(RUNTIME_CONFIG_PATH, "utf8"));
    return normalizePort(raw && raw.port);
  } catch {
    return null;
  }
}

function writeRuntimeConfig(port) {
  const safePort = normalizePort(port);
  if (!safePort) return false;
  const dir = path.dirname(RUNTIME_CONFIG_PATH);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(RUNTIME_CONFIG_PATH, JSON.stringify({ app: BRIDGE_SERVER_ID, port: safePort }, null, 2), "utf8");
  return true;
}

function getPortCandidates(preferredPort) {
  const ports = [];
  const seen = new Set();
  const add = (v) => {
    const p = normalizePort(v);
    if (p && !seen.has(p)) { seen.add(p); ports.push(p); }
  };
  add(preferredPort);
  add(readRuntimePort());
  SERVER_PORTS.forEach(add);
  return ports;
}

function buildPermissionUrl(port) {
  return `http://127.0.0.1:${normalizePort(port) || DEFAULT_SERVER_PORT}${PERMISSION_PATH}`;
}

function postStateToPort(port, payload, timeoutMs, callback) {
  const req = http.request({
    hostname: "127.0.0.1",
    port,
    path: STATE_PATH,
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Content-Length": Buffer.byteLength(payload),
    },
    timeout: timeoutMs,
  }, (res) => {
    res.resume();
    callback(res.statusCode === 200, port);
  });
  req.on("error", () => callback(false, port));
  req.on("timeout", () => { req.destroy(); callback(false, port); });
  req.end(payload);
}

function postStateToRunningServer(payload, options, callback) {
  const timeoutMs = (options && options.timeoutMs) || 150;
  const ports = getPortCandidates(options && options.preferredPort);
  let i = 0;
  const next = () => {
    if (i >= ports.length) { callback(false); return; }
    const port = ports[i++];
    postStateToPort(port, payload, timeoutMs, (ok) => {
      if (ok) callback(true, port);
      else next();
    });
  };
  next();
}

function resolveNodeBin() {
  try {
    return process.execPath;
  } catch {
    return "node";
  }
}

module.exports = {
  BRIDGE_SERVER_ID,
  BRIDGE_SERVER_HEADER,
  DEFAULT_SERVER_PORT,
  STATE_PATH,
  PERMISSION_PATH,
  readRuntimePort,
  writeRuntimeConfig,
  getPortCandidates,
  buildPermissionUrl,
  postStateToRunningServer,
  resolveNodeBin,
};
