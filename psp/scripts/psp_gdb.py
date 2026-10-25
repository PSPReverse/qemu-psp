"""Reusable gdb commands for tracing the PSP under QEMU.  See RECIPES.md.

    (gdb) source psp/scripts/psp_gdb.py

Every command takes a spec list: a path to a YAML/JSON file, or inline JSON
(starts with '[' or '{').  YAML needs PyYAML; JSON always works.  One JSON line
is written per hit, to stdout or to the file set with `psp-out <path>`.

    psp-out PATH            send hit lines to PATH (no argument: stdout again)
    psp-names CSV           address,name lines (e.g. a Ghidra export) used to fill `name`
    psp-who-touches SPEC    [{addr, kind: r|w|a, size, name, regs}]       watchpoints
    psp-caller SPEC         [{addr, sp_off, thumb, name}]                 lr + saved lr at [sp+sp_off]
    psp-func SPEC           [{addr, thumb, name}]                         entry/return log
    psp-svc SPEC            [{addr, source: lr|pc, thumb, name}]          SVC number decode
    psp-dump ADDR LEN FILE  raw bytes to FILE
    psp-dump-struct SPEC    [{addr, name, fields: [{name, off, size}]}]   one JSON line each
    psp-clear               delete the breakpoints these commands created

`thumb` (optional) forces the breakpoint instruction set; otherwise gdb decides.
"""
import bisect
import json
import sys

import gdb

try:
    import yaml
except ImportError:
    yaml = None

REGS = ["r%d" % i for i in range(13)] + ["sp", "lr", "pc", "cpsr"]
M32 = 0xFFFFFFFF

_out = sys.stdout
_names = {}
_sorted = []
_bps = []


def _reg(name):
    return int(gdb.selected_frame().read_register(name)) & M32


def _mem(addr, size):
    return bytes(gdb.selected_inferior().read_memory(addr, size))


def _word(addr, size=4):
    return int.from_bytes(_mem(addr, size), "little")


def _is_thumb():
    return bool(_reg("cpsr") & 0x20)


def _name(addr):
    """Exact symbol first, else nearest preceding as 'sym+0xoff'."""
    addr &= ~1
    if addr in _names:
        return _names[addr]
    i = bisect.bisect_right(_sorted, addr) - 1
    if i >= 0:
        return "%s+0x%x" % (_names[_sorted[i]], addr - _sorted[i])
    return None


def log_hit(event, name=None, regs=None, **extra):
    """Write one JSON line: event, name, all registers (or `regs` + pc/lr), extras."""
    rec = {"event": event}
    regs_all = {r: _reg(r) for r in REGS}
    keep = REGS if not regs else [r for r in REGS if r in regs or r in ("pc", "lr")]
    pc = regs_all["pc"]
    rec["name"] = name or _name(pc)
    for r in keep:
        rec[r] = "0x%x" % regs_all[r]
    rec["lr_name"] = _name(regs_all["lr"])
    for k, v in extra.items():
        rec[k] = "0x%x" % v if isinstance(v, int) and not isinstance(v, bool) else v
    _emit(rec)


def _emit(rec):
    _out.write(json.dumps(rec) + "\n")
    _out.flush()


def load_spec(arg):
    arg = arg.strip()
    if arg[:1] in "[{":
        data = json.loads(arg)
    else:
        with open(arg) as f:
            text = f.read()
        try:
            data = json.loads(text)
        except ValueError:
            if yaml is None:
                raise gdb.GdbError("PyYAML not importable in gdb's python; give JSON")
            data = yaml.safe_load(text)
    return [data] if isinstance(data, dict) else data


def _int(v):
    return v if isinstance(v, int) else int(v, 0)


def _bp_addr(spec):
    a = _int(spec["addr"])
    if "thumb" in spec:  # odd address = Thumb for gdb's ARM target
        a = (a & ~1) | (1 if spec["thumb"] else 0)
    return a


def _track(bp):
    _bps.append(bp)
    return bp


class _Cmd(gdb.Command):
    def __init__(self, name):
        super().__init__(name, gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        self.run(arg)


class Out(_Cmd):
    def run(self, arg):
        global _out
        if _out is not sys.stdout:
            _out.close()
        _out = open(arg.strip(), "a") if arg.strip() else sys.stdout


class Names(_Cmd):
    def run(self, arg):
        _names.clear()
        with open(arg.strip()) as f:
            for line in f:
                parts = line.strip().split(",")
                if len(parts) < 2:
                    continue
                try:
                    _names[int(parts[0].strip().strip('"'), 0) & ~1] = parts[1].strip().strip('"')
                except ValueError:
                    continue  # header
        _sorted[:] = sorted(_names)
        print("psp-names: %d symbols" % len(_names))


class Clear(_Cmd):
    def run(self, arg):
        for bp in _bps:
            if bp.is_valid():
                bp.delete()
        _bps.clear()


# ---- (a) who touches

class _Watch(gdb.Breakpoint):
    def __init__(self, spec):
        self.spec = spec
        self.addr = _int(spec["addr"])
        self.size = _int(spec.get("size", 4))
        kind = {"r": gdb.WP_READ, "w": gdb.WP_WRITE}.get(spec.get("kind", "a"), gdb.WP_ACCESS)
        ctype = {1: "unsigned char", 2: "unsigned short", 4: "unsigned int", 8: "unsigned long long"}[self.size]
        super().__init__("*(%s *)0x%x" % (ctype, self.addr), gdb.BP_WATCHPOINT, kind)

    def stop(self):
        log_hit("touch", name=self.spec.get("name"), regs=self.spec.get("regs"),
                addr=self.addr, kind=self.spec.get("kind", "a"), value=_word(self.addr, self.size))
        return False


class WhoTouches(_Cmd):
    def run(self, arg):
        for s in load_spec(arg):
            _track(_Watch(s))


# ---- (b) caller at a spin

class _Caller(gdb.Breakpoint):
    def __init__(self, spec):
        self.spec = spec
        super().__init__("*0x%x" % _bp_addr(spec))

    def stop(self):
        off = _int(self.spec.get("sp_off", 12))
        saved = _word(_reg("sp") + off)
        log_hit("caller", name=self.spec.get("name"), thumb=_is_thumb(),
                saved_lr=saved, saved_lr_name=_name(saved), sp_off=off)
        return False


class Caller(_Cmd):
    def run(self, arg):
        for s in load_spec(arg):
            _track(_Caller(s))


# ---- (c) function entry/return

_stack = []  # open frames: {addr, lr, sp}
_first_nonzero = [False]
_ret_bps = {}  # return address -> breakpoint


class _Ret(gdb.Breakpoint):
    def __init__(self, lr):
        self.lr = lr
        super().__init__("*0x%x" % lr, internal=True)

    def stop(self):
        sp = _reg("sp")
        for i in range(len(_stack) - 1, -1, -1):
            f = _stack[i]
            if f["lr"] == self.lr and f["sp"] == sp:
                del _stack[i:]
                r0 = _reg("r0")
                flag = r0 != 0 and not _first_nonzero[0]
                _first_nonzero[0] |= flag
                log_hit("return", name=_name(f["addr"]) or "0x%x" % f["addr"], depth=i,
                        callee=f["addr"], ret_r0=r0, first_nonzero=flag)
                break
        return False


class _Func(gdb.Breakpoint):
    def __init__(self, spec):
        self.spec = spec
        super().__init__("*0x%x" % _bp_addr(spec))

    def stop(self):
        lr, sp = _reg("lr"), _reg("sp")
        addr = _int(self.spec["addr"]) & ~1
        log_hit("entry", name=self.spec.get("name"), depth=len(_stack),
                a0=_reg("r0"), a1=_reg("r1"), a2=_reg("r2"), a3=_reg("r3"))
        _stack.append({"addr": addr, "lr": lr, "sp": sp})
        if lr not in _ret_bps:  # lr keeps its Thumb bit, so *lr is the right mode
            _ret_bps[lr] = _track(_Ret(lr))
        return False


class Func(_Cmd):
    def run(self, arg):
        _stack.clear()
        _first_nonzero[0] = False
        for s in load_spec(arg):
            _track(_Func(s))


# ---- (d) SVC

def decode_svc(source, thumb=None):
    """Return (svc_number, thumb, note).  source 'lr': trap return address; 'pc': the SVC itself."""
    base = _reg(source)
    if source == "pc":
        thumb = _is_thumb() if thumb is None else thumb
        base &= ~1
        return (_word(base, 2) & 0xFF if thumb else _word(base) & 0xFFFFFF), thumb, None
    lr = base & ~1
    arm_w, thumb_h = _word(lr - 4), _word(lr - 2, 2)
    arm_ok, thumb_ok = (arm_w >> 24) & 0xF == 0xF, thumb_h >> 8 == 0xDF
    if thumb is None:  # exception entry cleared T in cpsr: infer from what is at lr
        if thumb_ok and not arm_ok:
            thumb = True
        elif arm_ok and not thumb_ok:
            thumb = False
        else:
            return None, None, "ambiguous: arm=0x%x thumb=0x%x" % (arm_w & 0xFFFFFF, thumb_h & 0xFF)
    return (thumb_h & 0xFF if thumb else arm_w & 0xFFFFFF), thumb, None


class _Svc(gdb.Breakpoint):
    def __init__(self, spec):
        self.spec = spec
        super().__init__("*0x%x" % _bp_addr(spec))

    def stop(self):
        num, thumb, note = decode_svc(self.spec.get("source", "lr"), self.spec.get("thumb"))
        log_hit("svc", name=self.spec.get("name"), svc=num, svc_thumb=thumb, note=note)
        return False


class Svc(_Cmd):
    def run(self, arg):
        for s in load_spec(arg):
            _track(_Svc(s))


# ---- (e) dumps

class Dump(_Cmd):
    def run(self, arg):
        a, n, path = gdb.string_to_argv(arg)
        data = _mem(_int(a), _int(n))
        with open(path, "wb") as f:
            f.write(data)
        print("psp-dump: %d bytes -> %s" % (len(data), path))


class DumpStruct(_Cmd):
    def run(self, arg):
        for s in load_spec(arg):
            base = _int(s["addr"])
            fields = {}
            for fld in s["fields"]:
                size = _int(fld.get("size", 4))
                raw = _mem(base + _int(fld["off"]), size)
                fields[fld["name"]] = "0x%x" % int.from_bytes(raw, "little") if size <= 8 else raw.hex()
            _emit({"event": "struct", "name": s.get("name") or _name(base), "addr": "0x%x" % base,
                   "fields": fields})


for _n, _c in [("psp-out", Out), ("psp-names", Names), ("psp-clear", Clear),
               ("psp-who-touches", WhoTouches), ("psp-caller", Caller), ("psp-func", Func),
               ("psp-svc", Svc), ("psp-dump", Dump), ("psp-dump-struct", DumpStruct)]:
    _c(_n)
