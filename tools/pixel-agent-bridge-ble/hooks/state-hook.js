#!/usr/bin/env node
/**
 * Shared state hook for Codex / Copilot / CodeBuddy / Qoder -> pixel-agent-bridge /state
 * Usage: node state-hook.js <profile> [event]
 */
const { readStdinJson, postState, writeStdoutAndExit } = require("../lib/hook-utils");

const PROFILES = {
  codex: {
    agentId: "codex",
    noStateStdout: "{}",
    skipEvents: new Set(["PermissionRequest"]),
    map: {
      SessionStart: ["idle", "SessionStart"],
      UserPromptSubmit: ["thinking", "UserPromptSubmit"],
      PreToolUse: ["working", "PreToolUse"],
      PostToolUse: ["working", "PostToolUse"],
      Stop: ["attention", "Stop"],
    },
    sessionId: (p) => {
      const raw = (p && p.session_id) || "default";
      return String(raw).startsWith("codex:") ? raw : `codex:${raw}`;
    },
  },
  copilot: {
    agentId: "copilot-cli",
    skipEvents: new Set(["permissionRequest"]),
    map: {
      sessionStart: ["idle", "sessionStart"],
      sessionEnd: ["sleeping", "sessionEnd"],
      userPromptSubmitted: ["thinking", "userPromptSubmitted"],
      preToolUse: ["working", "preToolUse"],
      postToolUse: ["working", "postToolUse"],
      errorOccurred: ["error", "errorOccurred"],
      agentStop: ["attention", "agentStop"],
      subagentStart: ["juggling", "subagentStart"],
      subagentStop: ["working", "subagentStop"],
      preCompact: ["sweeping", "preCompact"],
    },
    sessionId: (p) => (p && (p.sessionId || p.session_id)) || "default",
  },
  codebuddy: {
    agentId: "codebuddy",
    eventFromPayload: true,
    stdout: (event) => (event === "PreToolUse" ? JSON.stringify({ decision: "allow" }) : "{}"),
    map: {
      SessionStart: ["idle", "SessionStart"],
      SessionEnd: ["sleeping", "SessionEnd"],
      UserPromptSubmit: ["thinking", "UserPromptSubmit"],
      PreToolUse: ["working", "PreToolUse"],
      PostToolUse: ["working", "PostToolUse"],
      Stop: ["attention", "Stop"],
      Notification: ["notification", "Notification"],
      PreCompact: ["sweeping", "PreCompact"],
    },
    sessionId: (p) => (p && p.session_id) || "default",
  },
  qoder: {
    agentId: "qoder",
    stdout: () => "{}",
    map: {
      SessionStart: ["idle", "SessionStart"],
      UserPromptSubmit: ["thinking", "UserPromptSubmit"],
      PreToolUse: ["working", "PreToolUse"],
      PostToolUse: ["working", "PostToolUse"],
      PostToolUseFailure: ["error", "PostToolUseFailure"],
      Stop: ["attention", "Stop"],
      Notification: ["notification", "Notification"],
      PermissionRequest: ["notification", "Notification"],
      PermissionDenied: ["notification", "Notification"],
      SessionEnd: ["sleeping", "SessionEnd"],
    },
    sessionId: (p) => {
      const raw = (p && p.session_id) || "default";
      return String(raw).startsWith("qoder:") ? raw : `qoder:${raw}`;
    },
  },
};

const profileName = process.argv[2];
const profile = PROFILES[profileName];
if (!profile) process.exit(0);

let codebuddyDone = false;
let codebuddyTimer = null;
if (profileName === "codebuddy") {
  codebuddyTimer = setTimeout(() => {
    if (!codebuddyDone) writeStdoutAndExit("{}");
  }, 800);
  codebuddyTimer.unref();
}

function finishWithStdout(outLine, cb) {
  codebuddyDone = true;
  if (codebuddyTimer) clearTimeout(codebuddyTimer);
  process.stdout.write(`${outLine}\n`);
  if (cb) cb();
  else process.exit(0);
}

function resolveEvent(payload) {
  if (profile.eventFromPayload) {
    return (payload && payload.hook_event_name) || "";
  }
  return process.argv[3] || (payload && payload.hook_event_name) || "";
}

readStdinJson().then((payload) => {
  const event = resolveEvent(payload);
  const outLine = profile.stdout ? profile.stdout(event) : (profile.noStateStdout || "{}");

  if (profile.skipEvents && profile.skipEvents.has(event)) {
    finishWithStdout(outLine);
    return;
  }

  const mapped = profile.map[event];
  if (!mapped) {
    finishWithStdout(outLine);
    return;
  }

  const body = {
    state: mapped[0],
    session_id: profile.sessionId(payload),
    event: mapped[1],
    agent_id: profile.agentId,
  };
  if (payload && typeof payload.cwd === "string" && payload.cwd) {
    body.cwd = payload.cwd;
  }

  postState(body, () => finishWithStdout(outLine));
}).catch(() => writeStdoutAndExit("{}"));
