/**
 * Cross-platform WCH / CH340 serial port helpers (macOS cu.wchusbserial*, Windows COM*).
 */

/** WCH USB vendor id (hex string in pnpId) */
const WCH_VID = "1A86";

/**
 * @param {import('@serialport/bindings-interface').PortInfo} port
 * @returns {boolean}
 */
function isWchPort(port) {
  const blob = [
    port.path,
    port.manufacturer,
    port.serialNumber,
    port.pnpId,
    port.friendlyName,
    port.productId,
    port.vendorId,
  ]
    .filter(Boolean)
    .join(" ")
    .toLowerCase();
  return (
    /wch|ch340|ch342|ch343|ch347|usb-serial/i.test(blob) ||
    blob.includes(WCH_VID.toLowerCase())
  );
}

/**
 * @param {string} path
 * @returns {number}
 */
function portSortKey(path) {
  const com = /^COM(\d+)$/i.exec(path);
  if (com) return parseInt(com[1], 10);
  const wch = /(\d{3,4})$/i.exec(path);
  if (wch) return parseInt(wch[1], 10);
  return 99999;
}

/**
 * Pick UART0 (agent bridge) from enumerated ports — lowest WCH port number.
 * @param {import('@serialport/bindings-interface').PortInfo[]} ports
 * @returns {import('@serialport/bindings-interface').PortInfo | null}
 */
function pickAgentUart0(ports) {
  const wch = ports.filter(isWchPort).sort((a, b) => portSortKey(a.path) - portSortKey(b.path));
  return wch[0] || null;
}

/**
 * @param {import('@serialport/bindings-interface').PortInfo[]} ports
 * @returns {import('@serialport/bindings-interface').PortInfo[]}
 */
function listWchPorts(ports) {
  return ports.filter(isWchPort).sort((a, b) => portSortKey(a.path) - portSortKey(b.path));
}

module.exports = { isWchPort, pickAgentUart0, listWchPorts, portSortKey };
