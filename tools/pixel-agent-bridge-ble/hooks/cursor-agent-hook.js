#!/usr/bin/env node
/** Cursor Agent hook -> pixel-agent-bridge /state */
const { readStdinJson, postState } = require("../lib/hook-utils");

const HOOK_TO_STATE = {
  sessionStart: { state: "idle", event: "SessionStart" },
  sessionEnd: { state: "sleeping", event: "SessionEnd" },
  beforeSubmitPrompt: { state: "thinking", event: "UserPromptSubmit" },
  preToolUse: { state: "working", event: "PreToolUse" },
  postToolUse: { state: "working", event: "PostToolUse" },
  postToolUseFailure: { state: "error", event: "PostToolUseFailure" },
  subagentStart: { state: "juggling", event: "SubagentStart" },
  subagentStop: { state: "working", event: "SubagentStop" },
  preCompact: { state: "sweeping", event: "PreCompact" },
  afterAgentThought: { state: "thinking", event: "AfterAgentThought" },
};

function stdoutForHook(hookName) {
  return hookName === "beforeSubmitPrompt" ? JSON.stringify({ continue: true }) : "{}";
}

readStdinJson().then((payload) => {
  const hookName = process.argv[2] || (payload && payload.hook_event_name) || "";
  let mapped = HOOK_TO_STATE[hookName] || null;
  if (hookName === "stop") {
    mapped = (payload && payload.status === "error")
      ? { state: "error", event: "StopFailure" }
      : { state: "attention", event: "Stop" };
  }

  const outLine = stdoutForHook(hookName);
  if (!mapped) {
    process.stdout.write(`${outLine}\n`);
    process.exit(0);
    return;
  }

  const sessionId = (payload && (payload.conversation_id || payload.session_id)) || "default";
  const body = {
    state: mapped.state,
    session_id: sessionId,
    event: mapped.event,
    agent_id: "cursor-agent",
  };
  let cwd = (payload && payload.cwd) || "";
  if (!cwd && payload && Array.isArray(payload.workspace_roots) && payload.workspace_roots[0]) {
    cwd = payload.workspace_roots[0];
  }
  if (cwd) body.cwd = cwd;

  postState(body, () => {
    process.stdout.write(`${outLine}\n`);
    process.exit(0);
  });
});
