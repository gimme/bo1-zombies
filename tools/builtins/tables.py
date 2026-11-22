"""The script builtin tables: the port's, parsed from its source, and each retail exe's, found in its data.

A table is rows of {name, function, developer flag}. A name is looked up through the tables in a fixed order and
the first match wins, so the order is part of what a name means; the two exes' orders differ.
"""
import os, re, struct, json
from exe import PE
from paths import SRC, OUT, MP_EXE, SP_EXE

# The port's tables: key, file, array, kind, where its bodies came from.
PORT_TABLES = [
    ("functions",        "game_mp/g_scr_main_mp.cpp",          "functions",                 "function",  "mp"),
    ("player",           "game_mp/g_client_script_cmd_mp.cpp", "methods",                   "method",    "mp"),
    ("scriptent",        "game/g_scr_mover.cpp",               "methods_1",                 "method",    "mp"),
    ("vehicle",          "game/g_scr_vehicle.cpp",             "s_methods_0",               "method",    "mp"),
    ("hudelem",          "game/g_hudelem.cpp",                 "methods_0",                 "method",    "mp"),
    ("helicopter",       "game/g_scr_helicopter.cpp",          "s_methods",                 "method",    "mp"),
    ("actor",            "game/actor_script_cmd.cpp",          "methods_2",                 "method",    "mp"),
    ("builtin",          "game_mp/g_scr_main_mp.cpp",          "methods_3",                 "method",    "mp"),
    ("sp_players_f",     "game_sp/g_scr_sp_players.cpp",       "g_sp_players_functions",    "function",  "sp"),
    ("sp_players_m",     "game_sp/g_scr_sp_players.cpp",       "g_sp_players_methods",      "method",    "sp"),
    ("sp_anim_f",        "game_sp/g_scr_sp_anim.cpp",          "g_sp_anim_functions",       "function",  "sp"),
    ("sp_anim_m",        "game_sp/g_scr_sp_anim.cpp",          "g_sp_anim_methods",         "method",    "sp"),
    ("sp_entity_f",      "game_sp/g_scr_sp_entity.cpp",        "g_sp_entity_functions",     "function",  "sp"),
    ("sp_entity_m",      "game_sp/g_scr_sp_entity.cpp",        "g_sp_entity_methods",       "method",    "sp"),
    ("sp_ai_f",          "game_sp/g_scr_sp_ai.cpp",            "g_sp_ai_functions",         "function",  "sp"),
    ("sp_ai_m",          "game_sp/g_scr_sp_ai.cpp",            "g_sp_ai_methods",           "method",    "sp"),
    ("sp_notported_f",   "game_sp/g_scr_sp_notported.cpp",     "g_sp_notported_functions",  "function",  "stub"),
    ("sp_notported_m",   "game_sp/g_scr_sp_notported.cpp",     "g_sp_notported_methods",    "method",    "stub"),
    ("c_functions",      "cgame/cg_scr_main.cpp",              "client_functions",          "cfunction", "mp"),
    ("c_methods",        "cgame/cg_scr_main.cpp",              "client_methods",            "cmethod",   "mp"),
    ("c_proj_functions", "cgame_mp/cg_scr_main_mp.cpp",        "client_project_functions",  "cfunction", "mp"),
    ("c_proj_methods",   "cgame_mp/cg_scr_main_mp.cpp",        "client_project_methods",    "cmethod",   "mp"),
    ("csp_client_f",     "cgame/cg_scr_sp_client.cpp",         "g_csp_client_functions",    "cfunction", "sp"),
    ("csp_client_m",     "cgame/cg_scr_sp_client.cpp",         "g_csp_client_methods",      "cmethod",   "sp"),
    ("csp_notported_f",  "cgame/cg_scr_sp_notported.cpp",      "g_csp_notported_functions", "cfunction", "stub"),
    ("csp_notported_m",  "cgame/cg_scr_sp_notported.cpp",      "g_csp_notported_methods",   "cmethod",   "stub"),
]

# The order the port looks a name up in (Scr_GetFunction, Scr_GetMethod, CScr_GetFunction, CScr_GetMethod).
PORT_ORDER = {
    "function":  ["functions", "sp_players_f", "sp_anim_f", "sp_entity_f", "sp_ai_f", "sp_notported_f"],
    "method":    ["player", "scriptent", "vehicle", "hudelem", "helicopter", "actor", "builtin",
                  "sp_players_m", "sp_anim_m", "sp_entity_m", "sp_ai_m", "sp_notported_m"],
    "cfunction": ["c_functions", "c_proj_functions", "csp_client_f", "csp_notported_f"],
    "cmethod":   ["c_methods", "c_proj_methods", "csp_client_m", "csp_notported_m"],
}
# Names the port takes from its MP tables off a zombie map only (Scr_GetFunction, Scr_GetMethod).
PORT_ZOMBIE_ABSENT = {"function": {"spawn", "spawnfx", "playloopedfx", "spawnvehicle", "spawnturret"},
                      "method": {"setinvisibletoall"}}
PORT_MP_COUNT = {"function": 1, "method": 7}

# The SP exe's order, read from its lookups: Scr_GetFunction 0x0052BF80, Scr_GetMethod 0x0068A640, the client's
# 0x005A50E0 and 0x004B91E0. Each entry is the table's address in BlackOps.exe.
SP_ORDER = {
    "function":  [("threatbias", 0x00A55E80), ("functions", 0x00B75EE0)],
    "method":    [("actor", 0x00A52058), ("sentient", 0x00A55E40), ("player", 0x00A52A80), ("scriptent", 0x00A54FC0),
                  ("vehicle", 0x00A551D8), ("hudelem", 0x00A535C8), ("builtin", 0x00A54218)],
    "cfunction": [("c_functions", 0x00B71DE0), ("c_proj_functions", 0x00B84988)],
    "cmethod":   [("c_methods", 0x00A4E7B8), ("c_proj_methods", 0x00A606E8)],
}

_ROW = re.compile(r'\{\s*"([^"]+)"\s*,\s*&?\s*([A-Za-z_0-9:]+)\s*,\s*([0-9]+)\s*\}')
_NAME = re.compile(rb"^[a-z_][a-z0-9_]*$")


def port_tables():
    """-> {key: {file, kind, origin, rows: [(name, C function, developer)]}}"""
    out = {}
    for key, f, arr, kind, origin in PORT_TABLES:
        text = open(os.path.join(SRC, f), encoding="utf-8", errors="replace").read()
        m = re.search(r"\b" + re.escape(arr) + r"\s*\[[^\]]*\]\s*=\s*\{", text)
        if not m:
            raise SystemExit("no table %s in src/%s" % (arr, f))
        i = j = m.end()
        depth = 1
        while depth:
            depth += {"{": 1, "}": -1}.get(text[j], 0)
            j += 1
        body = re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text[i:j - 1], flags=re.S))
        out[key] = dict(file=f, kind=kind, origin=origin,
                        rows=[(n, fn, int(t)) for n, fn, t in _ROW.findall(body) if fn != "nullptr"])
    return out


def exe_tables(path, minrows=4):
    """Runs of {char *name; void (*func)(); int developer;} rows in an exe's .rdata and .data."""
    cache = os.path.join(OUT, os.path.basename(path) + ".tables.json")
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(path):
        return json.load(open(cache))
    pe = PE(path)
    out = []
    for s in pe.sections:
        if s["name"] not in (".rdata", ".data"):
            continue
        base, n = s["va"], min(s["rsize"], s["vsize"])
        raw = pe.data[s["roff"]:s["roff"] + n]

        def row(o):
            if o + 12 > n:
                return None
            a, f, t = struct.unpack_from("<III", raw, o)
            if t > 1 or not pe.is_code(f):
                return None
            sec = pe.sec(a)
            if not sec or sec["exec"]:
                return None
            nm = pe.cstr(a, 80)
            if not nm or not _NAME.match(nm):
                return None
            return nm.decode(), f, t

        o = 0
        while o + 12 <= n:
            rows = []
            p = o
            while True:
                r = row(p)
                if not r:
                    break
                rows.append(r)
                p += 12
            if len(rows) >= minrows:
                out.append(dict(va=base + o, rows=rows))
                o = p
            else:
                o += 4
    os.makedirs(OUT, exist_ok=True)
    json.dump(out, open(cache, "w"))
    return out


class Tables:
    def __init__(self):
        self.port = port_tables()
        self.mp = exe_tables(MP_EXE)
        self.sp = exe_tables(SP_EXE)
        self.sp_by_va = {t["va"]: t for t in self.sp}
        for kind, order in SP_ORDER.items():
            for name, va in order:
                if va not in self.sp_by_va:
                    raise SystemExit("BlackOps.exe has no builtin table at %08x (%s): another build of the exe?" % (va, name))
        # the MP exe's table for each of the port's MP tables: the one with the same names in the same order
        self.mp_of = {}
        for key, t in self.port.items():
            if t["origin"] != "mp":
                continue
            names = [r[0] for r in t["rows"]]
            for cand in self.mp:
                tn = [r[0] for r in cand["rows"]]
                k = min(len(tn), len(names), 20)
                if tn[:k] == names[:k]:
                    self.mp_of[key] = cand
            if key not in self.mp_of:
                raise SystemExit("BlackOpsMP.exe has no table like the port's %s" % key)

    def port_lookup(self, kind, name):
        """-> (table key, C function, developer, origin) as a zombie map's compile resolves the name"""
        order = PORT_ORDER[kind]
        if name in PORT_ZOMBIE_ABSENT.get(kind, ()):
            order = order[PORT_MP_COUNT[kind]:]
        for key in order:
            for n, fn, dev in self.port[key]["rows"]:
                if n == name:
                    return key, fn, dev, self.port[key]["origin"]
        return None

    def mp_lookup(self, key, name):
        for n, f, dev in self.mp_of[key]["rows"]:
            if n == name:
                return f
        return None

    def sp_lookup(self, kind, name):
        """-> [(table, address, developer)] in the SP exe's order; the first is the one a script gets"""
        hits = []
        for tname, va in SP_ORDER[kind]:
            for n, f, dev in self.sp_by_va[va]["rows"]:
                if n == name:
                    hits.append((tname, f, dev))
                    break
        return hits

    def exe_names(self):
        """address -> script name, for each exe"""
        out = {"mp": {}, "sp": {}}
        for side, tabs in (("mp", self.mp), ("sp", self.sp)):
            for t in tabs:
                for n, f, dev in t["rows"]:
                    out[side].setdefault(f, "script:" + n)
        return out
