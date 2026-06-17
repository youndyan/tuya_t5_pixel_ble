#!/usr/bin/env python3
"""
ble_bridge.py — BLE transport for pixel-agent-bridge (stdio IPC with bridge.js)

Device types (auto-detected by name + GATT):
  TYBLE / TUYA_*     → demo_ble firmware, plain S:/P: lines (NUS or FD50)
  BuddyPixel / Clawd → buddy firmware, JSON heartbeat on FD50

Interference: many Tuya gadgets expose FD50 or 0x1910. Always bind address.

Usage:
  python3 ble_bridge.py --scan
  python3 ble_bridge.py --address <BLE-UUID-from-scan>
  python3 ble_bridge.py --name TYBLE
"""

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path
from typing import Optional

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    raise SystemExit(
        "Missing bleak. Install: python3 -m pip install bleak"
    )

TUYA_SERVICE = "0000fd50-0000-1000-8000-00805f9b34fb"
TUYA_WRITE = "00000001-0000-1001-8001-00805f9b07d0"
TUYA_NOTIFY = "00000002-0000-1001-8001-00805f9b07d0"

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Only pixel-agent demo boards (exclude generic TUYA_ / TY gadgets)
PIXEL_NAME_TOKENS = ("TYBLE", "BUDDY", "CLAWD", "PIXEL")

BIND_FILE = Path(__file__).resolve().parent / ".pixel-ble-address"
BIND_JSON = Path(__file__).resolve().parent / ".pixel-ble-bind.json"

RECONNECT_MIN_S = float(os.environ.get("PIXEL_BLE_RECONNECT_MIN", "3"))
RECONNECT_MAX_S = float(os.environ.get("PIXEL_BLE_RECONNECT_MAX", "30"))


def load_bind() -> dict:
    """Load bind record: address, chip (4-char), name."""
    env_addr = (os.environ.get("PIXEL_BLE_ADDRESS") or "").strip()
    env_chip = (os.environ.get("PIXEL_BLE_CHIP") or "").strip().lower()
    if env_addr:
        return {"address": env_addr, "chip": env_chip, "name": os.environ.get("PIXEL_BLE_NAME", "")}
    if BIND_JSON.is_file():
        try:
            data = json.loads(BIND_JSON.read_text())
            if isinstance(data, dict) and data.get("address"):
                data["chip"] = (data.get("chip") or "").lower()
                return data
        except (json.JSONDecodeError, OSError):
            pass
    if BIND_FILE.is_file():
        addr = BIND_FILE.read_text().strip()
        if addr:
            return {"address": addr, "chip": "", "name": ""}
    return {}


def load_bind_address() -> Optional[str]:
    rec = load_bind()
    return rec.get("address") or None


def save_bind(address: str, chip: str = "", name: str = "") -> None:
    addr = address.strip()
    if not addr:
        return
    chip = (chip or "").strip().lower()
    name = (name or "").strip()
    rec = {"address": addr, "chip": chip, "name": name}
    BIND_JSON.write_text(json.dumps(rec, indent=2) + "\n")
    BIND_FILE.write_text(addr + "\n")
    log(f"Saved bind: {addr} chip={chip or '?'} name={name or '?'}")


def save_bind_address(address: str) -> None:
    prev = load_bind()
    save_bind(address, prev.get("chip", ""), prev.get("name", ""))


def _print_bind_hint(devices) -> None:
    log("Bind your pixel screen (only one TYBLE powered on is safest):")
    for d in devices:
        log(f"  npm run bind:ble -- {d.address}")
    log("After bind, chip tag (I:xxxx) is verified on every connect.")
    log("macOS UUID is NOT hardware MAC — chip tag is the reliable ID.")


async def query_identity(client: BleakClient, write_uuid: str, notify_uuid: str) -> Optional[str]:
    """Ask device I: and read I:chip notify line."""
    rx_buf = bytearray()
    got = asyncio.Event()
    result: dict = {"chip": None}

    def on_identity(_handle, data: bytearray) -> None:
        rx_buf.extend(data)
        while True:
            idx = rx_buf.find(b"\n")
            if idx < 0:
                break
            line = bytes(rx_buf[:idx]).strip()
            del rx_buf[: idx + 1]
            if line.startswith(b"I:"):
                result["chip"] = line[2:].decode("utf-8", errors="replace").strip().lower()
                got.set()

    await client.start_notify(notify_uuid, on_identity)
    try:
        await write_line(client, write_uuid, b"I:\n")
        try:
            await asyncio.wait_for(got.wait(), timeout=4.0)
        except asyncio.TimeoutError:
            log("  identity timeout (no I: reply — old firmware?)")
            return None
        return result["chip"]
    finally:
        try:
            await client.stop_notify(notify_uuid)
        except Exception:
            pass


def _is_tyble_family(name: str) -> bool:
    u = (name or "").upper().strip()
    return u == "TYBLE" or u.startswith("TYBLE_")


def _ambiguous_top_tier(ranked: list) -> Optional[list]:
    """Return list of devices if auto-pick is unsafe (multiple TYBLE / tied top scores)."""
    if not ranked:
        return None
    top_score = ranked[0][0]
    top_tier = [d for sc, d in ranked if sc == top_score]
    tyble = [d for d in top_tier if _is_tyble_family(d.name or "")]
    if len(tyble) > 1:
        return tyble
    if len(top_tier) > 1 and top_score >= 80:
        return top_tier
    return None


def log(msg: str) -> None:
    print(f"[ble-bridge] {msg}", file=sys.stderr, flush=True)


def is_pixel_name(name: str) -> bool:
    if not name:
        return False
    u = name.upper().strip()
    if u in ("TY", "TYI", "SM", "TUYA_"):
        return False
    if u.startswith("TYBLE"):
        return True
    return any(tok in u for tok in ("BUDDY", "CLAWD", "PIXEL"))


def name_score(label: str, hint: str) -> int:
    u = label.upper()
    h = hint.upper() if hint else ""
    if not is_pixel_name(label):
        return 0
    score = 50
    if u.startswith("TYBLE_"):
        score += 60
    elif u == "TYBLE" or u.startswith("TYBLE"):
        score += 40
    if "BUDDY" in u or "CLAWD" in u:
        score += 40
    if h and h in u:
        score += 100
    return score


def pick_chars(services):
    """Prefer Tuya FD50 (demo_ble); fall back to NUS if present."""
    tuya = nus = None
    for s in services:
        su = str(s.uuid).lower()
        if su == TUYA_SERVICE:
            write = notify = None
            for ch in s.characteristics:
                cu = str(ch.uuid).lower()
                if cu == TUYA_WRITE:
                    write = cu
                elif cu == TUYA_NOTIFY:
                    notify = cu
            if write and notify:
                tuya = (write, notify)
        elif su == NUS_SERVICE:
            write = notify = None
            for ch in s.characteristics:
                cu = str(ch.uuid).lower()
                if cu == NUS_RX:
                    write = cu
                elif cu == NUS_TX:
                    notify = cu
            if write and notify:
                nus = (write, notify)
    if tuya:
        return tuya[0], tuya[1], "tuya"
    if nus:
        return nus[0], nus[1], "nus"
    return None, None, None


def detect_profile(dev_name: str, mode: str) -> str:
    u = (dev_name or "").upper()
    if "BUDDY" in u or "CLAWD" in u:
        return "buddy"
    if mode == "nus" or "TYBLE" in u or "TUYA" in u:
        return "agent"
    return "buddy"


def agent_line_to_payload(line: bytes, profile: str) -> bytes:
    text = line.decode("utf-8", errors="replace").strip()
    if profile != "buddy":
        return line if line.endswith(b"\n") else line + b"\n"

    if text.startswith("S:"):
        state = text[2:]
        if state == "idle":
            obj = {"type": "heartbeat", "active_sessions": 0, "total_tokens": 0, "pending_permissions": []}
        elif state in ("thinking", "working", "juggling", "error"):
            obj = {"type": "heartbeat", "active_sessions": 1, "total_tokens": 0, "pending_permissions": []}
        elif state == "notify":
            obj = {
                "type": "heartbeat",
                "active_sessions": 0,
                "total_tokens": 0,
                "pending_permissions": [{"id": "notify", "prompt": "notify"}],
            }
        elif state == "happy":
            obj = {"type": "turn", "role": "assistant", "content": "", "output_tokens": 1}
        else:
            obj = {"type": "heartbeat", "active_sessions": 0, "total_tokens": 0, "pending_permissions": []}
        return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")

    if text.startswith("P:"):
        rest = text[2:]
        if not rest:
            obj = {"type": "heartbeat", "active_sessions": 0, "total_tokens": 0, "pending_permissions": []}
        elif rest.startswith("tool="):
            tool = rest[5:]
            obj = {
                "type": "heartbeat",
                "active_sessions": 0,
                "total_tokens": 0,
                "pending_permissions": [{"id": "perm", "prompt": tool}],
            }
        else:
            obj = {"type": "heartbeat", "active_sessions": 0, "total_tokens": 0, "pending_permissions": []}
        return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")

    return line if line.endswith(b"\n") else line + b"\n"


def log_services(client) -> None:
    for s in client.services:
        chars = ", ".join(str(c.uuid).lower() for c in s.characteristics)
        log(f"  svc {str(s.uuid).lower()} -> [{chars}]")


async def write_line(client, write_uuid: str, payload: bytes) -> None:
    # Prefer write-without-response on FD50; with-response can stall right after reconnect
    for response in (False, True):
        try:
            await client.write_gatt_char(write_uuid, payload, response=response)
            return
        except Exception as e:
            if not response:
                log(f"  write no-response failed ({e}), retry with response")
            else:
                raise


async def probe_device(d, check_notify: bool = False, require_chip: str = ""):
    log(f"Probing {d.name} [{d.address}] ...")
    try:
        async with BleakClient(d.address, timeout=20.0) as client:
            write_uuid, notify_uuid, mode = pick_chars(client.services)
            if not write_uuid:
                log(f"  skip: no NUS/FD50")
                return None
            chip = await query_identity(client, write_uuid, notify_uuid)
            if chip:
                log(f"  identity chip={chip}")
            if require_chip:
                need = require_chip.lower()
                if not chip or chip != need:
                    log(f"  skip: chip {chip or '?'} != required {need}")
                    try:
                        profile = detect_profile(d.name or "", mode)
                        await write_line(
                            client, write_uuid, agent_line_to_payload(b"L:0", profile)
                        )
                    except Exception:
                        pass
                    return None
            if check_notify and mode == "tuya":
                profile = detect_profile(d.name or "", mode)
                if profile == "buddy":
                    seen = asyncio.Event()

                    def _h(_handle, data: bytearray) -> None:
                        if b'"type":"status"' in bytes(data) or b'"type": "status"' in bytes(data):
                            seen.set()

                    await client.start_notify(notify_uuid, _h)
                    await write_line(
                        client, write_uuid, b'{"type":"status_request"}\n'
                    )
                    try:
                        await asyncio.wait_for(seen.wait(), timeout=2.5)
                        log("  verify OK: status_request answered")
                    except asyncio.TimeoutError:
                        log("  WARN: no status reply (pairing? wrong firmware?)")
            log(f"OK: {d.name} has {mode}")
            return d.address, d.name, write_uuid, notify_uuid, mode, chip or ""
    except Exception as e:
        log(f"  skip: {e}")
    return None


async def scan_only(timeout: float) -> int:
    log(f"BLE scan ({timeout}s) — pixel-like devices only:")
    devices = await BleakScanner.discover(timeout=timeout)
    pixel = [d for d in devices if d.name and is_pixel_name(d.name)]
    other_tuya = [d for d in devices if d.name and d.name.upper() in ("TY", "TYI")]
    if not pixel:
        log("  (no TYBLE/BuddyPixel/Clawd found)")
    for d in sorted(pixel, key=lambda x: x.name or ""):
        rssi = getattr(d, "rssi", None)
        meta = getattr(d, "metadata", None) or {}
        local = meta.get("local_name") if isinstance(meta, dict) else None
        label = local or d.name or "?"
        log(f"  * {label:16s}  {d.address}  rssi={rssi}")
    if other_tuya:
        log("Nearby generic Tuya names (NOT auto-connected):")
        for d in other_tuya:
            log(f"  - {d.name:16s}  {d.address}")
    log(f"Total BLE advertisements: {len(devices)} (2.4GHz congestion possible)")
    bind = load_bind()
    if bind.get("address"):
        log(f"Saved bind: {bind['address']} chip={bind.get('chip') or '?'} name={bind.get('name') or '?'}")
    else:
        log("No bind yet — power ON only target board, then:")
        log("  npm run bind:ble -- <address-from-scan>")
    log("Tip: new firmware shows TYBLE_59d8 style names; use chip tag to avoid wrong board.")
    return 0


async def find_pixel_by_chip(chip: str, name_hint: str, timeout: float):
    """Scan and connect-probe until device returns matching I:chip."""
    chip = chip.lower().strip()
    if not chip:
        return None
    log(f"Scanning for chip={chip!r} (ignores stale macOS UUID)...")
    devices = await BleakScanner.discover(timeout=timeout)
    ranked = []
    for d in devices:
        if not d.name:
            continue
        sc = name_score(d.name, name_hint)
        if sc > 0:
            ranked.append((sc, d))
    if not ranked:
        ranked = [(50, d) for d in devices if d.name and is_pixel_name(d.name)]
    ranked.sort(key=lambda x: -x[0])
    for _, d in ranked:
        r = await probe_device(d, require_chip=chip)
        if r:
            return r
    log(f"No device answered with chip={chip}")
    return None


async def find_pixel_device(
    name_hint: str,
    timeout: float,
    address: Optional[str],
    chip_hint: str = "",
    allow_rescan: bool = False,
    auto_bind: bool = False,
):
    bound = (address or "").strip() or None
    chip_hint = (chip_hint or "").strip().lower()
    if bound:
        log(f"Using bound address: {bound}" + (f" chip={chip_hint}" if chip_hint else ""))
        if chip_hint:
            # Bound address + chip: one connect in _ble_session (avoids probe disconnect flash)
            log("defer identity check to session (single connect)")
            return bound, name_hint or "TYBLE", "", "", "tuya", chip_hint
        class _D:
            pass
        d = _D()
        d.address = bound
        d.name = name_hint or "TYBLE"
        r = await probe_device(d, check_notify=True, require_chip="")
        if r:
            return r
        log(f"Bound address unreachable: {bound}")
        if chip_hint:
            r = await find_pixel_by_chip(chip_hint, name_hint, timeout)
            if r:
                addr, name, w, n, mode, chip = r
                save_bind(addr, chip, name or "")
                log(f"Re-bound by chip: {addr} ({name})")
                return r
        if not allow_rescan:
            log("NOT auto-scanning without chip tag. Options:")
            log("  - npm run bind:ble -- <address>  (re-probe identity)")
            log("  - power ON only one pixel board, then bind again")
            return None
        log("allow-rescan: falling back to BLE scan")

    bind = load_bind()
    if bind.get("address") and not bound:
        return await find_pixel_device(
            name_hint,
            timeout,
            bind["address"],
            bind.get("chip", ""),
            allow_rescan=False,
            auto_bind=False,
        )

    log(f"Scanning ({timeout}s), hint={name_hint!r}...")
    devices = await BleakScanner.discover(timeout=timeout)

    ranked = []
    for d in devices:
        if not d.name:
            continue
        sc = name_score(d.name, name_hint)
        if sc > 0:
            ranked.append((sc, d))
    ranked.sort(key=lambda x: -x[0])

    if ranked:
        log("Pixel candidates (best first):")
        for sc, d in ranked[:8]:
            rssi = getattr(d, "rssi", None)
            log(f"  [{sc:3d}] {d.name} [{d.address}] rssi={rssi}")
    else:
        log(f"No name match for hint={name_hint!r}")
        pixel = [d for d in devices if d.name and is_pixel_name(d.name)]
        if not pixel:
            log("No pixel-like BLE names — power on screen, stay within 1 m")
            return None
        ranked = [(50, d) for d in pixel]
        log("Fallback: only pixel-like names (ignoring TY/TYI/headphones)")

    ambiguous = _ambiguous_top_tier(ranked)
    if ambiguous:
        log("ERROR: multiple pixel-like devices — will NOT auto-connect")
        _print_bind_hint(ambiguous)
        return None

    fd50_count = 0
    for _, d in ranked:
        r = await probe_device(d, check_notify=(fd50_count == 0))
        if r:
            addr, name, w, n, mode, chip = r
            if auto_bind or len(ranked) == 1:
                save_bind(addr, chip, name or "")
            else:
                log(f"Probe OK: {name} [{addr}] chip={chip or '?'} (use bind:ble to save)")
            return r
        fd50_count += 1

    log("All candidates failed. Run: python3 ble_bridge.py --scan")
    return None


async def _stdin_reader(queue: "asyncio.Queue[bytes]") -> None:
    """Read lines from bridge.js stdin for the lifetime of the process."""
    loop = asyncio.get_event_loop()
    reader = asyncio.StreamReader()
    protocol = asyncio.StreamReaderProtocol(reader)
    await loop.connect_read_pipe(lambda: protocol, sys.stdin)
    while True:
        line = await reader.readline()
        if not line:
            break
        await queue.put(line)


async def _send_payload(
    client: BleakClient, write_uuid: str, payload: bytes
) -> None:
    for i in range(0, len(payload), 200):
        await write_line(client, write_uuid, payload[i : i + 200])


def _is_control_line(line: bytes) -> bool:
    """Link/identity lines must not be replayed on reconnect."""
    t = line.strip()
    return t.startswith(b"L:") or t.startswith(b"I:")


async def _reject_chip_mismatch(
    client: BleakClient,
    write_uuid: str,
    profile: str,
    chip: Optional[str],
    expected_chip: str,
) -> None:
    """Send L:0 and disconnect when I: chip does not match bind file."""
    log(f"wrong board: chip={chip or '?'} expected={expected_chip}")
    try:
        await _send_payload(
            client, write_uuid, agent_line_to_payload(b"L:0", profile)
        )
    except Exception:
        pass
    await client.disconnect()
    raise RuntimeError(f"wrong board: chip={chip} expected={expected_chip}")


async def _ble_session(
    address: str,
    dev_name: str,
    profile: str,
    line_queue: "asyncio.Queue[bytes]",
    last_line: bytes,
    expected_chip: str = "",
) -> tuple:
    """One connected session; returns (last_line, write_uuid, notify_uuid, mode)."""
    rx_buf = bytearray()
    disconnected = asyncio.Event()
    identity_chip: dict = {"value": None}
    identity_done = asyncio.Event()

    def on_disconnect(_client: BleakClient) -> None:
        log("device disconnected (reboot or out of range)")
        log("LINK: down")
        disconnected.set()

    def on_notify(_handle, data: bytearray) -> None:
        rx_buf.extend(data)
        while True:
            idx = rx_buf.find(b"\n")
            if idx < 0:
                break
            line = bytes(rx_buf[:idx])
            del rx_buf[: idx + 1]
            if not line.strip():
                continue
            if not identity_done.is_set() and line.strip().startswith(b"I:"):
                identity_chip["value"] = (
                    line.strip()[2:].decode("utf-8", errors="replace").strip().lower()
                )
                identity_done.set()
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if text.startswith("{"):
                try:
                    obj = json.loads(text)
                    ev = obj.get("event")
                    if obj.get("type") == "event" and ev == "approve":
                        sys.stdout.buffer.write(b"B:allow\n")
                    elif obj.get("type") == "event" and ev == "deny":
                        sys.stdout.buffer.write(b"B:deny\n")
                    else:
                        continue
                except json.JSONDecodeError:
                    continue
            else:
                sys.stdout.buffer.write(line + b"\n")
            sys.stdout.buffer.flush()
            log(f"<- device: {text[:80]}")

    client = BleakClient(
        address, timeout=30.0, disconnected_callback=on_disconnect
    )
    await client.connect()
    log(f"Connected: {dev_name} [{address}]")
    log("GATT:")
    log_services(client)

    write_uuid, notify_uuid, mode = pick_chars(client.services)
    if not write_uuid or not notify_uuid:
        raise RuntimeError("no FD50/NUS characteristics on device")
    profile = detect_profile(dev_name, mode)
    log(f"GATT pick: transport={mode} notify={notify_uuid}")

    await client.start_notify(notify_uuid, on_notify)
    log(f"Notify on {notify_uuid}")
    await write_line(client, write_uuid, b"I:\n")
    need = (expected_chip or "").strip().lower()
    if need:
        try:
            await asyncio.wait_for(identity_done.wait(), timeout=4.0)
        except asyncio.TimeoutError:
            log("identity timeout (no I: reply — old firmware?)")
            await client.disconnect()
            raise RuntimeError("identity timeout")
        chip = identity_chip["value"]
        if chip:
            log(f"Device chip={chip}")
        if chip != need:
            await _reject_chip_mismatch(client, write_uuid, profile, chip, need)
    else:
        try:
            await asyncio.wait_for(identity_done.wait(), timeout=4.0)
        except asyncio.TimeoutError:
            log("identity timeout (no I: reply)")
        else:
            chip = identity_chip["value"]
            if chip:
                log(f"Device chip={chip}")

    log("LINK: up")
    link_payload = agent_line_to_payload(b"L:1", profile)
    await _send_payload(client, write_uuid, link_payload)
    log("-> device (link): L:1")
    await asyncio.sleep(0.5)

    sync_line = last_line if last_line.strip() else b"S:idle\n"
    if (
        not _is_control_line(sync_line)
        and sync_line.strip() not in (b"S:idle", b"")
    ):
        sync_payload = agent_line_to_payload(sync_line, profile)
        await _send_payload(client, write_uuid, sync_payload)
        log(f"-> device (sync): {sync_payload.decode('utf-8', errors='replace').strip()}")
    else:
        log("skip idle sync on connect (no auto agent mode)")

    while not line_queue.empty():
        try:
            pending = line_queue.get_nowait()
        except asyncio.QueueEmpty:
            break
        if not pending:
            continue
        norm = pending if pending.endswith(b"\n") else pending + b"\n"
        if _is_control_line(norm) or norm == last_line:
            continue
        try:
            payload = agent_line_to_payload(pending, profile)
            await _send_payload(client, write_uuid, payload)
            if norm.startswith(b"S:"):
                last_line = norm
            log(f"-> device (flush): {payload.decode('utf-8', errors='replace').strip()}")
        except Exception as e:
            log(f"flush write failed ({e})")
            disconnected.set()
            break

    while not disconnected.is_set():
        try:
            line = await asyncio.wait_for(line_queue.get(), timeout=0.5)
        except asyncio.TimeoutError:
            if not client.is_connected:
                disconnected.set()
            continue
        if not line:
            continue
        norm = line if line.endswith(b"\n") else line + b"\n"
        if _is_control_line(norm):
            continue
        try:
            payload = agent_line_to_payload(line, profile)
            await _send_payload(client, write_uuid, payload)
            if norm.startswith(b"S:"):
                last_line = norm
            log(f"-> device: {payload.decode('utf-8', errors='replace').strip()}")
        except Exception as e:
            log(f"write failed ({e})")
            disconnected.set()

    try:
        if client.is_connected:
            await _send_payload(
                client, write_uuid, agent_line_to_payload(b"L:0", profile)
            )
    except Exception:
        pass
    try:
        await client.disconnect()
    except Exception:
        pass
    return last_line, write_uuid, notify_uuid, mode


async def run(
    name_hint: str,
    scan_timeout: float,
    address: Optional[str],
    chip_hint: str = "",
    allow_rescan: bool = False,
    auto_bind: bool = False,
) -> int:
    found = await find_pixel_device(
        name_hint, scan_timeout, address, chip_hint,
        allow_rescan=allow_rescan, auto_bind=auto_bind,
    )
    if not found:
        return 1

    bound_address, dev_name, write_uuid, notify_uuid, mode, bound_chip = found
    if bound_chip:
        save_bind(bound_address, bound_chip, dev_name or "")
    log(f"Locked to address: {bound_address} ({dev_name}) chip={bound_chip or '?'}")
    profile = detect_profile(dev_name, mode)
    if mode == "nus":
        log(f"Profile: {profile} | name={dev_name!r} | transport=nus")
    else:
        log(f"Profile: {profile} | name={dev_name!r} | transport={mode} (FD50)")
    log("auto-reconnect enabled (device reboot / link loss)")
    if profile == "buddy":
        log("Buddy firmware → JSON heartbeat on FD50")
        log("If no screen change: System Settings → Bluetooth → forget device, re-pair (passkey on screen)")
    else:
        log("demo_ble firmware → S:/P: lines (expect serial log: agent ble: RX)")

    line_queue: asyncio.Queue = asyncio.Queue()
    stdin_task = asyncio.create_task(_stdin_reader(line_queue))

    last_line = b"S:idle\n"
    retry_s = RECONNECT_MIN_S
    fail_streak = 0

    while True:
        connect_address = bound_address
        if fail_streak >= 3:
            log(f"several failures — retrying bound address only: {bound_address}")
            fail_streak = 0

        try:
            last_line, write_uuid, notify_uuid, mode = await _ble_session(
                connect_address,
                dev_name,
                profile,
                line_queue,
                last_line,
                bound_chip,
            )
            profile = detect_profile(dev_name, mode)
            retry_s = RECONNECT_MIN_S
            fail_streak = 0
        except KeyboardInterrupt:
            raise
        except Exception as e:
            fail_streak += 1
            log(f"connect/session failed: {e}")
            log("LINK: down")
            if bound_chip and fail_streak >= 1:
                alt = await find_pixel_by_chip(bound_chip, name_hint, min(scan_timeout, 15.0))
                if alt:
                    bound_address, dev_name, _, _, _, verified = alt
                    if verified == bound_chip:
                        save_bind(bound_address, bound_chip, dev_name or "")
                        log(f"Switched to address {bound_address} for chip={bound_chip}")
                        fail_streak = 0

        log(f"reconnecting in {retry_s:.0f}s...")
        await asyncio.sleep(retry_s)
        retry_s = min(retry_s * 1.5, RECONNECT_MAX_S)


def main() -> None:
    parser = argparse.ArgumentParser(description="Pixel agent BLE stdio bridge")
    parser.add_argument("--scan", action="store_true", help="List pixel-like BLE devices and exit")
    parser.add_argument(
        "--set-address",
        metavar="UUID",
        help="Save CoreBluetooth UUID to .pixel-ble-address and exit",
    )
    parser.add_argument(
        "--name",
        default=os.environ.get("PIXEL_BLE_NAME", "TYBLE"),
        help="Name hint (default: TYBLE)",
    )
    parser.add_argument(
        "--address",
        default=None,
        help="Bind CoreBluetooth UUID (overrides .pixel-ble-bind.json)",
    )
    parser.add_argument(
        "--chip",
        default=os.environ.get("PIXEL_BLE_CHIP", ""),
        help="4-char chip tag from device I: reply",
    )
    parser.add_argument(
        "--resolve-bind",
        metavar="UUID",
        help="Probe address, save chip+name, exit (used by npm run bind:ble)",
    )
    parser.add_argument(
        "--allow-rescan",
        action="store_true",
        help="If bound address fails, scan again (unsafe with multiple TYBLE)",
    )
    parser.add_argument(
        "--auto-bind",
        action="store_true",
        help="On unambiguous scan, write .pixel-ble-address automatically",
    )
    parser.add_argument("--timeout", type=float, default=30.0, help="Scan timeout seconds")
    args = parser.parse_args()
    try:
        if args.resolve_bind or args.set_address:
            target = (args.resolve_bind or args.set_address or "").strip()
            if not target:
                log("Usage: npm run bind:ble -- <address-from-scan>")
                rc = 1
            else:

                async def _do_bind() -> int:
                    class _D:
                        pass
                    d = _D()
                    d.address = target
                    d.name = args.name
                    r = await probe_device(d)
                    if not r:
                        log("Bind failed — probe/identity timeout (flash new firmware?)")
                        return 1
                    addr, name, _, _, _, chip = r
                    save_bind(addr, chip, name or "")
                    log(f"Bound OK: {addr} chip={chip or '?'} name={name}")
                    log("Restart: npm run start:ble")
                    return 0

                rc = asyncio.run(_do_bind())
        elif args.scan:
            rc = asyncio.run(scan_only(args.timeout))
        else:
            bind = load_bind()
            addr = (args.address or "").strip() or bind.get("address") or ""
            chip = (args.chip or "").strip() or bind.get("chip") or ""
            if not addr:
                log("No bind yet. With multiple TYBLE nearby:")
                log("  1. npm run scan:ble")
                log("  2. npm run bind:ble -- <address>")
                log("  3. npm run start:ble")
            rc = asyncio.run(
                run(
                    args.name,
                    args.timeout,
                    addr or None,
                    chip,
                    allow_rescan=args.allow_rescan,
                    auto_bind=args.auto_bind,
                )
            )
    except KeyboardInterrupt:
        rc = 0
    sys.exit(rc)


if __name__ == "__main__":
    main()
