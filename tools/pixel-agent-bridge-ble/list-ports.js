#!/usr/bin/env node
/**
 * List serial ports — helps pick --port= on Windows (COM3) or macOS (cu.wchusbserial*).
 */
const { SerialPort } = require("serialport");
const { listWchPorts } = require("./lib/serial-ports");

async function main() {
  const all = await SerialPort.list();
  const wch = listWchPorts(all);

  console.log("=== All serial ports ===");
  if (all.length === 0) {
    console.log("(none — plug in the pixel board USB)");
  } else {
    for (const p of all) {
      const tag = wch.some((w) => w.path === p.path) ? " [WCH]" : "";
      console.log(`  ${p.path}${tag}  ${p.friendlyName || p.manufacturer || ""}`);
    }
  }

  console.log("");
  console.log("=== WCH ports (use the FIRST for pixel-agent-bridge / UART0) ===");
  if (wch.length === 0) {
    console.log("(none — install WCH/CH340 driver: https://www.wch.cn/downloads/CH341SER_EXE.html)");
    process.exit(1);
  }
  wch.forEach((p, i) => {
    const role = i === 0 ? " <- bridge / UART0 (recommended)" : " <- UART1 log only";
    console.log(`  ${p.path}${role}`);
  });

  const first = wch[0].path;
  console.log("");
  console.log("Start bridge:");
  if (/^COM\d+$/i.test(first)) {
    console.log(`  npm start -- --port=${first}`);
    console.log(`  set PIXEL_SERIAL_PORT=${first} && npm start`);
  } else {
    console.log(`  npm start -- --port=${first}`);
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
