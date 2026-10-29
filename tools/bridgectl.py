#!/usr/bin/env python3
"""Talk to an rfidnfc2usb_bridge over its serial port: show what it
is, change its settings, or print the tags it sees.

    python bridgectl.py --port COM6 info
    python bridgectl.py --port /dev/ttyACM0 config
    python bridgectl.py --port /dev/ttyACM0 config chip=rc522 ss=7
    python bridgectl.py --port /dev/cu.usbmodem1101 scan

Needs pyserial (pip install pyserial). The bridge speaks the PN532 HSU
protocol; its own commands are described in docs/protocol.md."""

import argparse
import json
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("bridgectl needs pyserial: pip install pyserial")

PREAMBLE = b"\x00\x00\xff"
ACK = b"\x00\x00\xff\x00\xff\x00"
WAKEUP = b"\x55\x55" + b"\x00" * 14
MAGIC = b"RFIDNFC2USB"
CHIPS = {0: "none", 1: "PN532", 2: "RC522", 3: "PN5180"}


class BridgeError(Exception):
    pass


class Pn532Link(object):
    """PN532 frames over a serial port."""

    def __init__(self, port, baud=115200):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        # Low DTR/RTS: on classic ESP32 boards they drive EN and GPIO0.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.rx = bytearray()

    def close(self):
        self.ser.close()

    def command(self, cmd, params=b"", timeout=1.0, wake=False):
        body = bytes([0xD4, cmd]) + bytes(params)
        if len(body) > 255:
            n = len(body)
            head = PREAMBLE + bytes([0xFF, 0xFF, n >> 8, n & 0xFF, (-(n >> 8) - (n & 0xFF)) & 0xFF])
        else:
            head = PREAMBLE + bytes([len(body), (-len(body)) & 0xFF])
        frame = head + body + bytes([(-sum(body)) & 0xFF, 0x00])
        self.ser.reset_input_buffer()
        del self.rx[:]
        self.ser.write((WAKEUP if wake else b"") + frame)
        if self._read_frame(min(timeout, 0.5)) != "ack":
            raise BridgeError("no ACK for command 0x%02X" % cmd)
        resp = self._read_frame(timeout)
        if resp is None or resp == "ack":
            raise BridgeError("no answer to command 0x%02X" % cmd)
        if resp == b"\x7f":
            raise BridgeError("command 0x%02X: syntax error (not supported)" % cmd)
        if len(resp) < 2 or resp[0] != 0xD5 or resp[1] != cmd + 1:
            raise BridgeError("unexpected answer to command 0x%02X" % cmd)
        return bytes(resp[2:])

    def abort(self):
        self.ser.write(ACK)

    def _read_frame(self, timeout):
        deadline = time.monotonic() + timeout
        buf = self.rx
        while True:
            start = buf.find(PREAMBLE)
            if start >= 0 and len(buf) >= start + 5:
                if buf[start + 3] == 0 and buf[start + 4] == 0xFF:
                    del buf[:start + 6]
                    return "ack"
                if buf[start + 3] == 0xFF and buf[start + 4] == 0xFF:
                    if len(buf) >= start + 8:
                        length = (buf[start + 5] << 8) | buf[start + 6]
                        header = 8
                    else:
                        length = None
                else:
                    length, header = buf[start + 3], 5
                    if (length + buf[start + 4]) & 0xFF:
                        del buf[:start + 1]
                        continue
                if length is not None and len(buf) >= start + header + length + 1:
                    body = bytes(buf[start + header:start + header + length])
                    check = buf[start + header + length]
                    del buf[:start + header + length + 1]
                    if (sum(body) + check) & 0xFF:
                        raise BridgeError("frame checksum error")
                    return body
            if time.monotonic() >= deadline:
                return None
            buf += self.ser.read(max(1, self.ser.in_waiting))


def bridge_info(link):
    """(chip name, has ISO 15693, {key: value}), or None for a real PN532."""
    try:
        data = link.command(0xF0)
    except BridgeError:
        return None
    if not data.startswith(MAGIC):
        return None
    rest = data[len(MAGIC):]
    fields = dict(item.split("=", 1) for item in rest[3:].decode("ascii", "replace").split(";") if "=" in item)
    fields["protocol"] = str(rest[0])
    return CHIPS.get(rest[1], "?"), bool(rest[2] & 1), fields


def cmd_info(link, args):
    fw = link.command(0x02, wake=True)
    print("PN532 firmware: IC 0x%02X, v%d.%d, supports 0x%02X" % tuple(fw[:4]))
    info = bridge_info(link)
    if info is None:
        print("Not an rfidnfc2usb_bridge (a real PN532?)")
        return
    chip, vicinity, fields = info
    print("Bridge firmware %s on %s (protocol %s)" % (fields.get("firmware"), fields.get("board"),
                                                      fields.get("protocol")))
    print("Reader: %s (setting: %s)%s" % (fields.get("reader"), fields.get("setting"),
                                          ", ISO 15693" if vicinity else ""))


def parse_value(text):
    if text in ("true", "false"):
        return text == "true"
    try:
        return int(text)
    except ValueError:
        return text


def cmd_config(link, args):
    if not args.settings:
        print(json.dumps(json.loads(link.command(0xF6).decode()), indent=2))
        return
    changes = {}
    for item in args.settings:
        if "=" not in item:
            sys.exit("settings are key=value, e.g. chip=rc522 ss=7")
        key, value = item.split("=", 1)
        changes[key] = parse_value(value)
    send_config(link, changes)


def send_config(link, changes):
    resp = link.command(0xF8, json.dumps(changes).encode())
    if resp[0] != 0:
        sys.exit("refused: " + resp[1:].decode("utf-8", "replace"))
    print("Saved; the bridge restarts with the new settings.")


def cmd_reset_config(link, args):
    send_config(link, {"reset": True})


def cmd_restart(link, args):
    link.command(0xFA)
    print("Restarting.")


def cmd_scan(link, args):
    link.command(0x14, b"\x01\x14\x01", wake=True)               # SAMConfiguration
    link.command(0x32, b"\x05\xff\x01\x01")                      # short retries
    info = bridge_info(link)
    vicinity = bool(info and info[1])
    print("Waiting for tags (Ctrl+C to stop)...")
    present = None
    misses = 0
    try:
        while True:
            seen = None
            resp = link.command(0x4A, b"\x01\x00", timeout=2.0)
            if resp and resp[0]:
                atqa, sak, uid_len = resp[2:4], resp[4], resp[5]
                uid = resp[6:6 + uid_len]
                ats = resp[6 + uid_len:]
                seen = uid
                if uid != present:
                    line = "ISO 14443A  UID %s  ATQA %s  SAK %02X" % (uid.hex().upper(), atqa.hex().upper(), sak)
                    if ats:
                        line += "  ATS %s" % ats.hex().upper()
                    print(line)
                link.command(0x52, b"\x00")                      # InRelease
            elif vicinity:
                resp = link.command(0xF2, timeout=1.0)
                if resp and resp[0]:
                    seen = resp[1:9]
                    if seen != present:
                        print("ISO 15693   UID %s" % seen.hex().upper())
            if seen is None:
                misses += 1
                if misses >= 2:
                    present = None
            else:
                misses = 0
                present = seen
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass


def cmd_ports(args):
    for port in list_ports.comports():
        if port.vid is None:
            continue
        print("%-20s %04X:%04X  %s" % (port.device, port.vid, port.pid, port.description))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", help="serial port (COM6, /dev/ttyACM0, /dev/cu.usbmodem...)")
    parser.add_argument("--baud", type=int, default=115200, help="classic ESP32 boards only")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("ports", help="list USB serial ports")
    sub.add_parser("info", help="show the bridge and its reader")
    config = sub.add_parser("config", help="show settings, or change them: key=value ...")
    config.add_argument("settings", nargs="*")
    sub.add_parser("reset-config", help="go back to the board's default settings")
    sub.add_parser("restart", help="restart the bridge")
    sub.add_parser("scan", help="print tags as they come")
    args = parser.parse_args()
    if args.command == "ports":
        cmd_ports(args)
        return
    if not args.port:
        parser.error("--port is needed (see: bridgectl.py ports)")
    try:
        link = Pn532Link(args.port, args.baud)
    except serial.SerialException as exc:
        sys.exit(str(exc))
    try:
        {"info": cmd_info, "config": cmd_config, "reset-config": cmd_reset_config,
         "restart": cmd_restart, "scan": cmd_scan}[args.command](link, args)
    except BridgeError as exc:
        sys.exit(str(exc))
    finally:
        link.close()


if __name__ == "__main__":
    main()
