# build_id.cmake - the build's ID, which a client and a server compare when it joins: a client joins only a server of
# its own build (src/client_mp/cl_main_mp.cpp, sv_client_mp.cpp: SV_DirectConnect). It names the engine's source, what
# both builds compile and run with: the last commit that changed src/, tl/, DemonWare/, data/ or cmake_files.cmake,
# and when those have changes not committed, a hash of them, such as 2d63ee1+5e0c9a1f. A commit that changes only the
# web client's own code (web/), the lobby or the docs keeps it, so the web client rebuilt after one still joins the
# server.
#
#   cmake -DROOT=<checkout> -DOUT=<header> [-DID=<id>] -P build_id.cmake
#
# CMakeLists.txt and web/CMakeLists.txt include it, which makes the build run it each time. It writes the header
# (#define BO1_BUILD_ID "<id>", which src/universal/com_buildinfo.cpp includes) when the ID changed, so the engine
# compiles again only then. ID: the ID docker/build-id.sh works out on the host, the same way, for a build in a
# container, which has no git (BO1_BUILD_ID). Without either, the ID is "unknown".
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(BO1_BUILD_ID "" CACHE STRING "the build's ID, for a build without git (docker/build-id.sh); empty: git's")
    set(header "${CMAKE_BINARY_DIR}/generated/bo1_build_id.h")
    add_custom_target(bo1_build_id
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_LIST_DIR}" "-DOUT=${header}" "-DID=${BO1_BUILD_ID}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        BYPRODUCTS "${header}"
        VERBATIM)
    add_dependencies(${BIN_NAME} bo1_build_id)
    # only the file that includes it: a directory added for every file would change how each compiles
    set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/universal/com_buildinfo.cpp" APPEND
        PROPERTY INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/generated")
    return()
endif()

set(paths src tl DemonWare data cmake_files.cmake)
if(NOT ID)
    find_program(GIT git)
    if(GIT)
        execute_process(COMMAND "${GIT}" -C "${ROOT}" log -1 --format=%h -- ${paths}
            OUTPUT_VARIABLE ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
        if(failed)
            set(ID "")
        endif()
    endif()
    if(ID)
        # the changes: the diff from the commit, then each file git doesn't track, with its SHA-1
        execute_process(COMMAND "${GIT}" -C "${ROOT}" diff HEAD --binary --no-ext-diff --no-textconv -- ${paths}
            OUTPUT_VARIABLE changes ERROR_QUIET)
        execute_process(COMMAND "${GIT}" -C "${ROOT}" ls-files --others --exclude-standard -- ${paths}
            OUTPUT_VARIABLE untracked ERROR_QUIET)
        string(REPLACE "\n" ";" untracked "${untracked}")
        foreach(file IN LISTS untracked)
            if(file)
                file(SHA1 "${ROOT}/${file}" sum)
                string(APPEND changes "${file} ${sum}\n")
            endif()
        endforeach()
        if(changes)
            string(SHA1 sum "${changes}")
            string(SUBSTRING "${sum}" 0 8 sum)
            string(APPEND ID "+${sum}")
        endif()
    endif()
endif()
if(NOT ID)
    set(ID unknown)
endif()

set(text "#define BO1_BUILD_ID \"${ID}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
