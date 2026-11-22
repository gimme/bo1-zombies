#!/usr/bin/env bash
# docker/build-id.sh - print the build's ID, as build_id.cmake works it out, for docker/build-exe.sh and
# docker/build-web.sh: their containers have no git, so they pass it in. It names the engine's source: the last commit
# that changed src/, tl/, DemonWare/, data/ or cmake_files.cmake, and when those have changes not committed, a hash of
# them (the diff from the commit, then each file git doesn't track, with its SHA-1). "unknown" outside a checkout.
#
#   docker/build-id.sh                  the source's ID
#   docker/build-id.sh <exe or wasm>... the source's ID, then each build's (make id): the ID its build wrote to
#                                       generated/bo1_build_id.h, if the file has it in it, else "?". Exits 1 unless
#                                       the builds are all of one ID, as a client of one can't join a server of another.
set -euo pipefail
# In Git Bash, C:/...: docker/host.sh sets MSYS_NO_PATHCONV for docker's sake, and with it git.exe gets a /c/... path
# as it is, finds no checkout there, and every build was "unknown".
root=$(cd "$(dirname "$0")/.." && if [ -n "${MSYSTEM:-}" ]; then pwd -W; else pwd; fi)
paths=(src tl DemonWare data cmake_files.cmake)
id=$(git -C "$root" log -1 --format=%h -- "${paths[@]}" 2>/dev/null) || id=
if [ -z "$id" ]; then
    echo unknown
    exit
fi
changes() {
    git -C "$root" diff HEAD --binary --no-ext-diff --no-textconv -- "${paths[@]}"
    git -C "$root" ls-files --others --exclude-standard -- "${paths[@]}" | while IFS= read -r file; do
        echo "$file $(sha1sum <"$root/$file" | cut -d' ' -f1)"
    done
}
if [ -n "$(changes | head -c 1)" ]; then
    id+=+$(changes | sha1sum | cut -c1-8)
fi
[ $# -gt 0 ] || { echo "$id"; exit; }

# The header sits in the build's directory, which is the exe's parent and the wasm's own. A build that failed after
# writing it leaves a file of the ID before, so the ID counts only when the file has it, as a string of its own.
built() {
    local header id
    [ -f "$1" ] || { echo "?: not built"; return; }
    for header in "$(dirname "$1")"/{.,..}/generated/bo1_build_id.h; do
        id=$(sed -n 's/^#define BO1_BUILD_ID "\(.*\)"$/\1/p' "$header" 2>/dev/null) || true
        if [ -n "$id" ] && grep -qaz -- "$id\$" "$1" 2>/dev/null; then echo "$id"; return; fi
    done
    echo "?: its last build didn't finish"
}
printf '%-18s %s\n' "$id" "the source"
ids=()
for file; do
    b=$(built "$file")
    printf '%-18s %s%s\n' "${b%%:*}" "$file" "${b#"${b%%:*}"}"
    ids+=("${b%%:*}")
done
[ "$(printf '%s\n' "${ids[@]}" | sort -u)" = "${ids[0]}" ] && [ "${ids[0]}" != "?" ]
