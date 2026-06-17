#!/usr/bin/env node
/** Claude Code command hook -> pixel-agent-bridge /state */
const { readStdinJson, postState } = require("../lib/hook-utils");

const EVENT_TO_STATE = {
  SessionStart: "idle",
  SessionEnd: "sleeping",
  UserPromptSubmit: "thinking",
  PreToolUse: "working",
  PostToolUse: "working",
  PostToolUseFailure: "error",
  Stop: "attention",
  StopFailure: "error",
  SubagentStart: "juggling",
  SubagentStop: "working",
  PreCompact: "sweeping",
  PostCompact: "attention",
  Notification: "notification",
  Elicitation: "notification",
  WorktreeCreate: "carrying",
};

const event = process.argv[2];
const state = EVENT_TO_STATE[event];
if (!state) process.exit(0);

readStdinJson().then((payload) => {
  const source = payload.source || payload.reason || "";
  const resolvedState = (event === "SessionEnd" && source === "clear") ? "sweeping" : state;
  const body = {
    state: resolvedState,
    session_id: payload.session_id || "default",
    event,
    agent_id: "claude-code",
  };
  if (payload.cwd) body.cwd = payload.cwd;
  postState(body);
});
