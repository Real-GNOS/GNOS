#!/usr/bin/env python3
import socket
import struct
import sys
import time

HOLD = 0
_rest = []
_i = 1
while _i < len(sys.argv):
    _a = sys.argv[_i]
    if _a == "--hold":
        HOLD = int(sys.argv[_i + 1]) if _i + 1 < len(sys.argv) else 10
        _i += 2
        continue
    if _a.startswith("--"):
        _i += 1
        continue
    _rest.append(_a)
    _i += 1
HOST = _rest[0] if len(_rest) > 0 else "127.0.0.1"
PORT = int(_rest[1]) if len(_rest) > 1 else 6000

ROOT = 0x06000000
VISUAL = 0x00000021
DEPTH = 24
WID = 0x06001000
GID = 0x06001100
WIN_W, WIN_H = 320, 200

ExposureMask = 0x00008000
StructureNotifyMask = 0x00020000
ButtonPressMask = 0x00000004
PointerMotionMask = 0x00000040


def u16(v):
    return struct.pack("<H", v & 0xFFFF)


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


class Client:

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=8)
        self.sock.settimeout(6.0)
        self.seq = 0
        self.pending = []

    def send(self, data):
        self.sock.sendall(data)
        self.seq = (self.seq + 1) & 0xFFFF

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("连接被关闭")
            buf += chunk
        return buf

    def _packet(self):
        if self.pending:
            return self.pending.pop(0)
        head = self._read(32)
        extra_len = 0
        if head[0] == 1:
            extra_len = struct.unpack("<I", head[4:8])[0] * 4
        extra = self._read(extra_len) if extra_len else b""
        return head[0], head, extra

    def reply(self, tag=""):
        while True:
            typ, head, extra = self._packet()
            if typ == 0:
                raise RuntimeError(
                    f"X11 error: code={head[1]} major={head[10]} "
                    f"minor={struct.unpack('<H', head[8:10])[0]} tag={tag}")
            if typ == 1:
                seq = struct.unpack("<H", head[2:4])[0]
                if seq != self.seq:
                    self.pending.append((typ, head, extra))
                    continue
                return head, extra
            continue

    def event(self, want_type, timeout=4.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            typ, head, extra = self._packet()
            if typ == 0:
                raise RuntimeError(f"等待事件时收到错误 code={head[1]}")
            if typ == want_type:
                return head, extra
        raise TimeoutError(f"未收到事件 type={want_type}")


def main():
    steps = []
    c = Client(HOST, PORT)
    print(f"connected {HOST}:{PORT}")

    c.sock.sendall(bytes([0x6C, 0]) + u16(11) + u16(0) + u16(0) + u16(0) +
                   b"\x00\x00")
    head = c._read(8)
    if head[0] != 1:
        print(f"FAIL: setup 失败 status={head[0]}")
        return 1
    major, minor, add_len = struct.unpack("<HHH", head[2:8])
    body = c._read(add_len * 4)
    release = struct.unpack("<I", body[0:4])[0]
    vendor_len = struct.unpack("<H", body[16:18])[0]
    n_screens = body[20]
    vpad = (vendor_len + 3) & ~3
    vendor = body[32:32 + vendor_len].decode(errors="replace")
    screen_off = 32 + vpad + 8
    root = struct.unpack("<I", body[screen_off:screen_off + 4])[0]
    scr_w, scr_h = struct.unpack("<HH", body[screen_off + 20:screen_off + 24])
    print(f"setup ok: proto {major}.{minor} release=0x{release:08x} "
          f"vendor={vendor!r} screens={n_screens} root=0x{root:08x} "
          f"screen={scr_w}x{scr_h}")
    steps.append("handshake")
    if root == 0 or major != 11:
        print("FAIL: setup 内容异常")
        return 1

    values = (u32(0x00204060) +
              u32(ExposureMask | StructureNotifyMask | ButtonPressMask |
                  PointerMotionMask))
    fixed = (u32(WID) + u32(root) + struct.pack("<hh", 40, 40) + u16(WIN_W) +
             u16(WIN_H) + u16(0) + u16(1) + u32(VISUAL) +
             u32(0x00000002 | 0x00000800))
    c.send(bytes([1, DEPTH]) + u16((32 + len(values)) // 4) + fixed + values)
    steps.append("CreateWindow")

    c.send(bytes([2, 0]) + u16(4) + u32(WID) + u32(0x00000002) + u32(0x203040))
    steps.append("ChangeWindowAttributes")

    c.send(bytes([8, 0]) + u16(2) + u32(WID))
    ev, _ = c.event(12)
    ex, ey, ew, eh = struct.unpack("<HHHH", ev[8:16])
    print(f"Expose: {ew}x{eh}+{ex}+{ey}")
    steps.append("MapWindow+Expose")

    name = b"WM_NAME"
    pad = b"\x00" * ((4 - len(name) % 4) % 4)
    c.send(bytes([16, 0]) + u16((8 + len(name) + len(pad)) // 4) +
           u16(len(name)) + u16(0) + name + pad)
    head, _ = c.reply("InternAtom")
    atom = struct.unpack("<I", head[8:12])[0]
    print(f"InternAtom WM_NAME = {atom}")
    if atom == 0:
        print("FAIL: InternAtom 返回 None")
        return 1
    steps.append("InternAtom")

    data = b"ncs-x11-probe\x00\x00\x00"
    c.send(bytes([18, 0]) + u16(10) + u32(WID) + u32(atom) + u32(atom) +
           bytes([8, 0]) + u16(0) + u32(16) + data)
    c.send(bytes([20, 0]) + u16(6) + u32(WID) + u32(atom) + u32(0) + u32(0) +
           u32(0xFFFFFFFF))
    head, extra = c.reply("GetProperty")
    n = struct.unpack("<I", head[16:20])[0]
    got = extra[:n] if n else b""
    print(f"GetProperty format={head[1]} n={n} data={got!r}")
    if got != data:
        print("FAIL: 属性往返不一致")
        return 1
    steps.append("ChangeProperty/GetProperty")

    c.send(bytes([84, 0]) + u16(4) + u32(0x20) + u16(0xFFFF) + u16(0x8000) +
           u16(0x0000) + u16(0))
    head, _ = c.reply("AllocColor")
    pixel = struct.unpack("<I", head[16:20])[0]
    print(f"AllocColor => pixel=0x{pixel:06x} red_echo="
          f"{struct.unpack('<H', head[8:10])[0]}")
    if (pixel >> 16) != 0xFF:
        print("FAIL: AllocColor 像素值不符")
        return 1
    steps.append("AllocColor")

    c.send(bytes([12, 0]) + u16(5) + u32(WID) + u16(0x0001 | 0x0002) +
           u16(0) + struct.pack("<ii", 120, 200))
    c.send(bytes([12, 0]) + u16(5) + u32(WID) + u16(0x0004 | 0x0008) + u16(0) +
           u32(360) + u32(240))
    time.sleep(0.3)
    steps.append("ConfigureWindow")

    c.send(bytes([61, 0]) + u16(4) + u32(WID) + struct.pack("<hhhh", 0, 0, 0, 0))
    steps.append("ClearArea")

    c.send(bytes([55, 0]) + u16(5) + u32(GID) + u32(0x20) + u32(0x0000FF) +
           u32(0))
    steps.append("CreateGC")

    rects = (struct.pack("<hhhh", 10, 10, 120, 60) +
             struct.pack("<hhhh", 150, 10, 140, 60))
    c.send(bytes([70, 0]) + u16((12 + len(rects)) // 4) + u32(WID) + u32(GID) +
           rects)
    steps.append("PolyFillRectangle")

    pts = struct.pack("<hhhhhhhh", 10, 90, 100, 150, 200, 90, 300, 150)
    c.send(bytes([65, 0]) + u16((12 + len(pts)) // 4) + u32(WID) + u32(GID) +
           pts)
    steps.append("PolyLine")

    tri = struct.pack("<hhhhhh", 60, 175, 150, 120, 250, 175)
    c.send(bytes([69, 0]) + u16((12 + len(tri)) // 4) + u32(WID) + u32(GID) +
           tri)
    steps.append("FillPoly")

    iw, ih = 32, 16
    row_bytes = (iw * 3 + 3) & ~3
    img = bytearray()
    for y in range(ih):
        for x in range(iw):
            img += bytes([(x * 8) % 256, (y * 16) % 256, 255])
        img += b"\x00" * (row_bytes - iw * 3)
    fixed = (u32(WID) + u32(GID) + u16(iw) + u16(ih) +
             struct.pack("<hh", 240, 100) + bytes([0, 24, 0, 0]))
    c.send(bytes([72, 2]) + u16((24 + len(img)) // 4) + fixed + bytes(img))
    steps.append("PutImage")

    c.send(bytes([14, 0]) + u16(2) + u32(WID))
    head, _ = c.reply("GetGeometry")
    gd = head[1]
    gx, gy, gw, gh, gb = struct.unpack("<hhhhh", head[12:22])
    print(f"GetGeometry: depth={gd} {gw}x{gh}+{gx}+{gy} border={gb}")
    if gw != 360 or gh != 240 or gx != 120 or gy != 200 or gd != DEPTH:
        print("FAIL: 几何不符")
        return 1
    steps.append("GetGeometry")

    c.send(bytes([38, 0]) + u16(2) + u32(WID))
    head, _ = c.reply("QueryPointer")
    rx, ry = struct.unpack("<hh", head[16:20])
    print(f"QueryPointer: same_screen={head[1]} root=({rx},{ry})")
    steps.append("QueryPointer")

    c.send(bytes([120, 0]) + u16(2) + u32(0))
    while True:
        typ, head, _ = c._packet()
        if typ == 0:
            if head[1] == 1:
                print("错误回复: BadRequest（符合协议）")
                steps.append("BadRequest")
                break
            print(f"FAIL: 错误码应为 BadRequest，实得 {head[1]}")
            return 1
        if typ == 1:
            continue

    if HOLD > 0:
        print(f"HOLD: 窗口保持 {HOLD}s")
        time.sleep(HOLD)

    c.send(bytes([4, 0]) + u16(2) + u32(WID))
    time.sleep(0.2)
    c.sock.close()

    print("PASS: x11 probe 全部步骤完成 ->", ", ".join(steps))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        sys.exit(1)
