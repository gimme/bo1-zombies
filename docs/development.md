# Development

The [README](../README.md) covers building with Docker, packaging and releasing. This page has the rest.

## Build output

Everything the builds make is in `build/`. `make clean` deletes the two builds and keeps the rest.

- `build/exe`: the Windows build, with `BO1Zombies.exe` in `build/exe/Release`.
- `build/web`: the web client's build (`build/web-asan` with AddressSanitizer).
- `build/bo1-zombies-<id>.tar.gz`: the archive from `make package`.
- `build/client`: the Windows client's settings and logs, from `Play.bat`.

On Windows the builds read the source through Docker Desktop's file sharing. `make exe` takes about 15 minutes the first time, toolchain image included, and a minute and a half after a change to one file. The first `make web` downloads FFmpeg's source, for the game's movies.

## Build IDs

A client can only join a server of the same build. Each build has an ID (`build_id.cmake`) that names the source it was built from: the last commit that changed `src/`, `tl/`, `DemonWare/`, `data/` or `cmake_files.cmake`, plus a hash of any uncommitted changes to them (`2d63ee1+5e0c9a1f`). The game's log starts with it (`BO1 Zombies build ...`).

- A client of another build is refused when it asks to join. It says which builds they are, and the server logs it.
- A change to only `web/`, `docker/` or the docs keeps the ID.
- `make id` shows the ID of the source and of each build. `make package` refuses two builds with different IDs.
- `bo1_build` is the build a client or server says it is. `+set bo1_build other` on the command line makes a client of another build, to see it refused.

## Building natively on Windows

Building works without Docker too, with CMake, VS 2022 and the June 2010 DirectX SDK:

```bat
cmake -S . -B build/exe -A Win32
cmake --build build/exe --config Release
```

`web\build.ps1` builds the web client. Without FFmpeg (`FFMPEG_DIR`) it plays no movies, and its loading screens stay still.

## The Windows client

`build/exe/Release/BO1Zombies.exe` is also the Windows client. On a PC with this checkout, a build in `build/exe/Release` and the game files in `game\t5\` in the checkout (or in the folder `BO1_GAME_DIR` names), a player joins their room's game with:

```bat
Play.bat <server>:<port> <key>
```

The menu under the player's name on the server's page shows the line with their key. While the game is open, the room shows the player as on Windows, and the page leaves the game to it. When the game ends the client quits, and its log (`build\client\main\console_mp.log`) says why. It runs in a borderless window without v-sync, and keeps its config in `build\client\players\`.

`tools\headless.ps1 -Client -Zombies -Commands "+set bo1_key <key> +connect 127.0.0.1:3075"` does the same unattended. It draws with the PC's GPU in a window on a desktop of its own, never on the screen. `-ShotsAtMs 45000` takes a screenshot 45 s in, into `build/exe/Release/main/screenshots/`.

## Script builtins against retail

The level's scripts call builtins, and most of them run code that came from the MP exe. `tools/builtins` compares each with the one retail's Zombies ran (the SP exe's of the same name), and `tools/builtins/status.tsv` keeps the result for each. It needs Python 3 with `capstone` (`pip install capstone`) and both exes in the game files' folder.

- `python tools/builtins triage` finds the builtin tables in the two exes, scans the zombie maps' scripts in the fastfiles for the builtins they call, and compares each one's function in `BlackOpsMP.exe` with the one in `BlackOps.exe`, following its calls. It rewrites the `triage` column: `same`, `layout` (the same code except for struct offsets and sizes), or one of three that need reading: `number` (a number or constant differs), `helper` (a function it calls differs) or `code` (its own code differs). Run it when a table in the source changes.
- `python tools/builtins show <name>` prints a builtin's function in the two exes side by side. `show <mp> <sp>` does it for any two functions by address.
- The `review` and `note` columns are kept by hand: `same` once a builtin has been read and the port does what the SP exe does, `fixed` when that took a change, and `differs` for a difference that was left in.

The triage compares the two retail exes, not the port. The port's source came from another MP build than retail's, so a builtin marked `same` can still be wrong in the port, and one marked for reading is often already right where the source has `// zombies:` code for it.
