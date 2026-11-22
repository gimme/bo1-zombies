#!/usr/bin/env bash
# docker/build-exe.sh - build the Windows game (BO1Zombies.exe, x86) on Linux: MSVC under Wine, in Docker.
#
#   docker/build-exe.sh [--config Release|Debug] [--clean] [-- extra cmake --build args, e.g. -j4 -v]
#
# Output: build/exe/<config>/BO1Zombies.exe (+ .pdb, the two DirectX DLLs, main/), the same layout as the Windows
# build (Play.bat), next to the build's CMake files; --clean deletes build/exe. ZOMBIES_BUILD_DIR=<dir> builds somewhere
# else. build/ is this folder's own: the engine's, wherever its checkout is.
# The first run builds the toolchain image (docker/Dockerfile, target msvc: downloads MSVC, the Windows SDK and
# the June 2010 DirectX SDK from Microsoft, accepting their license: ~5 min, ~1.5 GB download, 6.7 GB on disk).
set -euo pipefail
. "$(dirname "$0")/host.sh"
here=$(abs "$(dirname "$0")")
root=$(abs "$here/..")
out=${ZOMBIES_BUILD_DIR:-$root/build/exe}
image=${ZOMBIES_MSVC_IMAGE:-zombies-msvc:17}
config=Release clean=
while [ $# -gt 0 ]; do
    case $1 in
        --config) config=$2; shift 2 ;;
        --clean) clean=1; shift ;;
        --) shift; break ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

# The image is built the first time, and after that brought up to date with docker/Dockerfile (quietly: seconds
# when nothing changed).
progress=quiet; docker image inspect "$image" >/dev/null 2>&1 || progress=auto
docker build --progress=$progress -f "$here/Dockerfile" --target msvc -t "$image" \
    --build-arg UID="$uid" --build-arg GID="$gid" "$here" >/dev/null
[ -n "$clean" ] && rm -rf "$out"
mkdir -p "$out"
out=$(abs "$out")

# The engine is mounted read-only at /src and the build dir at /build, so the CMake cache does not depend on
# where the checkout lives on the host. The container has no git, so the build's ID (build_id.cmake) comes from here.
tty=; [ -t 1 ] && tty=-t
docker run --rm $tty --init \
    -v "$root:/src:ro" -v "$out:/build" -e BO1_BUILD_ID="$("$here/build-id.sh")" \
    "$image" bash /src/docker/container-build-exe.sh /src /build "$config" "$@"
echo "built $out/$config/BO1Zombies.exe"
