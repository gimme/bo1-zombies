"""The triage: every builtin the zombie maps' scripts call that runs a body from the MP exe, its function in
BlackOpsMP.exe compared with the same-named one in BlackOps.exe.

What comes out, for each builtin (status.tsv's triage column):
  same    its own code and all it calls is the same in the two exes
  layout  the same code but for numbers that moved in many functions alike: struct offsets and sizes
  number  the same code but for a number few other functions share, or a constant: to be read
  helper  its own code is the same or layout, a function it calls is different code: to be read
  code    its own code is different: to be read

The comparison follows calls, and stops at two kinds of helper, which are engine services and not a builtin's own
logic: the script VM's internals (the VM is MP's throughout, its variable ids 32-bit where SP's are 16-bit; a builtin
sees the same Scr_* calls either way), and the fatal error, error and print paths.
"""
import os, re, json, collections
from fcmp import Exe, Comparer
from tables import Tables, SP_ORDER
from paths import MP_EXE, SP_EXE, OUT, SRC, STATUS
import scripts

# The MP exe's script VM globals (variable table, VM stack and state, the string table's base): a helper that
# touches them is the VM's.
VM_GLOBALS = [(0x03DCB100, 0x03DD9000), (0x00C04F08, 0x00C04F0C)]
SERVICE = {(0x004FA5C0, 0x004C9B60): "Sys_Error", (0x00627380, 0x00651D90): "Com_Error",
           (0x005C9750, 0x0043C520): "Com_PrintMessage"}
SUPPORT = 3     # a moved number counts as layout when this many functions have it


def in_scope(T, calls):
    """The builtins a zombie map's scripts call that land on an MP body and aren't developer-only."""
    rows = []
    for lang, pre in (("gsc", ""), ("csc", "c")):
        for c in calls[lang]:
            kind = pre + c["kind"]
            r = T.port_lookup(kind, c["name"])
            if not r or r[3] != "mp" or r[2]:
                continue
            key, cfunc, dev, origin = r
            hits = T.sp_lookup(kind, c["name"])
            rows.append(dict(lang=lang, kind=kind, name=c["name"], calls=c["calls"], table=key, cfunc=cfunc,
                             mp=T.mp_lookup(key, c["name"]), sp=hits[0][1] if hits else None,
                             sp_table=hits[0][0] if hits else None))
    return rows


def compare(T, rows):
    """Own verdicts for every pair reachable from the builtins. -> comparer, {pair: [callee pairs]}"""
    C = Comparer(Exe(MP_EXE), Exe(SP_EXE))
    todo = [(r["mp"], r["sp"]) for r in rows if r["mp"] and r["sp"]]
    # every other builtin both exes have, for the count of which numbers moved in many functions
    for kind, order in SP_ORDER.items():
        seen = set()
        for key in [k for k, t in T.port.items() if t["kind"] == kind and t["origin"] == "mp"]:
            for n, f, dev in T.mp_of[key]["rows"]:
                hits = T.sp_lookup(kind, n)
                if n not in seen and hits:
                    todo.append((f, hits[0][1]))
                seen.add(n)
    seen = set(todo)
    edges = {}
    while todo:
        k = todo.pop()
        r = C.own(*k)
        kids = []
        if r.own != "D":
            for ta, tb, _ in r.calls:
                kids.append((ta, tb))
                if (ta, tb) not in seen:
                    seen.add((ta, tb))
                    todo.append((ta, tb))
        edges[k] = kids
    return C, edges


def boundaries(C, edges, builtin_pairs):
    tags = dict(SERVICE)
    for k in edges:
        if k in tags or k in builtin_pairs:
            continue
        for x in C.A.func(k[0]).insns:
            for op in x.ops:
                v = op[1] if op[0] == "i" else op[5] if op[0] == "m" else None
                if v is not None and any(lo <= v < hi for lo, hi in VM_GLOBALS):
                    tags[k] = "vm"
    return tags


def classify(rows, C, edges, tags):
    support = collections.defaultdict(set)
    for k, r in C.results.items():
        if r.own == "L":
            for i, kind, va, vb in r.lits:
                support[(kind, va, vb)].add(k)

    def weak(k):
        r = C.results[k]
        out = ["%s %#x/%#x" % (kind, va, vb) for i, kind, va, vb in r.lits if len(support[(kind, va, vb)]) < SUPPORT]
        return sorted(set(out + [d for i, va, vb, d in r.consts]))

    for row in rows:
        if not row["sp"]:
            row.update(triage="absent", own="", helpers=[])
            continue
        k = (row["mp"], row["sp"])
        closure, stack, seen = [], [k], {k}
        while stack:
            x = stack.pop()
            closure.append(x)
            if x in tags and x != k:
                continue
            for c in edges.get(x, []):
                if c not in seen:
                    seen.add(c)
                    stack.append(c)
        below = [p for p in closure if p != k and p not in tags]
        own = C.results[k].own
        dh = [p for p in below if C.results[p].own == "D"]
        wk = [p for p in ([k] if own == "L" else []) + [p for p in below if C.results[p].own == "L"] if weak(p)]
        if own == "D":
            t = "code"
        elif dh:
            t = "helper"
        elif wk:
            t = "number"
        elif own == "L" or any(C.results[p].own == "L" for p in below):
            t = "layout"
        else:
            t = "same"
        row.update(triage=t, own=own, helpers=dh + [p for p in wk if p != k])
    return rows


def port_functions():
    """Where each C function of the port is defined: name -> (file, first line, last line)."""
    defs = {}
    rx = re.compile(r"^(?:static\s+)?(?:[A-Za-z_][\w:<>\*&\s]*?)\b([A-Za-z_]\w*)\s*\(([^;{]*)\)\s*$")
    for dp, dn, fns in os.walk(SRC):
        for fn in fns:
            if not fn.endswith(".cpp"):
                continue
            p = os.path.join(dp, fn)
            lines = open(p, encoding="utf-8", errors="replace").read().split("\n")
            for i, ln in enumerate(lines):
                if not ln or ln[0] in " \t/#{}":
                    continue
                m = rx.match(ln)
                if not m:
                    continue
                j = i + 1
                while j < len(lines) and lines[j].strip() == "":
                    j += 1
                if j >= len(lines) or not lines[j].startswith("{"):
                    continue
                k = j
                while k < len(lines) and lines[k] != "}":
                    k += 1
                defs.setdefault(m.group(1), (os.path.relpath(p, SRC).replace("\\", "/"), i + 1, k + 1))
    return defs


COLUMNS = ["lang", "kind", "name", "calls", "port", "mp", "sp", "triage", "review", "note"]


def read_status():
    """status.tsv's hand-kept columns: (lang, kind, name) -> (review, note)"""
    kept = {}
    if os.path.exists(STATUS):
        for ln in open(STATUS, encoding="utf-8").read().split("\n")[1:]:
            c = ln.split("\t")
            if len(c) >= len(COLUMNS):
                kept[(c[0], c[1], c[2])] = (c[8], c[9])
    return kept


def write_status(rows):
    kept = read_status()
    out = ["\t".join(COLUMNS)]
    for r in sorted(rows, key=lambda r: (-r["calls"], r["lang"], r["name"], r["kind"])):
        review, note = kept.get((r["lang"], r["kind"], r["name"]), ("", ""))
        out.append("\t".join([r["lang"], r["kind"], r["name"], str(r["calls"]), r["cfunc"], "%08x" % r["mp"],
                              "%08x" % r["sp"] if r["sp"] else "", r["triage"], review, note]))
    open(STATUS, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


def run():
    T = Tables()
    rows = in_scope(T, scripts.called_builtins())
    C, edges = compare(T, rows)
    builtin_pairs = {(r["mp"], r["sp"]) for r in rows}
    tags = boundaries(C, edges, builtin_pairs)
    classify(rows, C, edges, tags)
    defs = port_functions()
    for r in rows:
        r["src"] = defs.get(r["cfunc"])
    write_status(rows)
    os.makedirs(OUT, exist_ok=True)
    json.dump(dict(rows=[dict(r, helpers=["%08x:%08x" % h for h in r["helpers"]]) for r in rows],
                   pairs={"%08x:%08x" % k: C.results[k].own for k in edges},
                   tags={"%08x:%08x" % k: v for k, v in tags.items()}),
              open(os.path.join(OUT, "triage.json"), "w"))
    return rows


def summary(rows):
    kept = read_status()
    t = collections.Counter(r["triage"] for r in rows)
    rv = collections.Counter(kept.get((r["lang"], r["kind"], r["name"]), ("", ""))[0] or "unread"
                             for r in rows if r["triage"] in ("number", "helper", "code"))
    lines = ["%d builtins the zombie maps' scripts call run a body from the MP exe (%d server, %d client)" % (
        len(rows), sum(r["lang"] == "gsc" for r in rows), sum(r["lang"] == "csc" for r in rows))]
    lines.append("the two exes' code:  same %d, layout %d  |  to read: number %d, helper %d, code %d%s" % (
        t["same"], t["layout"], t["number"], t["helper"], t["code"],
        ("  |  not in the SP exe: %d" % t["absent"]) if t["absent"] else ""))
    lines.append("of those to read:  " + ", ".join("%s %d" % (k, v) for k, v in sorted(rv.items())))
    return "\n".join(lines)
