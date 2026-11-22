# WebAssembly build of the engine (Emscripten), played in Chrome.
#
#   web\build.ps1            configure + build into build\web
#
# The engine sources are the same list the Windows build compiles (cmake_files.cmake); the Windows-only parts
# (window, input, sound device, sockets, minidumps, Bink, Steam) are left out and web/src/ provides them
# on top of the browser. web/compat/ stands in for the Windows and DirectX SDK headers: it is searched before the
# June 2010 DirectX SDK, whose d3d9.h types and constants the Direct3D 9 device (web/src/d3d9_device.cpp) and its
# WebGPU executor use.
cmake_minimum_required(VERSION 3.16)
project(BO1ZombiesWeb C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "web/ builds with Emscripten: use web\\build.ps1 (emcmake cmake)")
endif()

set(BIN_NAME "bo1zombies")
set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SRC_DIR "${ROOT_DIR}/src")
set(TL_DIR  "${ROOT_DIR}/tl")
set(DW_DIR  "${ROOT_DIR}/DemonWare")
set(WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXECUTABLE_SUFFIX ".js")

include("${ROOT_DIR}/cmake_files.cmake")
include("${ROOT_DIR}/build_id.cmake")

# ----- Sources: the Windows list minus what the browser replaces -----
get_target_property(ENGINE_SOURCES ${BIN_NAME} SOURCES)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "\\.h$|\\.hpp$|\\.inl$")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/binklib/|/src/CubeMapGenLib/|/src/mjpeg/|/src/tracy/")
# win_main (WinMain, crash handling, headless tooling), the console window, minidumps and the voice mixer are
# replaced by web/src; the window procedure, mouse, gamepad and net code stay and run on emulated Win32, with UDP on
# WebTransport (web/src/web_net.cpp).
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/win32/(win_main|win_mini_dumper|win_voice|win_syscon)\\.cpp$")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/groupvoice/(play_dsound|record_dsound)\\.cpp$")
# MMX skinning; sys_SSE is off on web, so the engine's scalar skinner runs instead
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/gfx_d3d/r_model_skin_sse\\.cpp$")

file(GLOB WEB_SOURCES CONFIGURE_DEPENDS "${WEB_DIR}/src/*.cpp" "${WEB_DIR}/src/*.c")
# the page's decoder (xwma.js, below), not the engine's
list(FILTER WEB_SOURCES EXCLUDE REGEX "/xwma_page\\.cpp$")

set_property(TARGET ${BIN_NAME} PROPERTY SOURCES ${ENGINE_SOURCES} ${WEB_SOURCES})

# The build's own data files, the base data files the install lacks (data/main/: playlists_sp.info), in the module, as
# the Windows build puts them next to the exe: at BO1_WEB_BUILD_ROOT/main, which the engine searches as its exe's
# folder (src/win32/win_common.cpp: Sys_DefaultCDPath), apart from the install (/game), which the browser's store holds
set(WEB_BUILD_ROOT "/bo1")
file(GLOB BASE_DATA_FILES "${ROOT_DIR}/data/main/*")

# ----- Preprocessor -----
# WIN32 stays defined: the engine's shared headers keep PITCH/YAW/ROLL, byte order and such under it. _WIN32 is not,
# so the bundled libraries (zlib, libtomcrypt, jpeg, speex) take their portable paths.
target_compile_definitions(${BIN_NAME} PUBLIC
    WIN32
    _CONSOLE
    BO1_MP
    BO1_WEB
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_NO_DEPRECATED_NO_WARNINGS
    NDEBUG
    LZO_ARCH_GENERIC=1 # minilzo guesses the CPU from the OS when WIN32 is defined; there is no x86 here
    BO1_WEB_BUILD_ROOT="${WEB_BUILD_ROOT}"
)

# ----- Include directories -----
# DXSDK_DIR: the June 2010 DirectX SDK (only its d3d9*.h are read; nothing links against it)
set(DXSDK_DIR "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)" CACHE PATH "June 2010 DirectX SDK root")
target_include_directories(${BIN_NAME} BEFORE PUBLIC "${WEB_DIR}/compat")
target_include_directories(${BIN_NAME} PUBLIC
    "${ROOT_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/libs"
    "${SRC_DIR}/libs/libtomcrypt-1.17/src/headers"
    "${SRC_DIR}/libs/libtommath-1.0"
    "${SRC_DIR}/libs/libvpx-1.5.0/include"
)
target_include_directories(${BIN_NAME} AFTER PUBLIC "${DXSDK_DIR}/Include")

# ----- Compiler -----
# -fms-extensions/-fms-compatibility: the decompiled code is MSVC C++ (/permissive). Wasm exceptions back
# setjmp/longjmp (Com_Error) without JS frames, which JSPI cannot suspend through.
set(WEB_COMMON_FLAGS
    -pthread
    -fms-extensions -fms-compatibility -fms-compatibility-version=19.40 -fdeclspec
    "SHELL:-include ${WEB_DIR}/compat/bo1_web_prelude.h"
    -fno-strict-aliasing -fwrapv
    -msimd128 -msse -msse2
    -fwasm-exceptions -sSUPPORT_LONGJMP=wasm
    -Wno-everything
)
target_compile_options(${BIN_NAME} PRIVATE ${WEB_COMMON_FLAGS})

# BO1_WEB_ASAN: an AddressSanitizer build for hunting memory corruption (slower, more memory)
option(BO1_WEB_ASAN "Build with AddressSanitizer" OFF)
set(WEB_MEMORY 2560MB)
if(BO1_WEB_ASAN)
    target_compile_options(${BIN_NAME} PRIVATE -fsanitize=address -fsanitize-recover=address)
    target_link_options(${BIN_NAME} PRIVATE -fsanitize=address -sSTACK_OVERFLOW_CHECK=2)
    set(WEB_MEMORY 3200MB)
endif()

# ----- Linker -----
target_link_options(${BIN_NAME} PRIVATE
    -pthread
    -fwasm-exceptions -sSUPPORT_LONGJMP=wasm
    -sPROXY_TO_PTHREAD
    -sWASMFS
    -sENVIRONMENT=web,worker -sEXIT_RUNTIME=1
    -sINITIAL_MEMORY=${WEB_MEMORY} -sALLOW_MEMORY_GROWTH=0
    -sSTACK_SIZE=16MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB
    -sPTHREAD_POOL_SIZE=24
    -sFORCE_FILESYSTEM=1
    -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$stringToNewUTF8,$UTF8ToString,$bo1Stop
    # the engine is a function (bo1Engine), which the page calls for each game, ending it after (library_bo1.js:
    # bo1Stop): the page stays. It may hand the engine the last one's memory (Module.wasmMemory), which Emscripten takes
    # only when its list of what it reads from Module names it: its default list, with wasmMemory.
    -sMODULARIZE=1 -sEXPORT_NAME=bo1Engine
    "-sINCOMING_MODULE_JS_API=[ENVIRONMENT,arguments,canvas,dynamicLibraries,elementPointerLock,instantiateWasm,\
locateFile,monitorRunDependencies,noExitRuntime,noInitialRun,onAbort,onExit,onRuntimeInitialized,postRun,preInit,\
preRun,print,printErr,setStatus,statusMessage,stderr,stdin,stdout,thisProgram,wasm,websocket,wasmMemory]"
    -sEXPORTED_RUNTIME_METHODS=[ccall,cwrap,UTF8ToString,stringToUTF8,HEAPU8,HEAP32]
    -sERROR_ON_UNDEFINED_SYMBOLS=1 -Wl,--error-limit=0
    -lhtml5
    --profiling-funcs
    -Wl,-Map=bo1zombies.map
    "--js-library=${WEB_DIR}/src/library_bo1.js"
    "--js-library=${WEB_DIR}/src/library_gpu.js"
    "SHELL:--embed-file ${ROOT_DIR}/data/main@${WEB_BUILD_ROOT}/main"
)
set_property(TARGET ${BIN_NAME} APPEND PROPERTY LINK_DEPENDS "${WEB_DIR}/src/library_bo1.js" "${WEB_DIR}/src/library_gpu.js"
    ${BASE_DATA_FILES})


# ----- Bink movies: FFmpeg's decoders (web/src/bink_web.cpp) -----
# FFMPEG_DIR: an FFmpeg built for Emscripten with only its Bink video and audio decoders and Bink demuxer (LGPL), as
# docker/Dockerfile's web target builds it to /opt/ffmpeg, and docker/container-build-web.sh passes it (the environment's
# FFMPEG_DIR otherwise). Without one the Bink stubs stay (web/src/web_stubs.cpp): the maps' loading movies and Call of
# the Dead's outro don't play, and their screens stay still.
set(FFMPEG_DIR "$ENV{FFMPEG_DIR}" CACHE PATH "FFmpeg with the Bink decoders, built for Emscripten (lib/libavcodec.a ...)")
if(FFMPEG_DIR AND EXISTS "${FFMPEG_DIR}/lib/libavcodec.a" AND EXISTS "${FFMPEG_DIR}/lib/libavformat.a"
   AND EXISTS "${FFMPEG_DIR}/lib/libavutil.a")
    target_compile_definitions(${BIN_NAME} PRIVATE BO1_WEB_BINK=1)
    target_include_directories(${BIN_NAME} PRIVATE "${FFMPEG_DIR}/include")
    target_link_libraries(${BIN_NAME} PRIVATE "${FFMPEG_DIR}/lib/libavformat.a" "${FFMPEG_DIR}/lib/libavcodec.a"
        "${FFMPEG_DIR}/lib/libavutil.a")
    message(STATUS "Bink movies play with FFmpeg's decoders from ${FFMPEG_DIR}")
else()
    message(WARNING "No FFmpeg for Emscripten (FFMPEG_DIR): the web client plays no movies (docker/build-web.sh builds one into its image)")
endif()

# ----- The page's xWMA decoder: xwma.js -----
# The engine's decoder (web/src/xwma.cpp) on its own, for the lobby's page, which plays the game's menu sounds
# (zombies-hub's web/src/sounds.ts) as the server sends them (zombies-hub's server/sounds.go: GET /sounds), xWMA as the
# zone keeps them, which no browser decodes. One script with its module inside (SINGLE_FILE), a few dozen KB, which the
# page loads as it starts:
# a factory, bo1Xwma(), whose module has bo1_xwma_decode (web/src/xwma_page.cpp).
add_executable(xwma "${WEB_DIR}/src/xwma.cpp" "${WEB_DIR}/src/xwma_page.cpp")
target_compile_options(xwma PRIVATE -O2)
target_link_options(xwma PRIVATE
    -O2
    -sMODULARIZE=1 -sEXPORT_NAME=bo1Xwma -sSINGLE_FILE=1
    -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_FUNCTIONS=[_bo1_xwma_decode,_malloc,_free]
    -sEXPORTED_RUNTIME_METHODS=[HEAPU8,HEAP16]
)
