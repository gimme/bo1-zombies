#!/usr/bin/env bash
# Runs inside the zombies-msvc image (docker/Dockerfile, target msvc): configures and builds BO1Zombies.exe with
# MSVC (cl/link under Wine) + CMake + Ninja. Called by docker/build-exe.sh.
#
#   container-build-exe.sh <source dir> <build dir> [Release|Debug] [extra cmake --build args...]
#
# "Ninja Multi-Config" puts the exe in <build dir>/<config>/ like the Visual Studio generator does, so the
# CMakeLists' $<CONFIG> copy steps (data/main/, the DLLs) and Play.bat's build\exe\Release
# layout match a Windows build.
set -euo pipefail
src=$1 out=$2 config=${3:-Release}
shift $(( $# < 3 ? $# : 3 ))

# link.exe under Wine keeps several file descriptors per object file (Wine's file handles + mappings, in the
# wineserver too); Docker's default soft limit of 1024 runs out a few hundred objects in (LNK1104 cannot open file).
ulimit -n "$(ulimit -Hn)"
# One wineserver for the whole build instead of one per cl.exe/link.exe run.
wineserver -p
trap 'wineserver -k 2>/dev/null || true' EXIT

# CMAKE_SYSTEM_NAME=Windows: cross-compiling, so CMake takes the MSVC rules (cl/link) from PATH
# (/opt/msvc/bin/x86: the x64-hosted, x86-targeting cl.exe/link.exe with the x86 INCLUDE/LIB of the
# Windows SDK, i.e. what vcvarsamd64_x86.bat / "-A Win32" gives on Windows).
# CMAKE_NINJA_FORCE_RESPONSE_FILE: on a Linux host CMake would put all ~1250 objects on link.exe's command line,
# past Windows' 32K command-line limit (Wine then cannot start link.exe and the build hangs). It goes in the
# environment, which CMake reads too: as a -D, CMake used it but warned on every configure that it hadn't.
# Configuring every time is cheap (a few seconds) and picks up changes to these settings, the build's ID among them
# (BO1_BUILD_ID: docker/build-exe.sh works it out, build_id.cmake).
CMAKE_NINJA_FORCE_RESPONSE_FILE=1 CC=cl CXX=cl cmake -S "$src" -B "$out" -G "Ninja Multi-Config" \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=X86 \
    -DCMAKE_CONFIGURATION_TYPES="Release;Debug" \
    -DDXSDK_DIR="${DXSDK_DIR:-/opt/dxsdk}" -DBO1_BUILD_ID="${BO1_BUILD_ID:-}"
cmake --build "$out" --config "$config" "$@"
