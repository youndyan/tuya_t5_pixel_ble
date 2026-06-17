const { execSync } = require("child_process");

const ACTIONS = {
  darwin: {
    clear: [
      'osascript -e \'tell application "System Events" to keystroke "a" using command down\'',
      'osascript -e \'tell application "System Events" to key code 51\'',
    ],
    backspace: ['osascript -e \'tell application "System Events" to key code 51\''],
    enter: ['osascript -e \'tell application "System Events" to key code 36\''],
  },
  linux: {
    clear: ["xdotool key ctrl+a BackSpace"],
    backspace: ["xdotool key BackSpace"],
    enter: ["xdotool key Return"],
  },
  win32: {
    clear: ['powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait(\'^a{BACKSPACE}\')"'],
    backspace: ['powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait(\'{BACKSPACE}\')"'],
    enter: ['powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait(\'{ENTER}\')"'],
  },
};

function runAction(action) {
  const platform = process.platform;
  const cmds = ACTIONS[platform] && ACTIONS[platform][action];
  if (!cmds) return;
  for (const cmd of cmds) {
    execSync(cmd, { timeout: 3000 });
  }
}

function pasteText(text) {
  const trimmed = String(text || "").trim();
  if (!trimmed) return;
  const platform = process.platform;
  try {
    if (platform === "darwin") {
      const b64 = Buffer.from(trimmed, "utf8").toString("base64");
      execSync(`printf '%s' '${b64}' | base64 -D | pbcopy`, { timeout: 3000 });
      execSync('osascript -e \'tell application "System Events" to keystroke "v" using command down\'', { timeout: 3000 });
    } else if (platform === "linux") {
      execSync(`xdotool type --clearmodifiers "${trimmed.replace(/"/g, '\\"')}"`, { timeout: 5000 });
    } else if (platform === "win32") {
      const ps = `Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Clipboard]::SetText('${trimmed.replace(/'/g, "''")}'); [System.Windows.Forms.SendKeys]::SendWait('^v')`;
      execSync(`powershell -NoProfile -Command "${ps}"`, { timeout: 5000 });
    }
  } catch {
    /* keyboard simulation is best-effort */
  }
}

module.exports = { runAction, pasteText };
