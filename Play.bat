@echo off
rem Black Ops 1 Zombies client: plays your room's game on a server, straight into it.
rem   Play.bat <host>[:port] <key>     port 3075 by default, the lobby's; key: yours, which the server's page shows you
rem Join a room on the server's page, and open Play in the Windows game (Play.bat): it shows the line to run, with
rem your key. While it is open, the room shows you on Windows, and you play here, not in the page; press Ready on the
rem page as everyone does (closing this game makes you unready). It waits for the room's game, and round 1 waits for
rem you. Run it again to take your character back, should you drop. When the game ends, or you leave it, the game
rem quits, and its log (build\client\main\console_mp.log) says why.
rem Runs the build (build\exe\Release) on the game files (game\t5\ in this folder, or the folder BO1_GAME_DIR names:
rem a copy of a Black Ops install); settings and logs go to build\client.
rem It draws without v-sync, which in a borderless window doesn't tear (Windows composes the screen) and doesn't hold frames back for the display, as many frames a second as the player's Frame Rate Limit lets
rem it (the Graphics menu's: 240 at first, as Unlimited runs the GPU flat out), and sends the server a packet every
rem frame up to 250 a second (as many as a frame rate of 240 has; every frame to a server at a LAN address).
setlocal
set "BIN=%~dp0build\exe\Release"
set "GAME=%~dp0game\t5"
if defined BO1_GAME_DIR set "GAME=%BO1_GAME_DIR%"
set "CLHOME=%~dp0build\client"
if not exist "%BIN%\BO1Zombies.exe" (echo Game not built: no %BIN%\BO1Zombies.exe & pause & exit /b 1)
if not exist "%GAME%\zone" (echo No game files in %GAME%: copy a Black Ops install there & pause & exit /b 1)
if "%~2"=="" (echo usage: Play.bat ^<host^>[:port] ^<key^>, the line the server's page shows you & pause & exit /b 1)
set "SERVER=%~1"
set "KEY=%~2"
if "%SERVER::=%"=="%SERVER%" set "SERVER=%SERVER%:3075"
if not exist "%CLHOME%" mkdir "%CLHOME%"
cd /d "%GAME%"
start "" "%BIN%\BO1Zombies.exe" +set bo1_zombies 1 +set fs_b "%GAME%" +set fs_h "%CLHOME%" +set logfile 2 +set com_introPlayed 1 +set r_fullscreen 0 +set r_borderless 1 +set vid_xpos 0 +set vid_ypos 0 +set r_vsync 0 +set com_maxfps 0 +set cl_maxpackets 250 +set bo1_key %KEY% +connect %SERVER%
