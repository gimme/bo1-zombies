# build.ps1 - build the WebAssembly version of the game (zombies-hub's web/public/player.js runs it in Chrome).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File web\build.ps1 [-Emsdk C:\emsdk] [-Asan] [-Clean]
#
# Needs the Emscripten SDK (https://emscripten.org/docs/getting_started/downloads.html, installed and activated
# with "emsdk install latest" / "emsdk activate latest"), CMake and Ninja (Visual Studio's own copies are found
# when they are not on PATH) and the June 2010 DirectX SDK headers. Output: build\web (build\web-asan with -Asan).
# The game's movies (the maps' loading screens) need FFmpeg's Bink decoders built for Emscripten, which
# docker/build-web.sh has in its image (docker/Dockerfile: the web target, FFMPEG_DIR): without them (FFMPEG_DIR
# unset) the build plays none, and says so.
param(
    [string]$Emsdk = $(if ($env:EMSDK) { $env:EMSDK } else { Join-Path $HOME 'emsdk' }),
    [switch]$Asan,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent # the engine's folder: this file is in its web\

$emsdkEnv = Join-Path $Emsdk 'emsdk_env.ps1'
if (-not (Test-Path $emsdkEnv)) { throw "No Emscripten SDK at '$Emsdk'. Pass -Emsdk with the folder that holds emsdk_env.ps1." }
. $emsdkEnv | Out-Null

# CMake and Ninja: PATH first, then the ones Visual Studio ships
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -or -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $vsCmake = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake' -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    if (-not $vsCmake) { throw 'CMake and Ninja were not found (install them, or Visual Studio with C++ CMake tools).' }
    $env:PATH = "$env:PATH;$(Join-Path $vsCmake.FullName 'CMake\bin');$(Join-Path $vsCmake.FullName 'Ninja')"
}

$out = Join-Path $root $(if ($Asan) { 'build\web-asan' } else { 'build\web' })
if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

$config = @('-S', $PSScriptRoot, '-B', $out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DBO1_WEB_ASAN=$(if ($Asan) { 'ON' } else { 'OFF' })")
emcmake cmake @config
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $out
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
"built $out\bo1zombies.wasm"
