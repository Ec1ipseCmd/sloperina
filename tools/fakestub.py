"""A stand-in for Lichii's GDB stub, to test magicwatch.py's protocol handling.

    python fakestub.py <port> <fake pc> <fake lr>

Speaks the subset magicwatch uses (acks, qSupported, ?, c, \\x03, Z2/z2, Hg/Hc, g, s, D) and
reports a write-watchpoint stop shortly after each 'c' while a watchpoint is set.
"""
import socket
import struct
import sys
import threading
import time

port, pc, lr = int(sys.argv[1]), int(sys.argv[2], 0), int(sys.argv[3], 0)
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(1)
print("fakestub listening", flush=True)
c, _ = srv.accept()
watch = set()
running = False
lock = threading.Lock()


def send(p):
    with lock:
        c.sendall(b"$" + p.encode() + b"#%02x" % (sum(p.encode()) & 0xFF))


def later_stop():
    time.sleep(0.3)
    if running and watch:
        a = min(watch)
        send(f"T05thread:1a;watch:{a:x};")


buf = b""
while True:
    d = c.recv(4096)
    if not d:
        break
    buf += d
    while buf:
        if buf[:1] in (b"+", b"-"):
            buf = buf[1:]
            continue
        if buf[:1] == b"\x03":
            buf = buf[1:]
            running = False
            send("T02thread:1;")
            continue
        h = buf.find(b"#")
        if buf[:1] != b"$" or h == -1 or len(buf) < h + 3:
            break
        p, buf = buf[1:h].decode(), buf[h + 3:]
        c.sendall(b"+")
        print("<", p, flush=True)
        if p.startswith("qSupported"):
            send("PacketSize=4000;qXfer:features:read+")
        elif p == "?":
            send("T05thread:1;")
        elif p == "c":
            running = True
            threading.Thread(target=later_stop, daemon=True).start()
        elif p[:2] in ("Z2", "z2"):
            a = int(p.split(",")[1], 16)
            (watch.add if p[0] == "Z" else watch.discard)(a)
            send("OK")
        elif p[:2] in ("Hg", "Hc"):
            send("OK")
        elif p == "g":
            regs = [0] * 29 + [0, lr, 0x1000, pc]
            send(struct.pack("<33Q", *regs).hex() + struct.pack("<I", 0x60000000).hex())
        elif p == "s":
            send("T05thread:1a;")
        elif p == "D":
            send("OK")
            c.close()
            sys.exit(0)
        else:
            send("")
