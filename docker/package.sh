#!/usr/bin/env bash
# docker/package.sh - pack the two builds into the archive a zombies-hub server runs: build/bo1-zombies-<version>.tar.gz.
#
#   docker/package.sh [version]
#
# <version> names the archive. A release's is its tag, such as v2 (.github/workflows/release.yml). Without one it is
# the build ID.
#
# The archive contains:
#
#   engine.json   {"version": "<version>", "build": "<build ID>"}
#   bin/          BO1Zombies.exe and main/ (build/exe/Release)
#   web/          bo1zombies.js, bo1zombies.wasm, bo1zombies.map, xwma.js (build/web)
#   pipelines/    <map>.bin (web/pipelines)
#
# bin/ has what a server needs to run the exe under Wine, and no DLL: the exe loads binkw32.dll from the game files,
# and Wine has its own of the two DirectX DLLs that the build puts next to the exe for Windows.
#
# It fails unless both builds have the same build ID (make id). The last two lines of output are the version and the
# archive's SHA-256, which zombies-hub pins (internal/bo1zombies/bo1zombies.go).
set -euo pipefail
. "$(dirname "$0")/host.sh"
here=$(abs "$(dirname "$0")")
root=$(abs "$here/..")
bin=$root/build/exe/Release
web=$root/build/web

ids=$("$here/build-id.sh" "$bin/BO1Zombies.exe" "$web/bo1zombies.wasm") ||
    { echo "$ids" >&2; echo "package: the builds are missing or have different IDs (make build)" >&2; exit 1; }
id=$(echo "$ids" | sed -n '2s/ .*//p')
version=${1:-$id}
archive=$root/build/bo1-zombies-$version.tar.gz

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/bin" "$stage/web" "$stage/pipelines"
cp "$bin/BO1Zombies.exe" "$stage/bin/"
cp -R "$bin/main" "$stage/bin/"
find "$stage/bin" -name '*.log' -delete
cp "$web/bo1zombies.js" "$web/bo1zombies.wasm" "$web/bo1zombies.map" "$web/xwma.js" "$stage/web/"
cp "$root/web/pipelines/"*.bin "$stage/pipelines/"
printf '{"version": "%s", "build": "%s"}\n' "$version" "$id" >"$stage/engine.json"

# Sorted names, fixed times and no gzip timestamp: packaging the same builds again gives the same archive.
# To stdout: GNU tar takes a file name with a colon (C:/...) as a remote host.
tar -C "$stage" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -c engine.json bin web pipelines |
    gzip -n >"$archive"
echo "packaged $archive ($(du -h "$archive" | cut -f1))"
echo "version: $version"
echo "sha256: $(sha256sum "$archive" | cut -d' ' -f1)"
