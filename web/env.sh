# source web/env.sh: Emscripten + the CMake/Ninja that ship with Visual Studio, for building web/ from Git Bash
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
for vs in "/c/Program Files/Microsoft Visual Studio"/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake; do
    [ -d "$vs" ] && export PATH="$PATH:$vs/CMake/bin:$vs/Ninja"
done
