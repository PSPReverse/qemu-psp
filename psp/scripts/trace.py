#!/usr/bin/env python3
"""Call/return tracer driving a QEMU gdbstub over the GDB Remote Serial Protocol.

Usage: trace.py CONFIG.json     |     trace.py --selftest      (see TRACE.md)
Stdlib only.
"""
import csv
import json
import os
import socket
import subprocess
import sys
import threading
import time
import unittest


# ---------------------------------------------------------------- RSP client

class GDB:
    def __init__(self, sock):
        self.s = sock
        if sock.family in (socket.AF_INET, socket.AF_INET6):  # tiny ack/frame writes: avoid Nagle stalls
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()

    def _byte(self):
        while not self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("gdbstub closed the connection")
            self.buf += d
        return self.buf.pop(0)

    def send(self, payload):
        frame = b"$" + payload + b"#" + ("%02x" % (sum(payload) & 0xFF)).encode()
        for _ in range(5):
            self.s.sendall(frame)
            ack = self._byte()
            if ack == ord("+"):
                return
            if ack != ord("-"):
                raise RuntimeError("unexpected ack byte 0x%02x" % ack)
        raise RuntimeError("packet NAKed 5 times: %r" % payload)

    def recv(self):
        while True:
            c = self._byte()
            if c == ord("$"):
                break  # anything else (stray +/-, interrupt) is skipped
        body = bytearray()
        raw_sum = 0  # the checksum covers the on-wire (escaped) bytes
        while True:
            c = self._byte()
            if c == ord("#"):
                break
            raw_sum += c
            if c == 0x7D:  # escaped byte
                c = self._byte()
                raw_sum += c
                c ^= 0x20
            body.append(c)
        cs = int(bytes([self._byte(), self._byte()]), 16)
        if cs != raw_sum & 0xFF:
            self.s.sendall(b"-")
            return self.recv()
        self.s.sendall(b"+")
        return bytes(body)

    def recv_stop(self, timeout):
        self.s.settimeout(timeout)
        while True:
            r = self.recv()
            if r.startswith(b"O") and r != b"OK":  # console output, not a reply or stop
                continue
            return r

    def cmd(self, payload):
        self.send(payload)
        return self.recv_stop(30)

    def interrupt(self):
        self.s.sendall(b"\x03")


def read_regs(g):
    raw = g.cmd(b"g").decode()
    w = [int.from_bytes(bytes.fromhex(raw[i:i + 8]), "little") for i in range(0, 16 * 8, 8)]
    cpsr = int.from_bytes(bytes.fromhex(g.cmd(b"p19").decode()), "little")  # reg 25 in arm-core.xml
    return w, cpsr


def read_mem(g, addr, length):
    r = g.cmd(b"m%x,%x" % (addr & 0xFFFFFFFF, length)).decode(errors="replace")
    if not r or r.startswith("E"):
        raise OSError("memory read %#x+%d failed: %r" % (addr, length, r))
    return bytes.fromhex(r)


# ------------------------------------------------------------------- helpers

def bp_kind(thumb):
    return 2 if thumb else 4


def is_stop(r):
    return r[:1] in (b"T", b"S")


def load_names(path):
    names = {}
    if not path:
        return names
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if len(row) < 2:
                continue
            try:
                names[int(row[0].strip(), 0) & ~1] = row[1].strip()
            except ValueError:
                try:
                    names[int(row[0].strip(), 16) & ~1] = row[1].strip()
                except ValueError:
                    pass  # header or junk
    return names


def decode_svc(g, lr):
    """SVC number from the instruction before lr: ARM word (lr-4) first, else Thumb halfword (lr-2)."""
    lr &= ~1
    try:
        w = int.from_bytes(read_mem(g, lr - 4, 4), "little")
        h = int.from_bytes(read_mem(g, lr - 2, 2), "little")
        # unconditional ARM svc (0xef......) first, then Thumb svc (0xdf..); a conditional
        # ARM svc (0x?f......, cond != e) is only taken when neither of those matches.
        if w >> 24 == 0xEF:
            return w & 0xFFFFFF, "arm"
        if h >> 8 == 0xDF:
            return h & 0xFF, "thumb"
        if (w >> 24) & 0x0F == 0x0F:
            return w & 0xFFFFFF, "arm"
    except OSError:
        pass
    return None, None


def hx(v):
    return "0x%08x" % v


# -------------------------------------------------------------------- tracer

class Tracer:
    def __init__(self, g, cfg, out):
        self.g, self.cfg, self.out = g, cfg, out
        self.names = load_names(cfg.get("names_csv"))
        self.stop = cfg.get("stop", {})
        self.svc_handler = cfg.get("svc_handler") if cfg.get("svc") else None
        self.bps = {}        # normalized addr -> bp spec
        self.inserted = {}   # normalized addr -> Z0 kind
        self.ret_refs = {}   # return addr -> open frames
        self.frames = []     # (callee, ret_addr)
        self.n = 0
        self.first_nonzero = None
        self.result = "running"

    def name(self, addr):
        return self.names.get(addr & ~1)

    def insert(self, addr, kind):
        if addr in self.inserted:
            return
        if self.g.cmd(b"Z0,%x,%d" % (addr, kind)) != b"OK":
            raise RuntimeError("Z0 failed at %#x" % addr)
        self.inserted[addr] = kind

    def remove(self, addr):
        kind = self.inserted.pop(addr, None)
        if kind is not None:
            self.g.cmd(b"z0,%x,%d" % (addr, kind))

    def emit(self, rec):
        self.out.write(json.dumps(rec) + "\n")
        self.out.flush()

    def setup(self):
        _, cpsr = read_regs(self.g)
        specs = list(self.cfg.get("breakpoints", []))
        fh = self.stop.get("first_hit")
        if fh is not None and all(s["addr"] & ~1 != fh & ~1 for s in specs):
            specs.append({"addr": fh, "kind": "hit"})
        for s in specs:
            a = s["addr"]
            thumb = bool(a & 1) or bool(cpsr & 0x20)
            self.bps[a & ~1] = s
            self.insert(a & ~1, bp_kind(thumb))
        if self.svc_handler is not None:
            self.insert(self.svc_handler & ~1, bp_kind(False))  # exception entry is ARM state

    def handle(self, stop):
        """Process a stop at a breakpoint. Return False to end the trace."""
        if not is_stop(stop):
            self.result = "target ended: " + stop.decode(errors="replace")
            return False
        w, cpsr = read_regs(self.g)
        pc, lr, sp = w[15], w[14], w[13]
        keep_going = True

        # return events first (a return address may also be a configured breakpoint)
        for i in range(len(self.frames) - 1, -1, -1):
            callee, ret = self.frames[i]
            if ret != pc:
                continue
            del self.frames[i:]  # pop this frame and anything nested above it
            self.release_ret(ret)
            ev = {"event": "return", "callee": callee, "ret_pc": hx(pc), "depth": len(self.frames)}
            ev.update({"r%d" % k: hx(w[k]) for k in range(4)})
            if w[0] != 0 and self.first_nonzero is None:
                self.first_nonzero = ev["first_nonzero"] = True
                if self.stop.get("first_return_nonzero"):
                    self.result = "first nonzero return"
                    keep_going = False
            self.emit(ev)
            break

        spec = self.bps.get(pc)
        is_svc = self.svc_handler is not None and pc == self.svc_handler & ~1
        if spec is not None or is_svc:
            kind = spec.get("kind", "hit") if spec else "hit"
            self.n += 1
            rec = {"n": self.n, "pc": hx(pc), "lr": hx(lr), "name": (spec or {}).get("name") or self.name(pc),
                   "cpsr": hx(cpsr)}
            rec.update({"r%d" % k: hx(w[k]) for k in range(13)})
            rec["sp"] = hx(sp)
            rec["depth"] = len(self.frames)
            rec["mem"] = self.read_specs(spec, w) if spec else []
            if is_svc:
                num, mode = decode_svc(self.g, lr)
                rec["svc"], rec["svc_mode"] = num, mode
            if kind in ("call", "return-of"):
                self.push_frame(rec["name"] or hx(pc), lr)
                rec["kind"] = kind
            if kind != "return-of":
                self.emit(rec)
            fh = self.stop.get("first_hit")
            if fh is not None and pc == fh & ~1:
                self.result = "first_hit"
                keep_going = False
            if self.stop.get("max_hits") and self.n >= self.stop["max_hits"]:
                self.result = "max_hits"
                keep_going = False
        return keep_going

    def push_frame(self, callee, lr):
        ret = lr & ~1
        self.frames.append((callee, ret))
        self.ret_refs[ret] = self.ret_refs.get(ret, 0) + 1
        self.insert(ret, bp_kind(bool(lr & 1)))

    def release_ret(self, ret):
        self.ret_refs[ret] -= 1
        if self.ret_refs[ret] <= 0:
            del self.ret_refs[ret]

    def wanted(self, addr):
        return addr in self.bps or addr in self.ret_refs or (
            self.svc_handler is not None and addr == self.svc_handler & ~1)

    def read_specs(self, spec, w):
        res = []
        for m in spec.get("mem", []):
            base = w[int(str(m["reg"]).lstrip("r"))] if "reg" in m else m["addr"]
            a = (base + m.get("off", 0)) & 0xFFFFFFFF
            try:
                res.append({"addr": hx(a), "len": m["len"], "hex": read_mem(self.g, a, m["len"]).hex()})
            except OSError as e:
                res.append({"addr": hx(a), "len": m["len"], "error": str(e)})
        return res

    def resume(self, pc):
        """Step off the breakpoint at pc, prune unneeded ones; return the step stop reply or None to continue."""
        a = pc & ~1
        kind = self.inserted.get(a)
        if kind is None:
            return None
        self.remove(a)
        step = self.g.cmd(b"s")
        if self.wanted(a):
            self.insert(a, kind)
        if not is_stop(step):
            return step
        nw, _ = read_regs(self.g)
        return step if nw[15] & ~1 in self.inserted else None

    def run(self):
        self.setup()
        end = time.time() + self.stop["wall_s"] if self.stop.get("wall_s") else None
        stop = None
        while True:
            if stop is None:
                self.g.send(b"c")
                try:
                    stop = self.g.recv_stop(max(0.01, end - time.time()) if end else None)
                except socket.timeout:
                    self.g.interrupt()
                    self.g.recv_stop(30)
                    self.result = "wall_s"
                    return
            if not self.handle(stop):
                return
            w, _ = read_regs(self.g)
            stop = self.resume(w[15])
            if stop is not None and not is_stop(stop):
                self.result = "target ended: " + stop.decode(errors="replace")
                return

    def cleanup(self):
        try:
            for a in list(self.inserted):
                self.remove(a)
            self.g.send(b"D")
            self.g.recv_stop(5)
        except (OSError, EOFError, RuntimeError):
            pass


# ----------------------------------------------------------------- front end

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def connect(host, port, proc=None, timeout=30):
    deadline = time.time() + timeout
    while True:
        try:
            return socket.create_connection((host, port), timeout=5)
        except OSError:
            if (proc is not None and proc.poll() is not None) or time.time() > deadline:
                raise RuntimeError("could not connect to gdbstub %s:%d" % (host, port))
            time.sleep(0.2)


def trace(cfg):
    proc = None
    out = open(cfg["out"], "w") if cfg.get("out") else sys.stdout
    try:
        if "qemu" in cfg:
            port = free_port()
            argv = list(cfg["qemu"]["argv"]) + ["-gdb", "tcp:127.0.0.1:%d" % port, "-S"]
            proc = subprocess.Popen(argv, stderr=subprocess.DEVNULL)
            host = "127.0.0.1"
        else:
            host, p = cfg["gdb"].rsplit(":", 1)
            port = int(p)
        g = GDB(connect(host, port, proc))
        t = Tracer(g, cfg, out)
        try:
            t.run()
        finally:
            t.cleanup()
        t.emit({"event": "done", "result": t.result, "hits": t.n, "first_nonzero_return": bool(t.first_nonzero)})
        return t
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if out is not sys.stdout:
            out.close()


def _int(v):
    return int(v, 0) if isinstance(v, str) else v


def _normalize(cfg):
    for s in cfg.get("breakpoints", []):
        s["addr"] = _int(s["addr"])
        for m in s.get("mem", []):
            if "addr" in m:
                m["addr"] = _int(m["addr"])
    st = cfg.setdefault("stop", {})
    if "first_hit" in st:
        st["first_hit"] = _int(st["first_hit"])
    if "svc_handler" in cfg:
        cfg["svc_handler"] = _int(cfg["svc_handler"])
    return cfg


# ------------------------------------------------------------------ selftest

class FakeStub(threading.Thread):
    """Scripted gdbstub: each 'c' jumps to the next scripted state whose pc has a breakpoint."""

    def __init__(self, script, mem=None, cpsr0=0):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.script, self.mem = list(script), mem or {}
        self.state = {"pc": 0, "lr": 0, "cpsr": cpsr0, "r": [0] * 13, "sp": 0x1000}
        self.bps, self.log, self.detached = {}, [], False

    def regs_g(self):
        s = self.state
        w = s["r"] + [s["sp"], s["lr"], s["pc"]]
        return "".join(v.to_bytes(4, "little").hex() for v in w).encode()

    def run(self):
        c, _ = self.srv.accept()
        g = GDB(c)
        try:
            while True:
                p = g.recv()
                self.log.append(p)
                if p == b"g":
                    g.send(self.regs_g())
                elif p == b"p19":
                    g.send(self.state["cpsr"].to_bytes(4, "little").hex().encode())
                elif p.startswith(b"m"):
                    a, n = (int(x, 16) for x in p[1:].split(b","))
                    if all(a + i in self.mem for i in range(n)):
                        g.send(bytes(self.mem[a + i] for i in range(n)).hex().encode())
                    else:
                        g.send(b"E01")
                elif p.startswith((b"Z0,", b"z0,")):
                    a, k = (int(x, 16) for x in p[3:].split(b","))
                    if p[:1] == b"Z":
                        self.bps[a] = k
                    else:
                        self.bps.pop(a, None)
                    g.send(b"OK")
                elif p == b"s":
                    self.state["pc"] += 4
                    g.send(b"T05")
                elif p == b"c":
                    while self.script and (self.script[0]["pc"] & ~1) not in self.bps:
                        self.script.pop(0)
                    if not self.script:
                        g.send(b"W00")
                        continue
                    st = self.script.pop(0)
                    self.state.update(pc=st["pc"], lr=st.get("lr", 0), cpsr=st.get("cpsr", 0),
                                      r=(st.get("r", []) + [0] * 13)[:13])
                    g.send(b"T05")
                elif p == b"D":
                    self.detached = True
                    g.send(b"OK")
                    return
                else:
                    g.send(b"")
        except (EOFError, OSError):
            pass


def _run_case(cfg, script, mem=None, cpsr0=0):
    stub = FakeStub(script, mem, cpsr0)
    stub.start()
    cfg = _normalize(dict(cfg, gdb="127.0.0.1:%d" % stub.port, out=None))
    import io
    buf = io.StringIO()
    g = GDB(socket.create_connection(("127.0.0.1", stub.port)))
    t = Tracer(g, cfg, buf)
    try:
        t.run()
    finally:
        t.cleanup()
    stub.join(5)
    return t, [json.loads(x) for x in buf.getvalue().splitlines()], stub


class SelfTest(unittest.TestCase):
    def test_framing(self):
        a, b = socket.socketpair()
        ga, gb = GDB(a), GDB(b)
        a.sendall(b"$bad#00")  # wrong checksum -> NAK, then a good packet
        a.sendall(b"$ab}\x5d#" + ("%02x" % ((ord("a") + ord("b") + 0x7D + 0x5D) & 0xFF)).encode())
        self.assertEqual(gb.recv(), b"ab}")
        self.assertEqual(a.recv(10), b"-+")
        t = threading.Thread(target=lambda: gb.send(b"OK"))
        t.start()
        self.assertEqual(ga.recv(), b"OK")
        t.join()

    def test_kinds_and_names(self):
        self.assertEqual((bp_kind(True), bp_kind(False)), (2, 4))
        cfg = {"breakpoints": [{"addr": 0x9701, "kind": "hit"}, {"addr": 0x9000, "kind": "hit"},
                               {"addr": 0xA000, "kind": "hit"}]}
        t, ev, stub = _run_case(cfg, [])  # ARM state: even -> 4, odd -> 2
        self.assertTrue(stub.detached)
        self.assertEqual([x for x in stub.log if x.startswith(b"Z0")],
                         [b"Z0,9700,2", b"Z0,9000,4", b"Z0,a000,4"])
        t, ev, stub = _run_case(cfg, [], cpsr0=0x20)  # Thumb state: all 2
        self.assertEqual({x[-1:] for x in stub.log if x.startswith(b"Z0")}, {b"2"})

    def test_call_return_nonzero(self):
        cfg = {"breakpoints": [{"addr": 0x9508, "kind": "call", "name": "f", "regs": []}],
               "stop": {"first_return_nonzero": True}}
        script = [
            {"pc": 0x9508, "lr": 0x2001, "r": [5]},        # Thumb caller
            {"pc": 0x9508, "lr": 0x3000, "r": [6]},        # nested ARM caller
            {"pc": 0x3000, "r": [0]},                      # inner returns 0
            {"pc": 0x2000, "r": [7]},                      # outer returns nonzero -> stop
            {"pc": 0x9508, "lr": 0x4000},                  # must never be reached
        ]
        t, ev, stub = _run_case(cfg, script)
        kinds = [(e.get("event"), e.get("depth")) for e in ev]
        self.assertEqual(kinds, [(None, 0), (None, 1), ("return", 1), ("return", 0)])
        self.assertNotIn("first_nonzero", ev[2])
        self.assertTrue(ev[3]["first_nonzero"])
        self.assertEqual(ev[3]["callee"], "f")
        self.assertEqual(t.result, "first nonzero return")
        zs = [x for x in stub.log if x.startswith(b"Z0")]
        self.assertIn(b"Z0,2000,2", zs)   # Thumb bit cleared, kind 2
        self.assertIn(b"Z0,3000,4", zs)
        self.assertEqual(stub.bps, {})    # everything removed on cleanup
        self.assertTrue(stub.detached)

    def test_first_hit_and_max(self):
        cfg = {"breakpoints": [{"addr": 0x100, "kind": "hit"}], "stop": {"first_hit": 0x200}}
        t, ev, _ = _run_case(cfg, [{"pc": 0x100}, {"pc": 0x200}, {"pc": 0x100}])
        self.assertEqual([e["pc"] for e in ev], ["0x00000100", "0x00000200"])
        self.assertEqual(t.result, "first_hit")
        cfg = {"breakpoints": [{"addr": 0x100, "kind": "hit"}], "stop": {"max_hits": 2}}
        t, ev, _ = _run_case(cfg, [{"pc": 0x100}] * 5)
        self.assertEqual((len(ev), t.result), (2, "max_hits"))

    def test_svc_decode(self):
        mem = {}
        for a, v in ((0x1ffc, 0xef000042),):   # ARM svc #0x42 at lr-4
            mem.update({a + i: b for i, b in enumerate(v.to_bytes(4, "little"))})
        mem.update({0x2ffe: 0x05, 0x2fff: 0xdf, 0x2ffc: 0x00, 0x2ffd: 0x00})  # Thumb svc 5 at lr-2
        cfg = {"svc": True, "svc_handler": 0x8}
        t, ev, _ = _run_case(cfg, [{"pc": 8, "lr": 0x2000}, {"pc": 8, "lr": 0x3000}], mem)
        self.assertEqual([(e["svc"], e["svc_mode"]) for e in ev], [(0x42, "arm"), (5, "thumb")])

    def test_names_csv(self):
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            f.write("address,name\n0x9508,bl_start_mp5\n")
        try:
            cfg = {"names_csv": f.name, "breakpoints": [{"addr": 0x9508, "kind": "hit"}]}
            _, ev, _ = _run_case(cfg, [{"pc": 0x9508}])
            self.assertEqual(ev[0]["name"], "bl_start_mp5")
        finally:
            os.unlink(f.name)


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        r = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SelfTest))
        return 0 if r.wasSuccessful() else 1
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1]) as f:
        trace(_normalize(json.load(f)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
