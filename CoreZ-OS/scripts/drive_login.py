import os
import socket
import subprocess
import sys
import time

os.chdir("/root/nitian-os")
LOG = "/tmp/nit_login.log"
MON = "/tmp/mon.sock"
for p in (LOG, MON):
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass

proc = subprocess.Popen(
    ["qemu-system-x86_64", "-accel", "tcg,tb-size=256", "-m", "1G", "-smp", "1",
     "-hda", "build/login_hd.img", "-debugcon", f"file:{LOG}",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none",
     "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

keys = sys.argv[1] if len(sys.argv) > 1 else "ROOT"
keys = keys.encode().decode("unicode_escape")
try:
    deadline = time.time() + 90
    text = ""
    while time.time() < deadline:
        if os.path.exists(LOG):
            text = open(LOG, errors="replace").read()
            if "login:" in text:
                break
        time.sleep(1)

    s = socket.socket(socket.AF_UNIX)
    for _ in range(30):
        try:
            s.connect(MON)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(1)
    time.sleep(2)
    s.recv(4096)
    for ch in keys:
        name = {" ": "spc", "\n": "ret", "\x08": "backspace", ".": "dot",
                "-": "minus", "/": "slash", "=": "equal"}.get(ch, ch)
        if ch.isupper():
            s.sendall(f"sendkey shift-{ch.lower()}\n".encode())
        else:
            s.sendall(f"sendkey {name}\n".encode())
        time.sleep(0.2)
    s.sendall(b"sendkey ret\n")
    time.sleep(25)
    s.sendall(b"quit\n")
    time.sleep(2)
except Exception as e:
    print("ERR", e)
finally:
    proc.kill()

text = open(LOG, errors="replace").read()
print(text[-3000:])
