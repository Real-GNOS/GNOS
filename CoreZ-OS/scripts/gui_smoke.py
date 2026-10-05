#!/usr/bin/env python3
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DBG = Path("/tmp/gui_dbg.log")
QMP = Path("/tmp/qmp.sock")

def qmp_cmd(sock, cmd):
    sock.sendall(json.dumps(cmd).encode())
    buf = b""
    while b"\n" not in buf:
        buf += sock.recv(65536)
    return json.loads(buf.split(b"\n")[0])

def send_keys(sock, qcodes, delay=0.12):
    for q in qcodes:
        qmp_cmd(sock, {"execute": "send-key",
                       "arguments": {"keys": [{"type": "qcode", "data": q}]}})
        time.sleep(delay)

def qmp_try_quit(s):
    try:
        qmp_cmd(s, {"execute": "quit"})
    except Exception as e:
        print("quit exception:", repr(e))


def screendump(s, png_path="/tmp/gui_shot.png"):
    ppm = png_path.replace(".png", ".ppm")
    qmp_cmd(s, {"execute": "screendump", "arguments": {"filename": ppm}})
    time.sleep(1)
    import zlib
    data = open(ppm, "rb").read()
    pos = 0
    toks = []
    while len(toks) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        tok = b""
        while not data[pos:pos + 1].isspace():
            tok += data[pos:pos + 1]
            pos += 1
        toks.append(tok)
    pos += 1
    w, h = int(toks[1]), int(toks[2])
    rgb = data[pos:pos + w * h * 3]
    raw = b""
    stride = w * 3
    for yy in range(h):
        raw += b"\x00" + rgb[yy * stride:(yy + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload)))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    open(png_path, "wb").write(png)
    return png_path


def main():
    build = subprocess.run(["python3", "build.py"], cwd=ROOT,
                           capture_output=True, text=True)
    log = (build.stdout or "") + (build.stderr or "")
    if build.returncode != 0 or "error:" in log:
        print("FAIL: build failed")
        for ln in log.splitlines():
            if "error:" in ln:
                print(ln.strip())
        return 1
    subprocess.run(["python3", "scripts/make_ext4.py", "build",
                    "build/test_hd.img", "--smoke"], cwd=ROOT, check=True)
    DBG.unlink(missing_ok=True)
    QMP.unlink(missing_ok=True)
    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-accel", "tcg,tb-size=256", "-m", "1G",
         "-smp", sys.argv[sys.argv.index("--smp") + 1] if "--smp" in sys.argv
         else "1", "-hda", str(ROOT / "build/test_hd.img"),
         "-debugcon", f"file:{DBG}", "-display", "none", "-no-reboot",
         "-vga", "std",
         "-qmp", f"unix:{QMP},server=on,wait=off"] +
        (["-device", "virtio-gpu-pci"] if "--virtio" in sys.argv else []) +
        (["-net", "none", "-netdev",
          "user,id=n0,hostfwd=tcp:127.0.0.1:6000-:6000",
          "-device", "e1000,netdev=n0"] if "--x11" in sys.argv else []),
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.time() + 420
        text = ""
        while time.time() < deadline:
            if DBG.exists():
                text = DBG.read_text(errors="replace")
                if "[corez@corez" in text:
                    break
            if proc.poll() is not None:
                print(f"FAIL: qemu exited rc={proc.returncode}")
                try:
                    print("qemu stderr:",
                          proc.stderr.read().decode(errors="replace")[:2000])
                except Exception:
                    pass
                return 1
            time.sleep(1)
        else:
            print("FAIL: shell prompt timeout")
            return 1
        print("shell ready")

        s = socket.socket(socket.AF_UNIX)
        s.connect(str(QMP))
        time.sleep(0.5)
        s.recv(65536)
        qmp_cmd(s, {"execute": "qmp_capabilities"})
        time.sleep(0.3)
        send_keys(s, ["g", "u", "i", "ret"], delay=0.25)

        deadline = time.time() + 90
        marker = None
        while time.time() < deadline:
            if proc.poll() is not None:
                print(f"FAIL: qemu exited during gui rc={proc.returncode}")
                return 1
            if DBG.exists():
                text = DBG.read_text(errors="replace")
                if "gui: active=" in text:
                    marker = "session-started"
                    break
                if "Kernel panic" in text or "kernel panic" in text:
                    print("FAIL: kernel panic during gui")
                    print(text[-2000:])
                    return 1
            time.sleep(1)
        if not marker:
            print("FAIL: gui session log timeout")
            print(text[-2000:])
            return 1
        line = [ln for ln in text.splitlines() if "gui: active=" in ln][-1]
        print("PASS:", line.strip())

        if "--wmcheck" in sys.argv:
            time.sleep(6)

            def wmkey(hold, key):
                qmp_cmd(s, {"execute": "send-key", "arguments": {
                    "keys": [{"type": "qcode", "data": h} for h in hold] +
                            [{"type": "qcode", "data": key}]}})
                time.sleep(0.5)

            wmkey([], "spc")
            time.sleep(1)
            print("SHOT: " + screendump(s, "/tmp/gui_shot_wm1.png"))
            px = 1024 - 8 - (4 * (24 + 4) - 4)

            def rel_move(dx, dy, step=6):
                n = max(1, (max(abs(dx), abs(dy)) + step - 1) // step)
                sx = int(dx / n)
                sy = int(dy / n)
                for _ in range(n):
                    qmp_cmd(s, {"execute": "input-send-event", "arguments": {
                        "events": [
                            {"type": "rel", "data": {"axis": "x",
                                                     "value": sx}},
                            {"type": "rel", "data": {"axis": "y",
                                                     "value": sy}}]}})
                    time.sleep(0.02)

            rel_move(0, 768 - 15 - 384)
            rel_move(px + 12 - 512, 0)
            time.sleep(0.5)
            for down in (True, False):
                qmp_cmd(s, {"execute": "input-send-event", "arguments": {
                    "events": [{"type": "btn", "data": {"down": down,
                                                        "button": "left"}}]}})
                time.sleep(0.3)
            time.sleep(1.5)
            print("SHOT: " + screendump(s, "/tmp/gui_shot_wm2.png"))
            wmkey(["alt"], "t")
            time.sleep(2.0)
            print("SHOT: " + screendump(s, "/tmp/gui_shot_wm3.png"))
            qmp_try_quit(s)
            return 0

        if "--shot" in sys.argv:
            time.sleep(8)
            print("SHOT: " + screendump(s))
            qmp_try_quit(s)
            return 0

        if "--x11" in sys.argv:
            for _ in range(80):
                if DBG.exists() and "x11gw: listening" in                         DBG.read_text(errors="replace"):
                    break
                time.sleep(0.5)
            for _ in range(80):
                try:
                    t = socket.create_connection(("127.0.0.1", 6000), timeout=1)
                    t.close()
                    break
                except OSError:
                    time.sleep(0.5)
            probe = subprocess.Popen(
                ["python3", "scripts/x11_probe.py", "--hold", "10"], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            time.sleep(9)
            print("SHOT: " + screendump(s, "/tmp/gui_shot_x11.png"))
            out, _ = probe.communicate(timeout=90)
            for ln in (out or "").splitlines():
                print("probe| " + ln.strip())
            if "PASS: x11 probe" not in (out or ""):
                print("FAIL: x11 probe")
                return 1

        perf_lines = []
        t_end = time.time() + 20
        flip = 1
        n = 0
        while time.time() < t_end:
            qmp_cmd(s, {"execute": "input-send-event", "arguments": {
                "events": [{"type": "rel", "data": {"axis": "x",
                                                    "value": 12 * flip}},
                           {"type": "rel", "data": {"axis": "y",
                                                    "value": 7 * flip}}]}})
            n += 1
            if n % 25 == 0:
                flip = -flip
                time.sleep(0.05)
                text = DBG.read_text(errors="replace")
                for ln in text.splitlines():
                    if "gui: t=" in ln and ln not in perf_lines:
                        perf_lines.append(ln)
                        print(ln.strip())
        print(f"injected {n} moves")
        if not perf_lines:
            print("FAIL: perf stopped under mouse load (freeze)")
            return 1

        def combo(hold, key):
            qmp_cmd(s, {"execute": "send-key", "arguments": {
                "keys": [{"type": "qcode", "data": h} for h in hold] +
                        [{"type": "qcode", "data": key}]}})
            time.sleep(0.4)

        time.sleep(2)
        combo(["alt"], "ret")
        combo(["alt"], "tab")
        combo(["alt"], "a")
        combo(["alt"], "m")
        combo(["alt"], "m")
        combo(["alt"], "t")
        combo(["alt"], "t")
        combo(["alt", "shift"], "e")

        deadline = time.time() + 90
        ok = False
        while time.time() < deadline:
            if proc.poll() is not None:
                print("FAIL: qemu exited during interaction")
                return 1
            text = DBG.read_text(errors="replace")
            if "Kernel panic" in text or "kernel panic" in text:
                print("FAIL: panic during interaction")
                print(text[-2000:])
                return 1
            if "gui: session ended" in text and "compositor: exit requested" in text:
                ok = True
                break
            time.sleep(1)
        if not ok:
            print("FAIL: session did not exit cleanly")
            print(text[-2000:])
            return 1
        print("PASS: session exited cleanly")
        # gui 现以 fork+exec 运行，退出后 shell 应当重生
        deadline = time.time() + 60
        ok = False
        while time.time() < deadline:
            if DBG.read_text(errors="replace").count("[corez@corez") >= 2:
                ok = True
                break
            if proc.poll() is not None:
                break
            time.sleep(1)
        print("PASS: shell respawned"
              if ok else "WARN: shell prompt not seen after exit")
        time.sleep(1)
        qmp_try_quit(s)
        return 0
    finally:
        try:
            proc.kill()
        except Exception as e:
            print("kill exception:", repr(e))

if __name__ == "__main__":
    _rc = main()
    sys.stdout.flush()
    sys.stderr.flush()
    import os
    os._exit(_rc)
