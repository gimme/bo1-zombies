#!/usr/bin/env bash
# docker/build-web.sh - build the WebAssembly version (web/) on Linux, in Docker: the Linux side of
# web/build.ps1, with the emsdk version it is tested with.
#
#   docker/build-web.sh [--asan] [--clean] [-- extra cmake --build args, e.g. -j4 -v]
#
# Output: build/web (build/web-asan with --asan), as web/build.ps1; ZOMBIES_WEB_BUILD_DIR=<dir> builds elsewhere.
# The first run builds the image (docker/Dockerfile, target web: emscripten/emsdk + the June 2010 DirectX SDK
# headers).
set -euo pipefail
. "$(dirname "$0")/host.sh"
here=$(abs "$(dirname "$0")")
root=$(abs "$here/..")
emsdk=${EMSDK_VERSION:-6.0.13}
image=${ZOMBIES_WEB_IMAGE:-zombies-web:$emsdk}
asan=OFF clean=
while [ $# -gt 0 ]; do
    case $1 in
        --asan) asan=ON; shift ;;
        --clean) clean=1; shift ;;
        --) shift; break ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
out=${ZOMBIES_WEB_BUILD_DIR:-$root/build/$([ $asan = ON ] && echo web-asan || echo web)}

# The image is built the first time, and after that brought up to date with docker/Dockerfile (quietly: seconds
# when nothing changed).
progress=quiet; docker image inspect "$image" >/dev/null 2>&1 || progress=auto
docker build --progress=$progress -f "$here/Dockerfile" --target web -t "$image" \
    --build-arg EMSDK_VERSION="$emsdk" "$here" >/dev/null
[ -n "$clean" ] && rm -rf "$out"
mkdir -p "$out"
out=$(abs "$out")

tty=; [ -t 1 ] && tty=-t
# Emscripten's cache (system libraries it builds for this project's flags: pthreads, wasm exceptions, WASMFS, ...)
# lives in a named volume, which Docker seeds from the image's own cache the first time, so it survives --rm. The
# build's ID (build_id.cmake) comes from here, as the container can't read the checkout's git.
docker run --rm $tty --init -u "$uid:$gid" \
    -v "$root:/src:ro" -v "$out:/build" -v "zombies-emcache-$emsdk:/emsdk/upstream/emscripten/cache" \
    -e BO1_BUILD_ID="$("$here/build-id.sh")" \
    "$image" bash /src/docker/container-build-web.sh /src /build "$asan" "$@"
echo "built $out/bo1zombies.wasm"
