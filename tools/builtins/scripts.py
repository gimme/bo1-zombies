r"""The zombie maps' scripts: read out of the fastfiles, and scanned for the builtins they call.

A call `name(...)` resolves to a builtin when it has no path (`a\b::name`), is not a thread call, and the calling
file defines no function of that name (Scr_GetBuiltin, src/clientscript/cscr_compiler.cpp). `obj name(...)` asks the
method tables, a bare `name(...)` the function tables. Developer blocks (/# #/) are dropped, as a retail compile
drops them.
"""
import os, re, json, zlib, struct, collections
from paths import GAME, OUT

MAPS = ["zombie_theater", "zombie_pentagon", "zombie_cosmodrome", "zombie_coast", "zombie_temple", "zombie_moon",
        "zombie_cod5_prototype", "zombie_cod5_asylum", "zombie_cod5_sumpf", "zombie_cod5_factory"]
SHARED = ["common_zombie_patch", "common_zombie", "patch", "code_post_gfx"]

_RAWFILE = re.compile(rb"\xff\xff\xff\xff(....)\xff\xff\xff\xff([\x20-\x7e]{3,120}\.(?:gsc|csc))\x00", re.S)


def zone_scripts(zone):
    """A fastfile's .gsc and .csc rawfiles: {name: text}. BO1's PC fastfiles are one zlib stream after 12 bytes;
    a script in it is {u32 length, u32 packed length, zlib}."""
    path = "%s/zone/Common/%s.ff" % (GAME, zone)
    if not os.path.exists(path):
        return {}
    d = open(path, "rb").read()
    if d[:8] not in (b"IWffu100", b"IWff0100"):
        raise SystemExit("%s: not a fastfile" % path)
    z = zlib.decompress(d[12:])
    out = {}
    for m in _RAWFILE.finditer(z):
        ln = struct.unpack("<I", m.group(1))[0]
        start = m.end()
        if ln == 0 or start + ln > len(z):
            continue
        body = z[start:start + ln]
        if ln > 8 and struct.unpack_from("<I", body, 4)[0] == ln - 8 and body[8:9] == b"\x78":
            body = zlib.decompress(body[8:])
        out[m.group(2).decode().replace("\\", "/").lower()] = body.decode("latin1")
    return out


TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<dev>/\#.*?\#/)
  | (?P<str>&?"(?:\\.|[^"\\])*")
  | (?P<num>(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<path>::)
  | (?P<op>\[\[|\]\]|[{}()\[\];,.:%\\]|[-+*/<>=!&|^~?\#]+)
""", re.S | re.X)

CONTROL = {"if", "while", "for", "switch"}
NOT_OBJECT = {"return", "else", "wait", "do", "case", "default", "thread", "childthread", "break", "continue"}
KEYWORDS = CONTROL | NOT_OBJECT | {"waittill", "waittillmatch", "waittillframeend", "notify", "endon", "breakpoint",
                                    "prof_begin", "prof_end", "true", "false", "undefined"}


def tokens(text):
    out = []
    pos = 0
    n = len(text)
    while pos < n:
        m = TOKEN.match(text, pos)
        if not m:
            pos += 1
            continue
        k = m.lastgroup
        if k not in ("ws", "lc", "bc", "dev"):
            out.append((k, m.group(0)))
        pos = m.end()
    return out


def scan(text):
    """-> (defined names, includes, [(kind, name)] builtin-candidate calls, far refs)"""
    toks = tokens(text)
    defined, includes, calls, far = set(), [], [], set()
    depth = 0
    paren = []  # stack of bool: is this '(' a control statement's
    last_close_control = False
    i = 0
    n = len(toks)
    while i < n:
        k, t = toks[i]
        if k == "op" and t == "#" and i + 1 < n and toks[i + 1][1] == "include":
            j = i + 2
            p = []
            while j < n and toks[j][1] != ";":
                p.append(toks[j][1])
                j += 1
            includes.append("".join(p).replace("\\", "/").lower())
            i = j + 1
            continue
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
        elif t == "(":
            prev = toks[i - 1][1].lower() if i else ""
            paren.append(prev in CONTROL)
        elif t == ")":
            last_close_control = paren.pop() if paren else False
            toks[i] = ("cparen_ctl" if last_close_control else "cparen", t)
        elif k == "id" and i + 1 < n and toks[i + 1][1] == "(":
            name = t.lower()
            pk, pt = toks[i - 1] if i else ("", "")
            if depth == 0 and not paren:
                # a definition: name(params) {
                j = i + 2
                d = 1
                while j < n and d:
                    if toks[j][1] == "(":
                        d += 1
                    elif toks[j][1] == ")":
                        d -= 1
                    j += 1
                if j < n and toks[j][1] == "{":
                    defined.add(name)
            elif name in KEYWORDS:
                pass
            elif pk == "path":
                # path::name( or ::name( -> a script call; remember the file a path names
                j = i - 2
                p = []
                while j >= 0 and (toks[j][0] == "id" or toks[j][1] == "\\"):
                    p.append(toks[j][1])
                    j -= 1
                if p:
                    far.add("".join(reversed(p)).replace("\\", "/").lower())
            elif pt.lower() in ("thread", "childthread"):
                pass
            else:
                is_method = (pk in ("id", "str", "num", "cparen") and pt.lower() not in NOT_OBJECT) or pt in ("]", "]]")
                calls.append(("method" if is_method else "function", name))
        elif k == "path":
            # function pointer reference path::name without a call
            j = i - 1
            p = []
            while j >= 0 and (toks[j][0] == "id" or toks[j][1] == "\\"):
                p.append(toks[j][1])
                j -= 1
            if p:
                far.add("".join(reversed(p)).replace("\\", "/").lower())
        i += 1
    return defined, includes, calls, far


def called_builtins(dump=None):
    """-> {"gsc": [{kind, name, calls, files}], "csc": [...]}: each unpathed, unthreaded call the maps' scripts make
    to a name their own file doesn't define, with the number of call sites. With dump, the scripts are written
    there too, a folder a zone."""
    cache = os.path.join(OUT, "calls.json")
    if os.path.exists(cache) and not dump:
        return json.load(open(cache))
    zones = {}
    for z in SHARED + [m + s for m in MAPS for s in ("_patch", "")]:
        zones[z] = zone_scripts(z)
        if dump:
            for name, text in zones[z].items():
                p = os.path.join(dump, z, name)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "w", encoding="latin1", newline="").write(text)
    scans = {}
    counts = {"gsc": collections.Counter(), "csc": collections.Counter()}
    sites = {"gsc": collections.defaultdict(set), "csc": collections.defaultdict(set)}
    counted = set()
    for m in MAPS:
        order = [m + "_patch", m] + SHARED          # a patch's script wins over the one it patches
        files = {}
        for z in order:
            for name in zones[z]:
                files.setdefault(name, z)
        # every script of the zombie zones, and what they include or name in the shared ones
        todo = [name for name, z in files.items() if z in order[:4]]
        reach = set()
        while todo:
            name = todo.pop()
            if name in reach or name not in files:
                continue
            reach.add(name)
            key = (files[name], name)
            if key not in scans:
                scans[key] = scan(zones[files[name]][name])
            defined, includes, calls, far = scans[key]
            todo += [ref + name[-4:] for ref in list(includes) + list(far)]
        for name in reach:
            key = (files[name], name)
            if key in counted:
                continue
            counted.add(key)
            defined, includes, calls, far = scans[key]
            for kind, nm in calls:
                if nm not in defined:
                    counts[name[-3:]][(kind, nm)] += 1
                    sites[name[-3:]][(kind, nm)].add(name)
    out = {lang: [dict(kind=k, name=nm, calls=c, files=sorted(sites[lang][(k, nm)])[:6])
                  for (k, nm), c in counts[lang].most_common()] for lang in counts}
    os.makedirs(OUT, exist_ok=True)
    json.dump(out, open(cache, "w"))
    return out
