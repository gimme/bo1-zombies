"""The two retail exes as files: a minimal PE32 reader (sections, VA to file offset, reads), and the index of
call targets, which says where functions start."""
import struct, bisect, pickle, os
from paths import OUT


class PE:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not MZ")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not PE")
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsize = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x10B:
            raise ValueError("not PE32")
        self.entry = struct.unpack_from("<I", d, opt + 16)[0]
        self.base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        self.sections = []
        so = opt + optsize
        for i in range(nsec):
            o = so + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, roff = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append(dict(name=name, va=self.base + va, vsize=vsize, rsize=rsize, roff=roff,
                                      chars=chars, exec=bool(chars & 0x20000000), write=bool(chars & 0x80000000)))
        self.entry += self.base
        self.lo = self.base
        self.hi = self.base + self.size_of_image

    def sec(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + max(s["vsize"], s["rsize"]):
                return s
        return None

    def off(self, va):
        s = self.sec(va)
        if not s:
            return None
        o = va - s["va"]
        if o >= s["rsize"]:
            return None  # bss
        return s["roff"] + o

    def read(self, va, n):
        o = self.off(va)
        if o is None:
            return None
        return self.data[o:o + n]

    def u32(self, va):
        b = self.read(va, 4)
        if b is None or len(b) < 4:
            return None
        return struct.unpack("<I", b)[0]

    def cstr(self, va, maxlen=256):
        o = self.off(va)
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + maxlen)
        if end < 0:
            return None
        return self.data[o:end]

    def is_code(self, va):
        s = self.sec(va)
        return bool(s and s["exec"])

    def in_image(self, va):
        return self.lo <= va < self.hi


def build(pe):
    """Every E8 rel32 whose target is in .text: target -> [call sites]. A target is a function's start."""
    s = [x for x in pe.sections if x["exec"]][0]
    raw = pe.data[s["roff"]:s["roff"] + min(s["rsize"], s["vsize"])]
    lo, hi = s["va"], s["va"] + len(raw)
    callers = {}
    i = raw.find(b"\xe8")
    while i >= 0 and i + 5 <= len(raw):
        rel = struct.unpack_from("<i", raw, i + 1)[0]
        tgt = lo + i + 5 + rel
        if lo <= tgt < hi:
            callers.setdefault(tgt, []).append(lo + i)
        i = raw.find(b"\xe8", i + 1)
    return callers

def load(path):
    os.makedirs(OUT, exist_ok=True)
    cache = os.path.join(OUT, os.path.basename(path) + ".calls.pkl")
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(path):
        return pickle.load(open(cache, "rb"))
    c = build(PE(path))
    pickle.dump(c, open(cache, "wb"))
    return c

class Index:
    def __init__(self, path):
        self.callers = load(path)
        self.starts = sorted(self.callers)
    def func_of(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        return self.starts[i] if i >= 0 else None
    def start_between(self, lo, hi):
        """Is some call target in (lo, hi]: a function starts between the two."""
        i = bisect.bisect_right(self.starts, lo)
        return i < len(self.starts) and self.starts[i] <= hi
