const { postStateToRunningServer } = require("./server-config");

function readStdinJson(timeoutMs = 400) {
  return new Promise((resolve) => {
    const chunks = [];
    let done = false;
    const finish = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      process.stdin.removeListener("data", onData);
      process.stdin.removeListener("end", finish);
      try {
        const raw = Buffer.concat(chunks).toString().trim();
        resolve(raw ? JSON.parse(raw) : {});
      } catch {
        resolve({});
      }
    };
    const onData = (c) => chunks.push(c);
    const timer = setTimeout(finish, timeoutMs);
    process.stdin.on("data", onData);
    process.stdin.on("end", finish);
  });
}

function postState(body, done) {
  postStateToRunningServer(JSON.stringify(body), { timeoutMs: 120 }, done || (() => process.exit(0)));
}

function writeStdoutAndExit(line) {
  process.stdout.write(`${line}\n`);
  process.exit(0);
}

module.exports = { readStdinJson, postState, writeStdoutAndExit };
