const { execFileSync } = require("child_process");

const CANDIDATES = [
  process.env.PIXEL_PYTHON,
  process.platform === "win32" ? "python" : null,
  process.platform === "win32" ? "py" : null,
  "/usr/bin/python3",
  "/opt/homebrew/bin/python3",
  "/usr/local/bin/python3",
  "python3",
  "python",
].filter(Boolean);

/**
 * Find a Python interpreter that can import bleak.
 * @returns {string|null}
 */
function resolveBlePython() {
  for (const py of CANDIDATES) {
    try {
      execFileSync(py, ["-c", "import bleak"], { stdio: "ignore" });
      return py;
    } catch {
      /* try next */
    }
  }
  return null;
}

module.exports = { resolveBlePython, CANDIDATES };
