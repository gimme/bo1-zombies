# syntax=docker/dockerfile:1
#
# Linux images that build the engine. One file, two targets:
#
#   msvc    build BO1Zombies.exe: the real MSVC (Visual Studio 2022 / 17.x, x86 target) under Wine via msvc-wine
#           (https://github.com/mstorsjo/msvc-wine), the Windows SDK, the June 2010 DirectX SDK, CMake, Ninja.
#           docker/build-exe.sh builds this image and runs the build in it.
#   web     build the WebAssembly version (web/): emsdk + the DirectX SDK headers. docker/build-web.sh.
#
#   docker build -f docker/Dockerfile --target msvc -t zombies-msvc:17 --build-arg UID=$(id -u) --build-arg GID=$(id -g) docker
#
# Building the msvc target downloads MSVC and the Windows SDK from Microsoft and accepts their license
# (https://go.microsoft.com/fwlink/?LinkId=2086102) on your behalf (vsdownload.py --accept-license); both targets
# download the DirectX SDK (its EULA ships inside it). The images are not redistributable: keep them local.

ARG UBUNTU=ubuntu:24.04
ARG EMSDK_VERSION=6.0.13

# ---- June 2010 DirectX SDK: headers, x86 import/static libs, and the redistributable cabs of the two DirectX DLLs
# Windows lacks (CMakeLists.txt puts them next to the exe) ----
FROM ${UBUNTU} AS dxsdk
ARG DXSDK_URL=https://download.microsoft.com/download/A/E/7/AE743F1F-632B-4809-87A9-AA1BB3458E31/DXSDK_Jun10.exe
ARG DXSDK_SHA256=705271dc83bfee54d9b94e028426e288d5f070784b7446d164f48ecfbb2a02cb
RUN apt-get update && apt-get install -y --no-install-recommends 7zip curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*
# one RUN: the 600 MB installer never lands in a layer
RUN curl -fsSL -o /tmp/dxsdk.exe "$DXSDK_URL" && \
    echo "$DXSDK_SHA256  /tmp/dxsdk.exe" | sha256sum -c - && \
    7z x -y -bd -o/tmp/x /tmp/dxsdk.exe DXSDK/Include DXSDK/Lib/x86 \
        DXSDK/Redist/Jun2010_d3dx9_43_x86.cab DXSDK/Redist/APR2007_xinput_x86.cab >/dev/null && \
    test -f /tmp/x/DXSDK/Redist/Jun2010_d3dx9_43_x86.cab && test -f /tmp/x/DXSDK/Redist/APR2007_xinput_x86.cab && \
    mkdir -p /opt/dxsdk && mv /tmp/x/DXSDK/Include /tmp/x/DXSDK/Lib /tmp/x/DXSDK/Redist /opt/dxsdk/ && \
    rm -rf /tmp/x /tmp/dxsdk.exe && du -sh /opt/dxsdk
# Lowercase aliases (DxErr.h -> dxerr.h, XInput.lib -> xinput.lib, ...), as msvc-wine adds for the Windows SDK. cl.exe
# under Wine finds either spelling, but /showIncludes reports the one in the #include, and Ninja (and every native
# tool) looks that up on a case-sensitive file system: without the alias the object counts as out of date forever.
RUN cd /opt/dxsdk && for f in Include/* Lib/x86/*; do \
        l="$(dirname "$f")/$(basename "$f" | tr 'A-Z' 'a-z')"; \
        [ -e "$l" ] || ln -s "$(basename "$f")" "$l"; \
    done

# ---- Wine for the build: 64-bit for cl.exe/link.exe, 32-bit for the game they build ----
FROM ${UBUNTU} AS wine-base
# winbind: cl.exe /Zi (CMake's Debug-config compiler probes) talks to mspdbsrv.exe, which needs it under Wine
RUN dpkg --add-architecture i386 && apt-get update && \
    apt-get install -y --no-install-recommends wine wine64 wine32:i386 winbind ca-certificates && \
    rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all

# ---- target msvc: the image the Windows build runs in ----
# MSVC + the Windows SDK are unpacked by msvc-wine (pinned commit) in this stage itself rather than in a separate
# one, so the ~3.3 GB toolchain is stored once (a COPY --from would keep a second copy in the build cache).
FROM wine-base AS msvc
# Pinned, so every machine builds the same exe: Visual Studio 2022 (17; vsdownload.py defaults to 18 = VS 2026),
# release 17.14 = MSVC toolset 14.44, Windows SDK 10.0.26100. Changing them downloads the toolchain again.
ARG VS_MAJOR=17
ARG MSVC_VERSION=17.14
ARG SDK_VERSION=10.0.26100
ARG MSVC_WINE_COMMIT=514f8ea34842cd6d831804d0e9658d3a32870ae1
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build python3 msitools curl && \
    rm -rf /var/lib/apt/lists/*
# x64 is kept next to x86: the x86-targeting cl.exe is an x64 program that also loads DLLs from Hostx64/x64, and
# install.sh builds msvctricks.exe (exit codes and stdout/stderr of the tools under Wine) with the x64 compiler.
# The sed: msvc-wine's wrapper (wine-msvc.sh) relays the tools' stdout and stderr through two sed processes into one
# pipe. Block-buffered, they splice stderr lines (cl's D9025 warning, for one) into the middle of /showIncludes lines;
# Ninja then records dependencies that never exist and rebuilds those objects every time. Line-buffered they cannot.
RUN curl -fsSL "https://github.com/mstorsjo/msvc-wine/archive/${MSVC_WINE_COMMIT}.tar.gz" | tar -xz -C /tmp && \
    mv /tmp/msvc-wine-* /tmp/msvc-wine && \
    PYTHONUNBUFFERED=1 /tmp/msvc-wine/vsdownload.py --accept-license --major "$VS_MAJOR" \
        ${MSVC_VERSION:+--msvc-version "$MSVC_VERSION"} ${SDK_VERSION:+--sdk-version "$SDK_VERSION"} \
        --architecture x86 x64 \
        --with-workload no --with-asan no --with-atl no --with-dia no --with-msbuild no --with-devcmd no \
        --dest /opt/msvc && \
    /tmp/msvc-wine/install.sh /opt/msvc && \
    test -f /opt/msvc/bin/msvctricks.exe && \
    sed -i 's/sed -E "\$WINE_MSVC_/sed -u -E "$WINE_MSVC_/g' /opt/msvc/bin/*/wine-msvc.sh && \
    test "$(grep -c 'sed -u -E' /opt/msvc/bin/x86/wine-msvc.sh)" = 3 && \
    cp /tmp/msvc-wine/LICENSE.txt /opt/msvc/msvc-wine-LICENSE.txt && \
    rm -rf /tmp/msvc-wine /root/.wine /root/.cache && \
    ls /opt/msvc/bin /opt/msvc/vc/tools/msvc /opt/msvc/kits/10/include
COPY --from=dxsdk /opt/dxsdk /opt/dxsdk
# Run as the host user (UID/GID build args) so the build tree in the bind mount is owned by them, with a Wine
# prefix made at image build time (wineboot takes a while, and must not run once per build).
ARG UID=1000
ARG GID=1000
RUN (userdel -r ubuntu 2>/dev/null || true) && \
    groupadd -g "$GID" builder && useradd -m -u "$UID" -g "$GID" builder
COPY --chmod=755 wineprefix-init.sh /usr/local/bin/
USER builder
ENV WINEPREFIX=/home/builder/.wine \
    PATH=/opt/msvc/bin/x86:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    DXSDK_DIR=/opt/dxsdk
RUN wineprefix-init.sh
WORKDIR /home/builder

# ---- target web: emsdk + the DirectX SDK headers (web/CMakeLists.txt reads its d3d9*.h) + FFmpeg's Bink decoders ----
FROM emscripten/emsdk:${EMSDK_VERSION} AS web
RUN apt-get update && apt-get install -y --no-install-recommends ninja-build xz-utils && \
    rm -rf /var/lib/apt/lists/*
COPY --from=dxsdk /opt/dxsdk/Include /opt/dxsdk/Include
ENV DXSDK_DIR=/opt/dxsdk
# FFmpeg (LGPL 2.1), only its Bink video and audio decoders and Bink demuxer, as WebAssembly with the engine's flags
# (threads, SIMD): the web client plays the game's movies with them (web/src/bink_web.cpp; web/CMakeLists.txt links
# them from FFMPEG_DIR). Static libraries for one thread: the engine decodes on its cinematic worker alone. A minute.
# emcc runs here as root, and what it adds to Emscripten's cache (js_output, symbol_lists) is root's: the build runs
# as another user, in a volume that Docker seeds from this cache (docker/build-web.sh), and could write no lock there.
# So what root wrote is made everyone's, as emsdk's own cache is; only that, since a chmod copies a file into the layer.
ARG FFMPEG_VERSION=7.1.1
ARG FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
RUN curl -fsSL -o /tmp/ffmpeg.tar.xz "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" && \
    echo "$FFMPEG_SHA256  /tmp/ffmpeg.tar.xz" | sha256sum -c - && \
    mkdir /tmp/ffmpeg && tar -xJf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg --strip-components=1 && cd /tmp/ffmpeg && \
    emconfigure ./configure --prefix=/opt/ffmpeg \
        --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm --objcc=emcc --dep-cc=emcc \
        --enable-cross-compile --target-os=none --arch=x86_32 --disable-asm --disable-stripping \
        --disable-programs --disable-doc --disable-everything --disable-autodetect --disable-network \
        --disable-avdevice --disable-swresample --disable-swscale --disable-avfilter --disable-postproc \
        --disable-pthreads --disable-w32threads --disable-os2threads --disable-debug \
        --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-demuxer=bink \
        --extra-cflags="-pthread -msimd128 -O3 -fno-strict-aliasing" >/tmp/ffmpeg-configure.log && \
    emmake make -j"$(nproc)" >/tmp/ffmpeg-make.log && emmake make install >/dev/null && \
    cd / && rm -rf /tmp/ffmpeg /tmp/ffmpeg.tar.xz /tmp/ffmpeg-*.log && ls /opt/ffmpeg/lib && \
    find /emsdk/upstream/emscripten/cache -user root -exec chmod a+rwX {} +
ENV FFMPEG_DIR=/opt/ffmpeg
