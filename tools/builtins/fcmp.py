"""Compare a function of one exe with a function of another, mechanically.

Each function is swept from its entry into an ordered instruction list. Two lists compare instruction by
instruction: mnemonics, registers, operand shapes and the control flow must be the same. What may move between the
two exes is symbolised: call targets (compared in turn, recursively), globals (a mapping that must stay one to one),
strings and constants in .rdata (compared by content) and imports (by name).

Own verdicts for a pair:
  E  the same code
  L  the same code but for literal numbers: struct offsets, stack offsets, immediates, constants
  D  different code
"""
import struct, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X
from exe import PE, Index

OP_REG, OP_IMM, OP_MEM = X.X86_OP_REG, X.X86_OP_IMM, X.X86_OP_MEM
MAXFUNC = 0x20000


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "flow", "target", "text")

    def __init__(self, addr, size, mnem, ops, flow, target, text):
        self.addr, self.size, self.mnem, self.ops, self.flow, self.target, self.text = addr, size, mnem, ops, flow, target, text


class Func:
    __slots__ = ("entry", "insns", "index", "tables", "bad", "end")

    def __init__(self, entry):
        self.entry = entry
        self.insns = []
        self.index = {}
        self.tables = {}   # table va -> [targets]
        self.bad = None
        self.end = entry


class Exe:
    def __init__(self, path):
        self.pe = PE(path)
        self.idx = Index(path)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.funcs = {}
        t = [s for s in self.pe.sections if s["exec"]][0]
        self.text_lo, self.text_hi = t["va"], t["va"] + min(t["rsize"], t["vsize"])
        self.text_raw = self.pe.data[t["roff"]:t["roff"] + min(t["rsize"], t["vsize"])]
        self.imports = self._imports()
        self.secs = [(s["va"], s["va"] + max(s["vsize"], s["rsize"]), s) for s in self.pe.sections]

    def _imports(self):
        pe = self.pe
        d = pe.data
        peoff = struct.unpack_from("<I", d, 0x3C)[0]
        opt = peoff + 24
        imp_rva = struct.unpack_from("<I", d, opt + 96 + 8)[0]
        out = {}
        if not imp_rva:
            return out
        va = pe.base + imp_rva
        while True:
            desc = pe.read(va, 20)
            if not desc or len(desc) < 20:
                break
            oft, _, _, name_rva, ft = struct.unpack("<IIIII", desc)
            if not name_rva and not ft:
                break
            dll = (pe.cstr(pe.base + name_rva) or b"?").decode("latin1").lower()
            look = pe.base + (oft or ft)
            slot = pe.base + ft
            i = 0
            while True:
                v = pe.u32(look + 4 * i)
                if not v:
                    break
                if v & 0x80000000:
                    nm = "#%d" % (v & 0xFFFF)
                else:
                    nm = (pe.cstr(pe.base + v + 2) or b"?").decode("latin1")
                out[slot + 4 * i] = dll + "!" + nm
                i += 1
            va += 20
        return out

    def section(self, va):
        for lo, hi, s in self.secs:
            if lo <= va < hi:
                return s
        return None

    def kind(self, va):
        """'code', 'rdata', 'data' or None for a value that could be an address in this image."""
        if va in self.imports:
            return "import"
        s = self.section(va)
        if not s or s["name"] not in (".text", ".rdata", ".data"):
            return None
        if s["exec"]:
            return "code"
        return "data" if s["write"] else "rdata"

    def is_func_start(self, t):
        if t in self.idx.callers:
            return True
        o = t - self.text_lo
        return t % 16 == 0 and 0 < o < len(self.text_raw) and self.text_raw[o - 1] == 0xCC

    def decode(self, pc):
        o = pc - self.text_lo
        if o < 0 or o >= len(self.text_raw):
            return None
        for ins in self.md.disasm(self.text_raw[o:o + 16], pc, 1):
            return ins
        return None

    def func(self, entry):
        f = self.funcs.get(entry)
        if f is None:
            f = self._sweep(entry)
            self.funcs[entry] = f
        return f

    def _read_table(self, f, tva):
        """Jump table at tva: dword entries that point into the function. A table inside the code (the CRT's
        memcpy) ends where the nearest entry that points past its start begins."""
        out = []
        a = tva
        bound = self.text_hi
        while a + 4 <= bound:
            e = struct.unpack_from("<I", self.text_raw, a - self.text_lo)[0]
            if not (f.entry <= e < f.entry + MAXFUNC):
                break
            if e > tva:
                bound = min(bound, e)
            out.append(e)
            a += 4
        return out

    def _sweep(self, entry):
        f = Func(entry)
        pc = entry
        far = entry
        data = []  # (lo, hi) data regions inside the function's footprint
        bytetabs = {}
        prev = []
        while True:
            if pc - entry > MAXFUNC:
                f.bad = "too long"
                break
            skipped = False
            for lo, hi in data:
                if lo <= pc < hi:
                    pc = hi
                    skipped = True
            if skipped:
                if pc > far:
                    break
                continue
            ins = self.decode(pc)
            if ins is None:
                f.bad = "undecodable at %08x" % pc
                break
            nxt = pc + ins.size
            flow, target = None, None
            groups = ins.groups
            ops = []
            for op in ins.operands:
                if op.type == OP_REG:
                    ops.append(("r", op.reg, op.size))
                elif op.type == OP_IMM:
                    ops.append(("i", op.imm & 0xFFFFFFFF, op.size))
                elif op.type == OP_MEM:
                    m = op.mem
                    ops.append(("m", m.segment, m.base, m.index, m.scale, m.disp & 0xFFFFFFFF, op.size))
            term = False
            if X.X86_GRP_RET in groups or ins.id == X.X86_INS_IRETD:
                flow, term = "ret", True
            elif ins.id == X.X86_INS_INT3:
                flow, term = "int3", True
            elif X.X86_GRP_CALL in groups:
                if ops and ops[0][0] == "i":
                    flow, target = "call", ops[0][1]
                else:
                    flow = "icall"
            elif X.X86_GRP_JUMP in groups:
                if ops and ops[0][0] == "i":
                    target = ops[0][1]
                    if ins.id == X.X86_INS_JMP:
                        term = True
                        intra = False
                        if target >= entry and target - entry < MAXFUNC:
                            if target <= far or target <= pc:
                                intra = True
                            elif not self.is_func_start(target) and not self.idx.start_between(pc, target):
                                intra = True
                        if intra:
                            flow = "jmp"
                            far = max(far, target)
                        else:
                            flow = "tail"
                    else:
                        flow = "jcc"
                        if target >= entry:
                            far = max(far, target)
                else:
                    term = True
                    flow = "ijmp"
                    if ops and ops[0][0] == "m":
                        _, seg, base, index, scale, disp, _sz = ops[0]
                        if scale == 4 and index and self.text_lo <= disp < self.text_hi:
                            # a byte table that indexes this jump table sits right after it
                            bt = None
                            for p in prev[-4:]:
                                if p.mnem == "movzx":
                                    for o2 in p.ops:
                                        if o2[0] == "m" and o2[6] == 1 and self.text_lo <= o2[5] < self.text_hi:
                                            bt = o2[5]
                            # entries point into the function's code, which ends before its tables
                            tab = self._read_table(f, disp)
                            skip = 0
                            while not tab and skip < 16:
                                # a table indexed from 1 up has its base before its first entry
                                skip += 4
                                tab = self._read_table(f, disp + skip)
                            if not tab:
                                skip = 0
                            if bt and bt > disp:
                                tab = tab[:(bt - disp) // 4]
                            f.tables[disp] = tab
                            data.append((disp + skip, disp + skip + 4 * len(tab)))
                            if bt:
                                n = len(tab)
                                a = bt
                                while a < self.text_hi and self.text_raw[a - self.text_lo] < n:
                                    a += 1
                                bytetabs[bt] = self.text_raw[bt - self.text_lo:a - self.text_lo]
                                data.append((bt, a))
                            for e in tab:
                                far = max(far, e)
            f.insns.append(Insn(pc, ins.size, ins.mnemonic, tuple(ops), flow, target, ins.op_str))
            prev.append(f.insns[-1])
            pc = nxt
            if term and pc > far:
                # tables may trail the last instruction
                moved = True
                while moved:
                    moved = False
                    for lo, hi in data:
                        if lo <= pc < hi:
                            pc = hi
                            moved = True
                break
        f.end = pc
        f.index = {ins.addr: i for i, ins in enumerate(f.insns)}
        f.tables = {k: v for k, v in f.tables.items()}
        return f


class Result:
    __slots__ = ("a", "b", "own", "why", "calls", "globals", "lits", "consts", "n", "deep", "bad")

    def __init__(self, a, b):
        self.a, self.b = a, b
        self.own = "E"
        self.why = None
        self.calls = []     # (ta, tb, kind)
        self.globals = []   # (da, db)
        self.lits = []      # (index, kind, va, vb)
        self.consts = []    # (index, va, vb, desc)
        self.n = 0
        self.deep = None
        self.bad = None


def _cstr(exe, va):
    s = exe.pe.cstr(va, 400)
    if s is None or len(s) < 1:
        return None
    if all(32 <= c < 127 or c in (9, 10, 13) for c in s):
        return s
    return None


class Comparer:
    def __init__(self, A, B):
        self.A, self.B = A, B
        self.results = {}
        self.gmapA = {}
        self.gmapB = {}

    def _addr_pair(self, r, i, va, vb, size, is_mem, mnem=""):
        """Two different values in corresponding operands. True if they are the same thing symbolically."""
        A, B = self.A, self.B
        ka, kb = A.kind(va), B.kind(vb)
        if ka is None or kb is None or ka != kb:
            return False
        if not is_mem and ka in ("data", "rdata"):
            # An immediate in the image's address range can be a number all the same: a contents mask (0x280E033 in
            # MP, 0x280E833 in SP), a flag. Masks differ by a few bits, where two exes linked apart don't put a
            # global at addresses that close, and no one tests or ors with an address. A number that differs by
            # more bits than that still passes for a global here. (An immediate that points into code is compared
            # as code, which a number fails.)
            if bin(va ^ vb).count("1") <= 3 or mnem in ("or", "test", "xor"):
                return False
        if ka == "import":
            if A.imports[va] == B.imports[vb]:
                return True
            r.consts.append((i, va, vb, "import %s / %s" % (A.imports[va], B.imports[vb])))
            return True
        if ka == "code":
            r.calls.append((va, vb, "ptr"))
            return True
        if ka == "rdata":
            sa, sb = _cstr(A, va), _cstr(B, vb)
            if not is_mem and sa is not None and sb is not None:
                if sa != sb:
                    r.consts.append((i, va, vb, "string %r / %r" % (sa[:60], sb[:60])))
                return True
            n = size if is_mem and size in (1, 2, 4, 8, 16) else 4
            ba, bb = A.pe.read(va, n), B.pe.read(vb, n)
            if ba != bb:
                # pointers inside rdata tables move too; only a constant read directly counts
                if is_mem:
                    pa = struct.unpack("<I", ba)[0] if ba and len(ba) == 4 else None
                    pb = struct.unpack("<I", bb)[0] if bb and len(bb) == 4 else None
                    ka, kb = (A.kind(pa), B.kind(pb)) if pa is not None and pb is not None else (None, None)
                    if ka is not None and ka == kb:
                        # a pointer kept in .rdata: compare what it points at
                        if ka == "code":
                            r.calls.append((pa, pb, "ptr"))
                        elif ka == "import" and A.imports[pa] != B.imports[pb]:
                            r.consts.append((i, va, vb, "import %s / %s" % (A.imports[pa], B.imports[pb])))
                    else:
                        r.consts.append((i, va, vb, "const %s / %s" % (ba.hex() if ba else None, bb.hex() if bb else None)))
            r.globals.append((va, vb))
            return True
        r.globals.append((va, vb))
        return True

    def own(self, a, b):
        key = (a, b)
        r = self.results.get(key)
        if r is not None:
            return r
        r = Result(a, b)
        self.results[key] = r
        A, B = self.A, self.B
        fa, fb = A.func(a), B.func(b)
        r.n = len(fa.insns)
        if fa.bad or fb.bad:
            r.own, r.why, r.bad = "D", "sweep: %s / %s" % (fa.bad, fb.bad), True
            raw = self.raw_same(a, b)
            if raw is not None:
                r.own, r.why, r.calls = "E", "raw bytes", raw
            return r
        if len(fa.insns) != len(fb.insns):
            raw = self.raw_same(a, b)
            if raw is not None:
                r.own, r.why, r.calls = "E", "raw bytes", raw
                return r
            r.own = "D"
            # where do they part
            k = 0
            for k, (x, y) in enumerate(zip(fa.insns, fb.insns)):
                if x.mnem != y.mnem or len(x.ops) != len(y.ops) or any(p[0] != q[0] or (p[0] == "r" and p[1] != q[1]) for p, q in zip(x.ops, y.ops)):
                    break
            r.why = "length %d / %d, part at #%d: %08x %s %s | %08x %s %s" % (
                len(fa.insns), len(fb.insns), k, fa.insns[k].addr, fa.insns[k].mnem, fa.insns[k].text,
                fb.insns[k].addr, fb.insns[k].mnem, fb.insns[k].text) if fa.insns and fb.insns else "empty"
            return r
        for i, (x, y) in enumerate(zip(fa.insns, fb.insns)):
            def diff(msg):
                r.own = "D"
                r.why = "#%d %s: %08x %s %s | %08x %s %s" % (i, msg, x.addr, x.mnem, x.text, y.addr, y.mnem, y.text)
                return r
            if x.mnem != y.mnem:
                return diff("mnemonic")
            if x.flow != y.flow:
                return diff("flow")
            if len(x.ops) != len(y.ops):
                return diff("operands")
            if x.flow in ("jcc", "jmp"):
                ia, ib = fa.index.get(x.target), fb.index.get(y.target)
                if ia is None or ib is None or ia != ib:
                    return diff("branch target")
                continue
            if x.flow in ("call", "tail"):
                r.calls.append((x.target, y.target, x.flow))
                continue
            for p, q in zip(x.ops, y.ops):
                if p[0] != q[0]:
                    return diff("operand kind")
                if p[0] == "r":
                    if p[1] != q[1]:
                        return diff("register")
                elif p[0] == "i":
                    if p[2] != q[2]:
                        return diff("imm size")
                    if p[1] != q[1]:
                        if not self._addr_pair(r, i, p[1], q[1], p[2], False, x.mnem):
                            r.lits.append((i, "imm", p[1], q[1]))
                else:
                    if p[1:5] != q[1:5] or p[6] != q[6]:
                        return diff("memory operand")
                    if p[5] != q[5]:
                        ta, tb = fa.tables.get(p[5]), fb.tables.get(q[5])
                        if ta is not None or tb is not None:
                            if ta is None or tb is None or len(ta) != len(tb):
                                return diff("jump table")
                            for ea, eb in zip(ta, tb):
                                if fa.index.get(ea) is None or fa.index.get(ea) != fb.index.get(eb):
                                    return diff("jump table entry")
                            continue
                        if not self._addr_pair(r, i, p[5], q[5], p[6], True):
                            r.lits.append((i, "disp", p[5], q[5]))
        if r.lits or r.consts:
            r.own = "L"
        return r

    def raw_same(self, a, b):
        """Hand-written code the sweep can't follow (the CRT's memcpy): the bytes up to the next function are the
        same but for addresses inside the function, rel32 calls to matching places and globals."""
        A, B = self.A, self.B
        def nxt(E, va):
            i = bisect.bisect_right(E.idx.starts, va)
            return E.idx.starts[i] if i < len(E.idx.starts) else E.text_hi
        na, nb = nxt(A, a) - a, nxt(B, b) - b
        if na != nb or na > MAXFUNC:
            return None
        ra = A.text_raw[a - A.text_lo:a - A.text_lo + na]
        rb = B.text_raw[b - B.text_lo:b - B.text_lo + nb]
        calls = []
        i = 0
        while i < na:
            if ra[i] == rb[i]:
                i += 1
                continue
            ok = False
            for j in range(i, max(i - 4, -1), -1):
                if j + 4 > na:
                    continue
                va, vb = struct.unpack_from("<I", ra, j)[0], struct.unpack_from("<I", rb, j)[0]
                if a <= va < a + na and va - a == vb - b:
                    ok = True
                elif j > 0 and ra[j - 1] in (0xE8, 0xE9) and ra[j - 1] == rb[j - 1]:
                    calls.append(((a + j + 4 + va) & 0xFFFFFFFF, (b + j + 4 + vb) & 0xFFFFFFFF, "call"))
                    ok = True
                elif A.kind(va) in ("data", "rdata", "import") and A.kind(va) == B.kind(vb):
                    ok = True
                if ok:
                    i = j + 4
                    break
            if not ok:
                return None
        return calls

    def deep(self, a, b, maxfuncs=200000):
        """Post-order over the call pairs. -> Result with .deep in E / L / D and the closure's non-E pairs."""
        root = self.own(a, b)
        if root.deep is not None:
            return root
        order = []
        seen = {(a, b)}
        stack = [(a, b)]
        while stack:
            k = stack.pop()
            order.append(k)
            r = self.own(*k)
            if r.own == "D":
                continue
            for ta, tb, _ in r.calls:
                c = (ta, tb)
                if c not in seen:
                    seen.add(c)
                    stack.append(c)
        return root, order


def worst(a, b):
    return max(a, b, key="ELD".index)
