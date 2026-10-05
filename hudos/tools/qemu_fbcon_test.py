#!/usr/bin/env python3
"""Boot hudos in QEMU, toggle fbcon + chinese test, take a screendump."""
import socket
import subprocess
import sys
import time

SERIAL = ("127.0.0.1", 4555)
MONITOR = ("127.0.0.1", 4556)
DUMP = "/tmp/hudos_fbcon.ppm"

def connect(port, tries=40):
    for _ in range(tries):
        try:
            s = socket.create_connection(port, timeout=2)
            s.settimeout(2)
            return s
        except OSError:
            time.sleep(0.5)
    raise SystemExit("cannot connect to %s" % (port,))

def drain(s, sec=1.0):
    out = b""
    end = time.time() + sec
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d:
                break
            out += d
        except socket.timeout:
            pass
    return out

def wait_for(s, needle, sec=30):
    out = b""
    end = time.time() + sec
    while time.time() < end:
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        out += d
        if needle in out:
            return out
    raise SystemExit("timeout waiting for %r; got:\n%s" % (needle, out[-2000:]))

def main():
    procs = []
    def spawn(*args):
        p = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                             stderr=subprocess.STDOUT)
        procs.append(p)
        return p

    spawn("qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
          "-m", "512", "-display", "none",
          "-bios", "/usr/share/qemu-efi-aarch64/QEMU_EFI.fd",
          "-drive", "if=virtio,format=raw,file=fat:rw:/home/Yin/gnos/hudos/esp",
          "-device", "virtio-gpu-pci",
          "-net", "none",
          "-serial", "tcp:127.0.0.1:4555,server,nowait",
          "-monitor", "tcp:127.0.0.1:4556,server,nowait")

    try:
        ser = connect(SERIAL)
        wait_for(ser, b"/>", sec=60)
        drain(ser, 1.0)
        for cmd in (b"fbcon\r", b"chinese\r", b"pwd\r"):
            ser.sendall(cmd)
            time.sleep(1.5)
            drain(ser, 1.0)
        time.sleep(2)

        mon = connect(MONITOR)
        drain(mon, 1.0)
        mon.sendall(("screendump %s\n" % DUMP).encode())
        time.sleep(2)
        drain(mon, 1.0)
        mon.sendall(b"quit\n")
        print("screendump ->", DUMP)
    finally:
        for p in procs:
            try:
                p.terminate()
            except OSError:
                pass

if __name__ == "__main__":
    main()
