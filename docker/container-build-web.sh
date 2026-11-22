#!/usr/bin/env bash
# Runs inside the zombies-web image (docker/Dockerfile, target "web"): the Linux side of web/build.ps1.
#
#   container-build-web.sh <source dir> <build dir> [ON|OFF (AddressSanitizer)] [extra cmake --build args...]
set -euo pipefail
src=$1 out=$2 asan=${3:-OFF}
shift $(( $# < 3 ? $# : 3 ))
mkdir -p "$out"
dxsdk=${DXSDK_DIR:-/opt/dxsdk}

# Case-insensitive includes on a case-sensitive file system. The sources spell some headers differently from the files
# (<Windows.h> for web/compat/windows.h, <XInput.h> for web/compat/xinput.h, "types.h" for CubeMapGenLib/Types.h).
# Windows takes the first case-insensitive match along the include path; Linux takes the first exact one, which for
# <XInput.h> is the DirectX SDK's own XInput.h instead of web/compat's stand-in. So the build reads the DirectX SDK
# through an overlay directory (-DDXSDK_DIR, the last directory web/CMakeLists.txt searches) that:
#   - links every DXSDK header, except those web/compat replaces (Windows finds the compat one first anyway), and
#   - adds each spelling the sources use whose Windows match has another case, linked to that match.
# The include path below must follow web/CMakeLists.txt's target_include_directories. The overlay is in the
# container's own file system, not the build dir: on a Windows folder (docker/build-web.sh in Git Bash) a name matches
# whatever its case, and the DXSDK has headers whose names differ only in case (D3Dcompiler.h, d3dcompiler.h).
overlay=/tmp/dxsdk-case-overlay
rm -rf "$overlay" && mkdir -p "$overlay/Include"
python3 -I - "$src" "$overlay/Include" "$dxsdk/Include" <<'EOF'
import os, re, sys
src, overlay, dxinc = sys.argv[1:4]
compat = os.path.join(src, "web/compat")
path = [compat] + [os.path.join(src, d) for d in ("", "src", "src/libs", "src/libs/libtomcrypt-1.17/src/headers",
        "src/libs/libtommath-1.0", "src/libs/libvpx-1.5.0/include")] + [dxinc]

listing = {}
def ls(d):
    """{lowercase name: name} of d's entries; an exact-case entry wins over a lowercase alias of it."""
    if d not in listing:
        try:
            names = sorted(os.listdir(d), key=lambda e: e == e.lower())
        except OSError:
            names = []
        listing[d] = {}
        for e in names:
            listing[d].setdefault(e.lower(), e)
    return listing[d]

def find_ci(base, name):
    """(path, exact) of the case-insensitive match of name under base, or (None, False)."""
    cur, exact = base, True
    for part in name.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            cur = os.path.dirname(cur)
            continue
        hit = part if os.path.lexists(os.path.join(cur, part)) else ls(cur).get(part.lower())
        if hit is None:
            return None, False
        exact &= hit == part
        cur = os.path.join(cur, hit)
    return cur, exact

compat_names = set(ls(compat))
for e in os.listdir(dxinc):
    if e.lower() not in compat_names:
        os.symlink(os.path.join(dxinc, e), os.path.join(overlay, e))

inc = re.compile(rb'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
aliases = {}
for top in [os.path.join(src, t) for t in ("src", "tl", "DemonWare", "web")] + [dxinc]:
    for dp, _, fn in os.walk(top):
        for f in fn:
            if not f.lower().endswith((".c", ".cpp", ".h", ".hpp", ".inl")):
                continue
            for kind, name in inc.findall(open(os.path.join(dp, f), "rb").read()):
                name = name.decode("latin-1")
                for d in ([dp] if kind == b'"' else []) + path:
                    hit, exact = find_ci(d, name)
                    if hit:
                        if not exact:
                            aliases[name] = hit
                        break
for name, target in sorted(aliases.items()):
    link = os.path.join(overlay, name)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(target, link)
    print(f"case alias: {name} -> {target}")
EOF

# Configuring every time is cheap and picks up changes to these settings (as web/build.ps1 does), the build's ID
# among them (BO1_BUILD_ID: docker/build-web.sh works it out, build_id.cmake). FFMPEG_DIR: the image's FFmpeg, whose
# Bink decoders play the game's movies (web/src/bink_web.cpp).
emcmake cmake -S "$src/web" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBO1_WEB_ASAN="$asan" \
    -DDXSDK_DIR="$overlay" -DBO1_BUILD_ID="${BO1_BUILD_ID:-}" -DFFMPEG_DIR="${FFMPEG_DIR:-}"
cmake --build "$out" "$@"
