#!/usr/bin/env node
/**
 * Register IDE hooks -> pixel-agent-bridge (127.0.0.1:23335)
 */
const fs = require("fs");
const path = require("path");
const os = require("os");
const {
  buildPermissionUrl,
  DEFAULT_SERVER_PORT,
  readRuntimePort,
  resolveNodeBin,
} = require("./lib/server-config");

const ROOT = __dirname;
const HOOKS = path.join(ROOT, "hooks");
const STATE_HOOK = path.join(HOOKS, "state-hook.js");

function readJson(p) {
  return JSON.parse(fs.readFileSync(p, "utf8"));
}

function writeJson(p, data) {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, JSON.stringify(data, null, 2), "utf8");
}

function quote(v) {
  return `"${String(v).replace(/"/g, '\\"')}"`;
}

function mergeClaudeStyleHook(settings, event, command, marker) {
  if (!settings.hooks) settings.hooks = {};
  if (!Array.isArray(settings.hooks[event])) settings.hooks[event] = [];
  for (const entry of settings.hooks[event]) {
    for (const h of (entry.hooks || (entry.command ? [entry] : []))) {
      if (h.command && h.command.includes(marker)) {
        if (h.command !== command) { h.command = command; return "updated"; }
        return "skipped";
      }
    }
  }
  settings.hooks[event].push({ matcher: "", hooks: [{ type: "command", command, timeout: 10 }] });
  return "added";
}

function countResult(r, stats) {
  if (r === "added") stats.added++;
  if (r === "updated") stats.updated++;
  if (r === "skipped") stats.skipped++;
}

function installClaude() {
  const settingsPath = path.join(os.homedir(), ".claude", "settings.json");
  const nodeBin = resolveNodeBin();
  const script = path.join(HOOKS, "agent-state-hook.js");
  const marker = "agent-state-hook.js";
  const events = [
    "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "PostToolUseFailure", "Stop", "SubagentStart", "SubagentStop",
    "Notification", "Elicitation", "WorktreeCreate",
  ];
  let settings = {};
  try { settings = readJson(settingsPath); } catch (e) { if (e.code !== "ENOENT") throw e; }

  const stats = { added: 0, updated: 0, skipped: 0 };
  for (const event of events) {
    countResult(mergeClaudeStyleHook(settings, event, `"${nodeBin}" "${script}" ${event}`, marker), stats);
  }
  settings.hooks.PermissionRequest = [{
    matcher: "",
    hooks: [{ type: "http", url: buildPermissionUrl(DEFAULT_SERVER_PORT), timeout: 600 }],
  }];
  writeJson(settingsPath, settings);
  console.log(`Claude: ~/.claude/settings.json (+${stats.added} ~${stats.updated})`);
}

function installCursor() {
  const hooksPath = path.join(os.homedir(), ".cursor", "hooks.json");
  if (!fs.existsSync(path.dirname(hooksPath))) {
    console.log("Cursor: skip (~/.cursor not found)");
    return;
  }
  const nodeBin = resolveNodeBin();
  const script = path.join(HOOKS, "cursor-agent-hook.js");
  const marker = "cursor-agent-hook.js";
  const events = [
    "sessionStart", "sessionEnd", "beforeSubmitPrompt", "preToolUse", "postToolUse",
    "postToolUseFailure", "subagentStart", "subagentStop", "preCompact", "afterAgentThought", "stop",
  ];
  let settings = {};
  try { settings = readJson(hooksPath); } catch {}
  if (!settings.hooks) settings.hooks = {};
  if (typeof settings.version !== "number") settings.version = 1;

  const stats = { added: 0, updated: 0, skipped: 0 };
  for (const event of events) {
    countResult(mergeClaudeStyleHook(settings, event, `"${nodeBin}" "${script}" ${event}`, marker), stats);
  }
  writeJson(hooksPath, settings);
  console.log(`Cursor: ~/.cursor/hooks.json (+${stats.added} ~${stats.updated})`);
}

function installCodex() {
  const codexDir = path.join(os.homedir(), ".codex");
  if (!fs.existsSync(codexDir)) { console.log("Codex: skip"); return; }

  const configPath = path.join(codexDir, "config.toml");
  let toml = "";
  try { toml = fs.readFileSync(configPath, "utf8"); } catch {}
  if (!/^\s*hooks\s*=\s*true\s*$/m.test(toml)) {
    const nl = toml.includes("\r\n") ? "\r\n" : "\n";
    fs.writeFileSync(configPath, `${toml.replace(/\s*$/, "")}${toml.trim() ? `${nl}${nl}` : ""}[features]${nl}hooks = true${nl}`);
  }

  const hooksPath = path.join(codexDir, "hooks.json");
  const nodeBin = resolveNodeBin();
  const marker = "state-hook.js codex";
  const events = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop"];
  let settings = {};
  try { settings = readJson(hooksPath); } catch (e) { if (e.code !== "ENOENT") throw e; }
  if (!settings.hooks) settings.hooks = {};

  let added = 0;
  for (const event of events) {
    if (!Array.isArray(settings.hooks[event])) settings.hooks[event] = [];
    const cmd = process.platform === "win32"
      ? `& ${quote(nodeBin)} ${quote(STATE_HOOK)} codex ${quote(event)}`
      : `${quote(nodeBin)} ${quote(STATE_HOOK)} codex ${quote(event)}`;
    let found = false;
    for (const entry of settings.hooks[event]) {
      const hooks = entry.hooks || [];
      for (const h of hooks) {
        if (h.command && h.command.includes(marker)) {
          h.command = cmd;
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found) {
      settings.hooks[event].push({ hooks: [{ type: "command", command: cmd, timeout: 30 }] });
      added++;
    }
  }
  writeJson(hooksPath, settings);
  console.log(`Codex: ~/.codex/hooks.json (+${added})`);
}

function installCopilot() {
  const copilotDir = path.join(os.homedir(), ".copilot");
  if (!fs.existsSync(copilotDir)) { console.log("Copilot: skip"); return; }

  const hooksPath = path.join(copilotDir, "hooks", "hooks.json");
  fs.mkdirSync(path.dirname(hooksPath), { recursive: true });
  const nodeBin = resolveNodeBin();
  const marker = "state-hook.js copilot";
  const events = [
    "sessionStart", "userPromptSubmitted", "preToolUse", "postToolUse", "sessionEnd",
    "errorOccurred", "agentStop", "subagentStart", "subagentStop", "preCompact",
  ];
  let settings = {};
  try { settings = readJson(hooksPath); } catch (e) { if (e.code !== "ENOENT") throw e; }
  if (!settings.hooks) settings.hooks = {};

  let added = 0;
  for (const event of events) {
    if (!Array.isArray(settings.hooks[event])) settings.hooks[event] = [];
    const tail = `${quote(STATE_HOOK)} copilot ${quote(event)}`;
    const cmd = `${quote(nodeBin)} ${tail}`;
    const desired = { type: "command", bash: cmd, powershell: `& ${cmd}`, timeoutSec: 5 };
    let found = false;
    for (const entry of settings.hooks[event]) {
      for (const field of ["bash", "powershell", "command"]) {
        if (typeof entry[field] === "string" && entry[field].includes(marker)) {
          Object.assign(entry, desired);
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found) {
      settings.hooks[event].push(desired);
      added++;
    }
  }
  writeJson(hooksPath, settings);
  console.log(`Copilot: ~/.copilot/hooks/hooks.json (+${added}, state-only)`);
}

function installCodeBuddy() {
  const settingsPath = path.join(os.homedir(), ".codebuddy", "settings.json");
  if (!fs.existsSync(path.dirname(settingsPath))) { console.log("CodeBuddy: skip"); return; }

  const nodeBin = resolveNodeBin();
  const cmd = `"${nodeBin}" "${STATE_HOOK}" codebuddy`;
  const marker = "state-hook.js codebuddy";
  const events = [
    "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "Stop", "Notification", "PreCompact",
  ];
  let settings = {};
  try { settings = readJson(settingsPath); } catch (e) { if (e.code !== "ENOENT") throw e; }

  const stats = { added: 0, updated: 0, skipped: 0 };
  for (const event of events) {
    countResult(mergeClaudeStyleHook(settings, event, cmd, marker), stats);
  }

  const permUrl = buildPermissionUrl(readRuntimePort() || DEFAULT_SERVER_PORT);
  if (!Array.isArray(settings.hooks.PermissionRequest)) settings.hooks.PermissionRequest = [];
  let hasPerm = false;
  for (const entry of settings.hooks.PermissionRequest) {
    for (const h of (entry.hooks || [])) {
      if (h.type === "http" && String(h.url).includes("/permission")) {
        h.url = permUrl;
        hasPerm = true;
      }
    }
  }
  if (!hasPerm) {
    settings.hooks.PermissionRequest.push({
      matcher: "",
      hooks: [{ type: "http", url: permUrl, timeout: 600 }],
    });
    stats.added++;
  }
  writeJson(settingsPath, settings);
  console.log(`CodeBuddy: ~/.codebuddy/settings.json (+${stats.added} ~${stats.updated})`);
}

function installQoder() {
  const settingsPath = path.join(os.homedir(), ".qoder", "settings.json");
  if (!fs.existsSync(path.dirname(settingsPath))) { console.log("Qoder: skip"); return; }

  const nodeBin = resolveNodeBin();
  const marker = "state-hook.js qoder";
  const events = [
    "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure",
    "Stop", "Notification", "PermissionRequest", "PermissionDenied", "SessionEnd",
  ];
  let settings = {};
  try { settings = readJson(settingsPath); } catch (e) { if (e.code !== "ENOENT") throw e; }
  if (!settings.hooks) settings.hooks = {};

  let added = 0;
  for (const event of events) {
    if (!Array.isArray(settings.hooks[event])) settings.hooks[event] = [];
    const cmd = process.platform === "win32"
      ? `powershell -NoProfile -EncodedCommand ${Buffer.from(`"${nodeBin}" "${STATE_HOOK}" qoder ${event}`, "utf16le").toString("base64")}`
      : `"${nodeBin}" "${STATE_HOOK}" qoder ${event}`;
    let found = false;
    for (const entry of settings.hooks[event]) {
      const hooks = entry.hooks || [];
      for (const h of hooks) {
        if (h.command && h.command.includes(marker)) {
          h.command = cmd;
          found = true;
        }
      }
      if (typeof entry.command === "string" && entry.command.includes(marker)) {
        entry.hooks = [{ name: "pixel-bridge", type: "command", command: cmd }];
        delete entry.command;
        found = true;
      }
      if (found) break;
    }
    if (!found) {
      settings.hooks[event].push({
        matcher: "*",
        hooks: [{ name: "pixel-bridge", type: "command", command: cmd }],
      });
      added++;
    }
  }
  writeJson(settingsPath, settings);
  console.log(`Qoder: ~/.qoder/settings.json (+${added}, state-only)`);
}

console.log("Installing pixel-agent-bridge hooks...\n");
installClaude();
installCursor();
installCodex();
installCopilot();
installCodeBuddy();
installQoder();
console.log("\nDone. npm start -- --port=/dev/cu.wchusbserialXXXX");
