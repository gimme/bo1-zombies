"""The MP exe's function beside the SP exe's, aligned, operands symbolised: what a reader compares.

Lines that are in both are shown once (two spaces), with the SP one under a `~` where a number differs; `<` is MP
only, `>` SP only. `#n` is a branch to instruction n. A call shows both targets and how the callees compare:
[callee E] the same code, L the same but for numbers, D different code, ? not compared.
"""
import json, difflib, re, os, struct
from fcmp import Exe, Comparer, Result, _cstr
from paths import OUT, MP_EXE, SP_EXE

# Functions identified by reading, MP address : SP address.
KNOWN = {
    "0067fdd0:0064a840": "Scr_Error", "00502ca0:0057cdd0": "va", "0054fad0:005c6da0": "Scr_GetNumParam",
    "0049f830:0045dbb0": "Scr_AddInt", "0065d930:00644900": "Scr_Error(server)", "005f8970:004c1bb0": "Scr_GetInt",
    "0065bd60:00473380": "Scr_GetFloat", "006a8160:00567cb0": "Scr_GetString", "0055b680:00462860": "Scr_GetVector",
    "006245e0:004b5940": "Scr_ErrorWithMsg", "004faaf0:0065e540": "Scr_AddFloat", "004c69b0:0069edb0": "Scr_ParamError",
    "00532ef0:00432940": "Scr_AddVector", "00624c70:00687530": "SL_ConvertToString", "0069df80:0043fad0": "Scr_GetType",
    "0068b8c0:005fd1f0": "Scr_AddString", "00441940:00642f10": "Scr_GetEntity", "004805b0:005c86d0": "Scr_MakeArray",
    "005f20c0:004b3a60": "Scr_GetConstString", "0041b6e0:005d1590": "Scr_GetPointerType",
    "005cde90:00653280": "SND_FindAliasId", "004126c0:0043bf30": "Com_Printf", "004fa5c0:004c9b60": "Sys_Error",
    "00627380:00651d90": "Com_Error", "005c9750:0043c520": "Com_PrintMessage", "009ca860:00965750": "memcpy",
}
_names = None
_pairs = None


def names():
    global _names
    if _names is None:
        from tables import Tables
        _names = Tables().exe_names()
        for k, v in KNOWN.items():
            a, b = k.split(":")
            _names["mp"][int(a, 16)] = v
            _names["sp"][int(b, 16)] = v
    return _names


def pairs():
    global _pairs
    if _pairs is None:
        try:
            _pairs = json.load(open(os.path.join(OUT, "triage.json")))["pairs"]
        except FileNotFoundError:
            _pairs = {}
    return _pairs


def sym(exe, side, v, is_mem, size):
    k = exe.kind(v)
    if k is None:
        return None
    if k == "import":
        return "<%s>" % exe.imports[v]
    if k == "code":
        return "<fn %s>" % names()[side].get(v, "%08x" % v)
    if k == "rdata":
        s = _cstr(exe, v)
        if s is not None and not is_mem and len(s) > 1:
            return '"%s"' % s[:70].decode("latin1").replace("\n", "\\n")
        b = exe.pe.read(v, size if is_mem and size in (4, 8) else 4)
        if is_mem and size == 4 and b:
            return "<const %s f=%g>" % (b.hex(), struct.unpack("<f", b)[0])
        if is_mem and size == 8 and b:
            return "<const f8=%g>" % struct.unpack("<d", b)[0]
        return "<rdata>"
    return "<g>"


def lines(exe, side, va):
    """-> func, display lines, shape keys (numbers folded), literal keys (addresses folded, numbers kept)"""
    f = exe.func(va)
    out, keys, lkeys, tgts = [], [], [], []
    for i, x in enumerate(f.insns):
        text = x.text
        key = x.mnem + " " + re.sub(r"0x[0-9a-f]+", "N", x.text)
        lkey = x.mnem + " " + x.text
        tgt = None
        if x.flow in ("call", "tail"):
            nm = names()[side].get(x.target)
            text = ("%08x" % x.target) + (" ; " + nm if nm else "")
            key = lkey = x.mnem + " FN"
            tgt = x.target
        elif x.flow in ("jcc", "jmp"):
            j = f.index.get(x.target)
            text = "#%s" % j
            key = lkey = x.mnem + " L"
        else:
            notes = []
            for op in x.ops:
                v = None
                if op[0] == "i":
                    v = op[1]
                    s = sym(exe, side, v, False, op[2])
                elif op[0] == "m":
                    v = op[5]
                    s = sym(exe, side, v, True, op[6])
                else:
                    s = None
                if s:
                    notes.append(s)
                    strong = s[0] == '"' or s.startswith("<const") or "!" in s
                    if strong:
                        key += " " + s
                    lkey = lkey.replace("0x%x" % v, s if strong else "<a>")
            if notes:
                text += "   ; " + " ".join(notes)
        out.append("%3d %08x  %-7s %s" % (i, x.addr, x.mnem, text))
        keys.append(key)
        lkeys.append(lkey)
        tgts.append(tgt)
    return f, out, keys, lkeys, tgts


def numbers_differ(cmpr, x, y):
    """Two immediates in the image's address range that the comparison takes for numbers, not for one global."""
    for p, q in zip(x.ops, y.ops):
        if p[0] != "i" or q[0] != "i" or p[1] == q[1]:
            continue
        if not cmpr._addr_pair(Result(0, 0), 0, p[1], q[1], p[2], False, x.mnem):
            return True
        # taken for pointers to code: a number unless the two places are the same code
        if cmpr.A.kind(p[1]) == "code" and pairs().get("%08x:%08x" % (p[1], q[1]), "D") == "D":
            return True
    return False


def show(a, b, allv=False, A=None, B=None, fold=4):
    A = A or Exe(MP_EXE)
    B = B or Exe(SP_EXE)
    fa, la, ka, lka, ta = lines(A, "mp", a)
    fb, lb, kb, lkb, tb = lines(B, "sp", b)
    cmpr = Comparer(A, B)
    res = ["MP %08x (%d insns)%s   |   SP %08x (%d insns)%s" % (a, len(la), " " + names()["mp"].get(a, ""), b, len(lb), " " + names()["sp"].get(b, ""))]
    sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)
    same = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            n = i2 - i1
            same += n
            run = []
            for k in range(n):
                x, y = i1 + k, j1 + k
                if ta[x] is not None and tb[y] is not None:
                    v = pairs().get("%08x:%08x" % (ta[x], tb[y]), "?")
                    run.append(("  " + la[x] + "  | SP %08x%s  [callee %s]" % (tb[y], " ; " + names()["sp"][tb[y]] if tb[y] in names()["sp"] else "", v), v != "E"))
                elif lka[x] != lkb[y] or numbers_differ(cmpr, fa.insns[x], fb.insns[y]):
                    run.append(("  " + la[x] + "\n  ~" + lb[y][1:], True))
                else:
                    run.append(("  " + la[x], False))
            # fold the uneventful stretches
            keep = [allv or imp for _, imp in run]
            for k in range(len(run)):
                if not allv and (k < 2 or k >= len(run) - 2):
                    keep[k] = True
            k = 0
            while k < len(run):
                if keep[k]:
                    res.append(run[k][0])
                    k += 1
                else:
                    j = k
                    while j < len(run) and not keep[j]:
                        j += 1
                    if j - k <= 2:
                        for q in range(k, j):
                            res.append(run[q][0])
                    else:
                        res.append("  ... %d same ..." % (j - k))
                    k = j
        else:
            for k in range(i1, i2):
                res.append("< " + la[k])
            for k in range(j1, j2):
                res.append("> " + lb[k])
    res[0] += "   same %d of %d/%d" % (same, len(la), len(lb))
    return "\n".join(res)
