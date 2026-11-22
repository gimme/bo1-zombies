# bo1-zombies

The engine of Call of Duty: Black Ops Zombies, as source code that builds. It produces two builds of the same engine:

- `BO1Zombies.exe` for Windows. It is the Windows client and, under Wine, the dedicated server.
- A WebAssembly build for Chrome, which draws with WebGPU (`web/`).

[zombies-hub](https://github.com/gimme/zombies-hub) is the server that runs it. It downloads a build from this repo's [releases](https://github.com/gimme/bo1-zombies/releases).

The source is decompiled from the game, starting from [MisaDev4's decompilation](https://github.com/MisaDev4/bo1-zombies-decompiled). It has no licence: the game belongs to Activision. The repo contains none of the game's files. A server and each of its players use their own copy of Black Ops.

## Building

You need Docker and `make`. On Windows, run the commands in Git Bash with Docker Desktop started.

```sh
make build     # BO1Zombies.exe into build/exe/Release, and the web client into build/web
make id        # show the build ID of the source and of each build
make package   # pack both builds into build/bo1-zombies-<id>.tar.gz
```

A client can only join a server of the same build, so `make build` always makes both builds. Only what changed is compiled.

The build ID is the last commit that changed `src/`, `tl/`, `DemonWare/`, `data/` or `cmake_files.cmake` (see `build_id.cmake`).

The first build makes the toolchain image, which takes about 5 minutes and 6.7 GB. It downloads MSVC and the SDKs from Microsoft and accepts their licences. Keep the images local: they can't be redistributed.

## Releases

A zombies-hub server runs one pinned release. GitHub Actions builds and publishes a release when a version tag is pushed (`.github/workflows/release.yml`):

```sh
git tag v2
git push origin v2
```

To move zombies-hub to the new release, put the tag and the archive's SHA-256 in `internal/bo1zombies/bo1zombies.go` there. The release notes have the SHA-256.

To try a build without releasing it, run `make package` and start the zombies-hub server with `ZOMBIES_ENGINE_FILE` set to the archive's path.

## Layout

- `src/`, `tl/`, `DemonWare/`: the engine's source, shared by both builds. `cmake_files.cmake` lists the files.
- `CMakeLists.txt`: the Windows build (MSVC, x86).
- `web/`: the Chrome build (Emscripten). `web/pipelines/` has one list per map of the GPU pipelines the web client compiles while the map loads.
- `data/main/`: base data files that an install of the game lacks.
- `docker/`: the build and package scripts, and the images the builds run in (MSVC under Wine, Emscripten).
- `tools/`: scripts for working on the engine.
- `Play.bat`: starts the Windows client and joins a room's game. It expects a copy of a Black Ops install in `game\t5\` in this folder, or in the folder `BO1_GAME_DIR` names.
