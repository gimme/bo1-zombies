cmake_minimum_required(VERSION 3.16)
project(BO1Zombies)

set(BIN_NAME "BO1Zombies")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_GENERATOR_PLATFORM "WIN32")

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TL_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/tl")
set(DW_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/DemonWare")

include(cmake_files.cmake)
include(build_id.cmake)

# phys_gjk.cpp specific flag (experimental)
set_source_files_properties(
    "${SRC_DIR}/physics/phys_gjk.cpp"
    PROPERTIES
    COMPILE_OPTIONS "/arch:IA32"
)

# ----- Preprocessor Defines -----
# Common to both Debug and Release
target_compile_definitions(BO1Zombies PUBLIC
    WIN32
    _CONSOLE
    BO1_MP
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_NO_DEPRECATED_NO_WARNINGS
    # the Steamworks functions are the stand-ins in src/win32/win_steam_none.cpp, not steam_api.dll
    STEAM_API_NODLL
)
# Debug-only
target_compile_definitions(BO1Zombies PUBLIC
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Debug>:TRACY_ENABLE>
    $<$<CONFIG:Debug>:TRACY_ON_DEMAND>
)
# Release-only
target_compile_definitions(BO1Zombies PUBLIC
    $<$<CONFIG:Release>:NDEBUG>
)

# ----- Include directories -----
target_include_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/libs")
target_include_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/libs/libtomcrypt-1.17/src/headers")
target_include_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/libs/libtommath-1.0")
target_include_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/libs/libvpx-1.5.0/include")
# DXSDK_DIR: the June 2010 DirectX SDK root (Include/, Lib/x86/). Defaults to its installer's path;
# pass -DDXSDK_DIR=<dir> to use an extracted copy instead.
set(DXSDK_DIR "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)" CACHE PATH "June 2010 DirectX SDK root")
target_include_directories(BO1Zombies PUBLIC "${DXSDK_DIR}/Include")

# ----- Library directories -----
target_link_directories(BO1Zombies PUBLIC "${DXSDK_DIR}/Lib/x86")
target_link_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/libs/libvpx-1.5.0")
target_link_directories(BO1Zombies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/binklib")

# The static C/C++ runtime (/MT, /MTd). /MT in the flags alone lost to CMake's own /MD (policy CMP0091), so the exe
# needed msvcp140.dll and vcruntime140.dll: Windows players had to have them, and Wine's msvcp140 lacks what VS 17.10+
# compiles std::mutex against (the server aborted in _Throw_Cpp_error).
set_property(TARGET BO1Zombies PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The prebuilt libvpx.lib (src/mjpeg/avi.cpp's video capture) was built for the DLL runtime and asks for it: leave that
# out, so its calls resolve to the static one (LNK4217 / LNK4286 say so for each of them). _beginthreadex is the one
# nothing else pulls in. Its objects name a libvpx.pdb that isn't shipped (LNK4099).
target_link_options(BO1Zombies PRIVATE /NODEFAULTLIB:msvcrt.lib /INCLUDE:__beginthreadex /IGNORE:4217,4286,4099)
# Warnings the decompiled code gives by the thousand, none of which has turned out to mean anything: conversions that
# may lose data (C4244, C4267, C4302, C4305, C4309, C4838), unused locals and labels (C4101, C4102), float constants
# that overflow (C4056), signed/unsigned comparisons (C4018), a bool mixed with an int (C4805), alignment specifiers
# that are ignored (C4359), enum values past an int (C4369) and class/struct mismatches, the DirectX SDK's among them
# (C4099). The rest stay on, as they have each found a real bug: format strings, missing returns, endless recursion,
# a bool compared with a number (C4804), redefined macros (C4005: Windows' HIBYTE in place of the decompiler's).
set(MSVC_WARNING_DISABLES "/wd4244 /wd4267 /wd4302 /wd4305 /wd4309 /wd4838 /wd4101 /wd4102 /wd4056 /wd4018 /wd4805 /wd4359 /wd4369 /wd4099")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /MP /W3 ${MSVC_WARNING_DISABLES} /permissive")
set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG} /Od /MP /W3 ${MSVC_WARNING_DISABLES} /permissive")
# /std:c++20 (CMAKE_CXX_STANDARD) implies /permissive-. The Visual Studio generator turns the /permissive above into
# ConformanceMode=false, but Ninja (docker/) puts -std:c++20 after CMAKE_CXX_FLAGS_*, so repeat it after that.
target_compile_options(BO1Zombies PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/permissive>)

if (MSVC AND WIN32 AND NOT MSVC_VERSION VERSION_LESS 142)
    target_link_options(BO1Zombies PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
    target_compile_options(BO1Zombies PRIVATE $<$<CONFIG:Debug>:/ZI>)

    set(CMAKE_CXX_FLAGS_DEBUG           "${CMAKE_CXX_FLAGS_DEBUG} /SAFESEH:NO")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG    "${CMAKE_EXE_LINKER_FLAGS_DEBUG} /SAFESEH:NO")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} /SAFESEH:NO")
    set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS_DEBUG} /SAFESEH:NO")
endif()

# ----- Links -----
target_link_libraries(BO1Zombies PUBLIC
    binkw32.lib
    d3d9.lib
    ws2_32.lib
    dxguid.lib
    dsound.lib
    winmm.lib
    xinput.lib
    xapobase.lib
    dxerr.lib
    libvpx.lib
    # June-2010 DirectX SDK libs (xapobase, dxerr) reference pre-VS2015 CRT
    # symbols (_vsnprintf, etc.) that the modern UCRT no longer exports.
    # legacy_stdio_definitions.lib provides shims so those static libs link.
    legacy_stdio_definitions.lib
)
# ----- Debug Links -----
target_link_libraries(BO1Zombies PUBLIC $<$<CONFIG:Debug>:d3dx9d.lib>)
# ----- Release Links -----
target_link_libraries(BO1Zombies PUBLIC $<$<CONFIG:Release>:d3dx9.lib>)
target_link_options(BO1Zombies PRIVATE /machine:x86)

# ----- Linker Options -----
# /DYNAMICBASE:NO - Turning this off causes directx to crash "directx has encountered an unrecoverable error". 
# /LARGEADDRESSAWARE - for convenience (4gb)
# /DEBUG - PDB for both configs 
target_link_options(BO1Zombies PRIVATE /DYNAMICBASE:NO /LARGEADDRESSAWARE /DEBUG)

set_target_properties(BO1Zombies PROPERTIES WIN32_EXECUTABLE TRUE)

# The VS debugger runs the exe in the game files (game/t5/): the working directory is fs_b's default
set_target_properties(BO1Zombies PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/game/t5"
)

# The exe loads binkw32.dll from the game files, its working directory: every Black Ops install has it. It needs no
# steam_api.dll (src/win32/win_steam_none.cpp).
# The two DirectX 9 DLLs a fresh Windows lacks, out of the DirectX SDK's redistributable cabs, next to the exe too, so a
# Windows player installs nothing (Wine has its own).
add_custom_command(TARGET BO1Zombies POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E tar xf "${DXSDK_DIR}/Redist/Jun2010_d3dx9_43_x86.cab" d3dx9_43.dll
    COMMAND ${CMAKE_COMMAND} -E tar xf "${DXSDK_DIR}/Redist/APR2007_xinput_x86.cab" xinput1_3.dll
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:BO1Zombies>"
    COMMENT "Unpacking d3dx9_43.dll and xinput1_3.dll to the run folder")

# Base-game data files the install does not carry (data/main/ -> main/ next to the exe, found through fs_cdpath), e.g.
# playlists_sp.info, the offline stand-in for the retail online WAD entry (BG_SetZombieExternalDvars in
# src/bgame/bg_misc.cpp).
add_custom_target(copy_base_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/main" "${CMAKE_BINARY_DIR}/$<CONFIG>/main"
    COMMENT "Copying data/main/ to the run folder")

# Keep the build machine's folder out of the binary: source paths in asserts are trimmed to the project and the
# PDB is referenced by file name only.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" BO1_SOURCE_DIR_NATIVE)
if(NOT CMAKE_HOST_WIN32)
    # MSVC under Wine (docker/): msvc-wine hands cl.exe the sources as z:/<unix path>
    set(BO1_SOURCE_DIR_NATIVE "z:${CMAKE_CURRENT_SOURCE_DIR}")
endif()
target_compile_options(BO1Zombies PRIVATE "/d1trimfile:${BO1_SOURCE_DIR_NATIVE}")
target_link_options(BO1Zombies PRIVATE "/PDBALTPATH:%_PDB%")
