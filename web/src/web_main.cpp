// Entry point and system layer of the WebAssembly build (what src/win32/win_main.cpp, win_syscon.cpp and
// win_mini_dumper.cpp are on Windows).
//
// main() runs on a worker (PROXY_TO_PTHREAD), so the engine's main thread can block like it does on Windows; the
// page's own thread stays free for drawing (d3d9_webgpu.cpp), input and the file fetches.
//
// Files: WasmFS mounts the install at /game, read from the browser's store (zombies-hub's web/public/store.js, through
// library_bo1.js), which a map's download filled before its game (zombies-hub's web/public/player.js: store), and a
// writable /home for configs, profiles and logs (fs_h), in memory but for players/, the player's own folder, which is
// the store's players/ directory, so that their config keeps their settings from one visit to the next. The install's
// file list comes from the server (game/manifest.txt: offline, the page's service worker keeps it), each file with its
// size and its contents' hash, which the store keeps its pieces by; names are case-insensitive, as on Windows. The
// mount is at the manifest's version (game/<version>/...), which names the files' contents.
#include <windows.h>

#include <win32/win_main.h>
#include <win32/win_syscon.h>
#include <win32/win_wndproc.h>
#include <win32/win_localize.h>
#include <win32/win_steam.h>
#include <win32/win_shared.h>
#include <client/client.h>
#include <client/cl_keys.h>
#include <gfx_d3d/r_cinematic.h>
#include <clientscript/cscr_stringlist.h>
#include <client/con_channels.h>
#include <qcommon/threads.h>
#include <qcommon/mem_track.h>
#include <qcommon/tl_support.h>
#include <universal/physicalmemory.h>
#include <universal/com_memory.h>
#include <universal/q_parse.h>
#include <universal/timing.h>
#include <live/live_steam.h>
#include <tl/tl_system.h>
#include <win32/win_input.h>
#include <qcommon/cmd.h>
#include <win32/win_common.h>
#include <game_mp/g_main_mp.h>
#include <win32/win_net.h>
#include "d3d9_cmd.h"

#include <stdarg.h>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <emscripten.h>
#include <atomic>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "web_platform.h"

// ============================================================================================================
// platform helpers (web_platform.h)
// ============================================================================================================

static double s_startMs;

double Bo1_Milliseconds()
{
    // emscripten_get_now is performance.timeOrigin + performance.now(): one clock across workers
    return emscripten_get_now() - s_startMs;
}

unsigned Bo1_GetCpuCount()
{
    static unsigned count;
    if (!count)
    {
        int n = EM_ASM_INT({ return (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4; });
        count = n < 2 ? 2 : n > 16 ? 16 : (unsigned)n;
    }
    return count;
}

static char s_cmdline[2048];
char sys_cmdline[1024];

char *Bo1_GetCommandLine() { return s_cmdline; }

static bool s_logFiles;

bool Bo1_LogFiles() { return s_logFiles; }

void Bo1_Log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emscripten_log(EM_LOG_CONSOLE, "[bo1] %s", buf);
}

// AddressSanitizer builds (web/build.ps1 -Asan): report every bad access and carry on, once per place
extern "C" const char *__asan_default_options() { return "halt_on_error=0:suppress_equal_pcs=1:detect_leaks=0:print_summary=1"; }

void Bo1_LogStall(const char *what)
{
    static std::atomic<int> budget{40};
    if (budget.fetch_sub(1) > 0)
        emscripten_log(EM_LOG_CONSOLE | EM_LOG_JS_STACK, "[bo1] stall: %s on thread %lu", what, (unsigned long)GetCurrentThreadId());
}

std::atomic<uint64_t> g_bo1FileUs, g_bo1PageUs, g_bo1CompileUs, g_bo1WaitPageUs;
std::atomic<uint32_t> g_bo1Compiles, g_bo1PipelineWaits, g_bo1PipelineStandIns, g_bo1PreloadPending;
std::atomic<int> g_bo1ShadowMaps{-1};
std::atomic<uint64_t> g_bo1GameUs;
std::atomic<uint32_t> g_bo1GameFrames, g_bo1GameFrameUs;

static std::mutex s_slowReadLock;
static double s_slowReadMs; // the slowest read since the last frame, and what it read
static std::string s_slowReadPath;
static unsigned long s_slowReadThread;

void Bo1_NoteFileRead(const char *path, double ms)
{
    g_bo1FileUs += (uint64_t)(ms * 1000.0);
    if (ms < 1.0) // the data was there
        return;
    std::lock_guard<std::mutex> guard(s_slowReadLock);
    if (ms > s_slowReadMs)
    {
        s_slowReadMs = ms;
        s_slowReadPath = path;
        s_slowReadThread = (unsigned long)GetCurrentThreadId();
    }
}

void Bo1_FrameEnd()
{
    static double last, usual;
    static uint64_t file, page, compile, wait;
    static uint32_t compiles, waits, standIns;
    static int budget = 200;
    double now = Bo1_Milliseconds();
    double ms = now - last;
    uint64_t f = g_bo1FileUs, p = g_bo1PageUs, c = g_bo1CompileUs, w = g_bo1WaitPageUs;
    uint32_t n = g_bo1Compiles, left = g_bo1PipelineWaits, si = g_bo1PipelineStandIns;
    double slowMs;
    std::string slowPath;
    unsigned long slowThread;
    {
        std::lock_guard<std::mutex> guard(s_slowReadLock);
        slowMs = s_slowReadMs;
        slowPath.swap(s_slowReadPath);
        slowThread = s_slowReadThread;
        s_slowReadMs = 0.0;
    }
    // a spike: over 50 ms, and three times the usual frame (which follows slower stretches, such as loading screens)
    bool spike = last > 0.0 && usual > 0.0 && ms > 50.0 && ms > 3.0 * usual;
    if (last > 0.0)
        usual = usual > 0.0 ? usual * 0.9 + (ms < 3.0 * usual ? ms : 3.0 * usual) * 0.1 : ms;
    last = now;
    if (spike && budget > 0)
    {
        --budget;
        char slow[400] = "";
        if (slowMs > 0.0)
            snprintf(slow, sizeof(slow), ", the slowest %.0f ms (%s, thread %lu)", slowMs, slowPath.c_str(), slowThread);
        Bo1_Log("lag spike: a %.0f ms frame (usually %.0f ms). The game waited %.0f ms for the page, which drew for %.0f ms, %.0f ms of it "
                "starting %u pipelines (%u draws waited for pipelines compiling, %u of them drawn with a stand-in). File reads "
                "took %.0f ms%s.",
            ms, usual, (w - wait) / 1000.0, (p - page) / 1000.0, (c - compile) / 1000.0, n - compiles, left - waits, si - standIns,
            (f - file) / 1000.0, slow);
    }
    file = f;
    page = p;
    compile = c;
    wait = w;
    compiles = n;
    waits = left;
    standIns = si;
}

extern "C" void bo1_status(const char *what, const char *text); // library_bo1.js, on the page's thread

static void Bo1_JsNotify(const char *what, const char *text) { bo1_status(what, text); }

// The page asks for bo1_player's answers (src/cgame_mp/cg_consolecmds_mp.cpp), where it measures the map's views by
// itself and no script reads the console for them (zombies-hub's web/src/perf.ts: &perf=tour): from then on Sys_Print
// gives it them, as it does bo1_lobby's lines. Only then: the call waits for the page's thread, which a test that asks
// many times a second (zombies-hub's e2e/game/player.ts) would hold the game's thread up with.
static std::atomic<bool> s_pageHearsPlayer;

extern "C" EMSCRIPTEN_KEEPALIVE void Bo1_PageHearsPlayer() { s_pageHearsPlayer.store(true, std::memory_order_release); }

// the engine read the install where the browser's store has nothing (library_bo1.js: a piece of the map's game files
// it lacks): the page says so, which ends the game
extern "C" EMSCRIPTEN_KEEPALIVE void bo1_store_missing(const char *path, double offset)
{
    char text[512];
    snprintf(text, sizeof(text), "This browser lacks part of the game files (%s, at byte %.0f): remove them, and "
        "download the map again.", path, offset);
    Bo1_Log("%s", text);
    Bo1_JsNotify("error", text);
}

// the browser's store didn't answer a read of the install in 30 s (library_bo1.js: a step of its file system hung):
// the page says so, which ends the game; started again, the game has a new page's workers, which are past the step
extern "C" EMSCRIPTEN_KEEPALIVE void bo1_store_hung(const char *path)
{
    char text[512];
    snprintf(text, sizeof(text), "This browser stopped answering as the game read its files (%s): start the game "
        "again.", path);
    Bo1_Log("%s", text);
    Bo1_JsNotify("error", text);
}

// the server is another build than this client (src/client_mp/cl_main_mp.cpp: CL_RefuseBuild): the page says so, and
// whether reloading it gets the server's build
void Bo1_BuildRefused(const char *text) { Bo1_JsNotify("build", text); }

// the client's game ended (src/client_mp/cl_main_mp.cpp: CL_GameEnded): the page hears why at once, "<why>\n<text>",
// and decides what comes next; the engine quits after (Bo1_Exit has nothing more to say)
static bool s_gameEnded;

void Sys_GameEnded(const char *why, const char *text)
{
    char what[1100];
    snprintf(what, sizeof(what), "%s\n%s", why, text);
    s_gameEnded = true;
    Bo1_MovieStop(); // a loading movie the game ended before the client played on with (it couldn't join, say)
    Bo1_JsNotify("ended", what);
}

// it ended, and Call of the Dead's outro rolls first (src/client/cl_cin.cpp: CL_PlayOutro): the page waits for it, which
// Sys_GameEnded follows
void Sys_GameEndsAfterOutro() { Bo1_JsNotify("outro", ""); }

// the client's loading screen shows the map it loads, its movie or its still (src/client/cl_cin.cpp:
// CL_LoadingMovieStart): the page shows the canvas, which it kept out of sight over the map's movie while the engine
// started and joined the game (zombies-hub's web/public/player.js: movieSaid), black until then
void Sys_LoadingScreenShows()
{
    static bool s_said;
    if (s_said)
        return;
    s_said = true;
    Bo1_JsNotify("loading", "");
}

// The maps' loading movies play on the page (bink_web.cpp), apart from the engine, from its start (main): the client
// plays on with the one the page started, and asks after it, and stops it (src/client/cl_cin.cpp). One the client
// starts plays at the player's volumes as the engine has them.
bool Sys_LoadingMoviePlay(const char *name) { return Bo1_MovieStart(name, R_Cinematic_PlayerVolume()); }

SysLoadingMovie Sys_LoadingMovieState()
{
    switch (Bo1_MovieState())
    {
    case BO1_MOVIE_PENDING:
        return SYS_LOADING_MOVIE_PENDING;
    case BO1_MOVIE_SHOWS:
        return SYS_LOADING_MOVIE_SHOWS;
    case BO1_MOVIE_OVER:
        return SYS_LOADING_MOVIE_OVER;
    default:
        return SYS_LOADING_MOVIE_NONE;
    }
}

void Sys_LoadingMovieStop() { Bo1_MovieStop(); }

// the mouse is no one's (src/client/cl_cin.cpp: CL_LoadingMovieHidesCursor): the page captures it at a click all the
// same, which hides the browser's arrow, and lets it go as a menu's cursor shows (library_bo1.js)
void Sys_MouseIdle(bool idle) { __atomic_store_n(&g_bo1Input.mouseIdle, idle ? 1 : 0, __ATOMIC_RELEASE); }

// The map the page loads (Module.bo1LoadingMap: the room's, or the one played alone): its loading movie starts now, as
// the player's game starts, rather than as the client loads the map, ten seconds and more on. Not where the command
// line plays none (+set cl_loadingMovie 0), as the client then doesn't (cl_cin.cpp); nor where it has the movie start
// as the client loads the map (+set bo1_web_movieAtLoad 1: the player of zombies-hub's e2e/game/engine/solo.spec.ts,
// whose loaded game waits for it to skip it, in a browser that takes as long to load the map as the movie lasts). Its
// sound plays at the player's cinematics and master volumes, which the page gives too (Module.bo1MovieVolume:
// zombies-hub's web/src/settings.ts, cinematicGain), as the engine has yet to read the player's config: 1 (the movie's
// own) where it gives none.
static void Bo1_StartLoadingMovie()
{
    char map[64] = "";
    MAIN_THREAD_EM_ASM({
        const map = Module['bo1LoadingMap'];
        stringToUTF8(typeof map === 'string' ? map : '', $0, $1);
    }, map, (int)sizeof(map));
    double volume = MAIN_THREAD_EM_ASM_DOUBLE({
        const volume = Module['bo1MovieVolume'];
        return typeof volume === 'number' && volume >= 0 && volume <= 1 ? volume : 1;
    });
    if (!Com_IsZombieMap(map) || strstr(s_cmdline, "cl_loadingMovie 0") || strstr(s_cmdline, "bo1_web_movieAtLoad 1"))
        return;
    char name[80];
    snprintf(name, sizeof(name), "%s_load", map);
    Bo1_MovieStart(name, (float)volume);
}

void Bo1_Exit(int code)
{
    Bo1_Log("exit %d", code);
    if (!s_gameEnded)
        Bo1_JsNotify("exit", code ? "The game stopped." : "Quit.");
    Bo1_MovieStop();
    bo1gpu::ExecutorStop(); // the page may stay, saying why: it replays no more of an engine gone
    emscripten_force_exit(code);
    for (;;)
        Sleep(1000);
}

// ============================================================================================================
// win_main.cpp: system info, errors, quitting, the event queue
// ============================================================================================================

const dvar_t *sys_configureGHz;
const dvar_t *sys_sysMB;
const dvar_t *sys_gpu;
const dvar_t *sys_configSum;
const dvar_t *sys_SSE;

bool g_allowMature = true;
int s_nosnd;
SysInfo sys_info;

static volatile LONG s_quitRequested;

bool __cdecl Sys_IsHeadless() { return false; }
bool __cdecl Sys_IsHeadlessClient() { return false; }
bool __cdecl Sys_IsStdio() { return false; }
bool __cdecl Sys_IsUnattended() { return false; }
void __cdecl Sys_HeadlessNoteHud(int, bool) {}
void __cdecl Sys_HeadlessTimeline(const char *) {}
void __cdecl Sys_HeadlessLevelNotify(const char *) {}
void __cdecl Sys_HeadlessLogAssert(const char *filename, int line, const char *message)
{
    static std::atomic<int> budget{50};
    if (budget.fetch_sub(1) > 0)
        emscripten_log(EM_LOG_CONSOLE | EM_LOG_JS_STACK, "[bo1] assert %s:%d: %s", filename, line, message);
}
bool __cdecl Sys_AssertStops() { return false; } // __debugbreak is a no-op here (bo1_web_prelude.h)
unsigned int __cdecl Sys_HitchWatchBegin() { return 0; }
void __cdecl Sys_HitchWatchEnd() {}
unsigned int __cdecl Sys_FramePerfHitchMs() { return 0; }

void __cdecl Sys_SetQuitRequested() { InterlockedExchange(&s_quitRequested, 1); }
bool __cdecl Sys_IsQuitRequested() { return s_quitRequested != 0 || Sys_QueryWin32QuitEvent(); }
void __cdecl Sys_QuitWithLostDevice(int) { Bo1_Exit(0); }

char sys_exitCmdLine[1024];
sysEvent_t eventQue[256];
int eventHead;
int eventTail;
char sys_processSemaphoreFile[260];

bool __cdecl PC_StartWithNoSounds()
{
    if (G_ExitAfterToolComplete())
        s_nosnd = 1;
    return s_nosnd != 0;
}

void __cdecl Sys_GetInfo(SysInfo *info) { memcpy(info, &sys_info, sizeof(SysInfo)); }

bool __cdecl Sys_HasConfigureChecksumChanged(int checksum)
{
    Sys_RegisterInfoDvars();
    if (!sys_configSum->current.integer || sys_configSum->current.integer != checksum)
        Dvar_SetInt((dvar_s *)sys_configSum, checksum);
    return false;
}

bool __cdecl Sys_ShouldUpdateForConfigChange() { return false; }

void Sys_RegisterInfoDvars()
{
    sys_configureGHz = _Dvar_RegisterFloat("sys_configureGHz", 0.0, -3.4028235e38, 3.4028235e38, 0x11u,
        "Normalized total CPU power, based on cpu type, count, and speed; used in autoconfigure");
    sys_sysMB = _Dvar_RegisterInt("sys_sysMB", 0, 0x80000000, 0x7FFFFFFF, 0x11u, "Physical memory in the system");
    sys_gpu = _Dvar_RegisterString("sys_gpu", (char *)"", 0x11u, "GPU description");
    sys_configSum = _Dvar_RegisterInt("sys_configSum", 0, 0x80000000, 0x7FFFFFFF, 0x11u, "Configuration checksum");
    sys_SSE = _Dvar_RegisterBool("sys_SSE", sys_info.SSE, 0x40u, "Operating system allows Streaming SIMD Extensions");
    _Dvar_RegisterFloat("sys_cpuGHz", sys_info.cpuGHz, -3.4028235e38, 3.4028235e38, 0x40u, "Measured CPU speed");
    _Dvar_RegisterString("sys_cpuName", sys_info.cpuName, 0x40u, "CPU name description");
}

bool __cdecl Sys_HasInfoChanged()
{
    Sys_RegisterInfoDvars();
    return false;
}

bool __cdecl Sys_ShouldUpdateForInfoChange() { return false; }

// The default of the player's Frame Rate Limit (src/qcommon/common.cpp: bo1_maxfps), which the page has (zombies-hub's
// web/public/player.js: Module.bo1MaxFpsDefault, from zombies-hub's web/src/settings.ts: DEFAULTS): 90 on mobile, every
// second refresh of a display faster than that (60 at 120 Hz, 72 at 144), to spare a phone's battery and its heat, none
// on a PC. A player's config that has the setting keeps it.
int Bo1_DefaultMaxFps()
{
    int fps = MAIN_THREAD_EM_ASM_INT({ return Module['bo1MaxFpsDefault'] | 0; });
    return fps >= 0 && fps <= 1000 ? fps : 0;
}

// Mobile's recommended settings, which the page has by the dvars' names (zombies-hub's web/public/player.js:
// Module.bo1Recommended, from zombies-hub's web/src/settings.ts: RECOMMENDED; none on a PC), over the game's, which are
// a PC's graphics card's (configure_mp.csv's row for any GPU: 4x anti-aliasing, shadows): set as the game sets its own
// (Com_SetConfigureDvars), so a first game's run of the player's config after them still keeps what the player set in
// the lobby before it (Com_CheckSetRecommended).
static void Bo1_SetPageRecommended()
{
    enum { MAX_SETTINGS = 16 };
    char list[MAX_SETTINGS * 66] = "";
    MAIN_THREAD_EM_ASM({
        const recommended = Module['bo1Recommended'];
        let list = '';
        for (const name in recommended || {})
            list += name + ' ' + recommended[name] + '\n';
        stringToUTF8(list, $0, $1);
    }, list, (int)sizeof(list));
    char names[MAX_SETTINGS][32], values[MAX_SETTINGS][32];
    int count = 0;
    for (char *line = list, *end; count < MAX_SETTINGS && (end = strchr(line, '\n')) != nullptr; line = end + 1)
    {
        *end = 0;
        char *value = strchr(line, ' ');
        if (!value || value == line || value - line >= 32 || !value[1] || strlen(value + 1) >= 32)
            continue;
        *value++ = 0;
        I_strncpyz(names[count], line, 32);
        I_strncpyz(values[count], value, 32);
        Com_Printf(16, "the page's recommended settings: %s %s\n", line, value);
        ++count;
    }
    if (count)
        Com_SetConfigureDvars(count, (const char (*)[32])names, (const char (*)[32])values);
}

// Com_SetRecommended's last call, once it has set the game's recommended settings (a player's first game, and its
// menu's Default System Settings): the page's go over them here.
void __cdecl Sys_ArchiveInfo(int checksum)
{
    Sys_RegisterInfoDvars();
    Dvar_SetFloat((dvar_s *)sys_configureGHz, sys_info.configureGHz);
    Dvar_SetInt((dvar_s *)sys_sysMB, sys_info.sysMB);
    Dvar_SetString((dvar_s *)sys_gpu, sys_info.gpuDescription);
    Dvar_SetInt((dvar_s *)sys_configSum, checksum);
    Bo1_SetPageRecommended();
}

static void Sys_WebFatal(const char *what, const char *text)
{
    Bo1_Log("%s: %s", what, text);
    Bo1_JsNotify("error", text);
    // the engine's threads stop here; the page keeps the last frame and shows the message
    for (;;)
        Sleep(1000);
}

void __cdecl Sys_DirectXFatalError() { Sys_WebFatal("Sys_DirectXFatalError", "WebGPU could not be initialized."); }

void __cdecl Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "Out of memory (%s, line %d).", filename, line);
    Sys_WebFatal("Sys_OutOfMemErrorInternal", msg);
}

void __cdecl Sys_QuitAndStartProcess(const char *exeName)
{
    I_strncpyz(sys_exitCmdLine, exeName, 1024);
    Cbuf_AddText(0, "quit\n");
}

void __cdecl Sys_OpenURL(const char *url, int)
{
    MAIN_THREAD_ASYNC_EM_ASM({ window.open(UTF8ToString($0), '_blank'); }, url);
}

void Sys_Error(char *error, ...)
{
    char string[4100];
    va_list va;
    va_start(va, error);
    _vsnprintf(string, 0x1000u, error, va);
    va_end(va);
    string[0x1000] = 0;
    Sys_EnterCriticalSection(CRITSECT_COM_ERROR);
    com_errorEntered = 1;
    Sys_WebFatal("Sys_Error", string);
}

void __cdecl Sys_Quit()
{
    Sys_EnterCriticalSection(CRITSECT_COM_ERROR);
    LiveSteam_Shutdown();
    CL_ShutdownAll();
    IN_Shutdown();
    Key_Shutdown();
    Sys_NormalExit();
    Win_ShutdownLocalization();
    Dvar_Shutdown();
    Cmd_Shutdown();
    Sys_ShutdownEvents();
    SL_Shutdown(SCRIPTINSTANCE_SERVER);
    SL_Shutdown(SCRIPTINSTANCE_CLIENT);
    Con_ShutdownChannels();
    Bo1_Exit(0);
}

void Sys_SpawnQuitProcess() {}

void __cdecl Sys_Print(char *msg)
{
    // the engine's console text, line by line, to the browser console. What the game says to its lobby
    // (src/server_mp/sv_lobby_mp.cpp: bo1_lobby) goes to the page as well, which is the lobby of a game of the page's
    // own (zombies-hub's web/public/player.js: onGame): once the line is out, and the lock released, as the page's
    // thread takes the call.
    // So does bo1_player's answer, for a page that asked for those (Bo1_PageHearsPlayer).
    static char line[4096];
    static int lineLen;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    std::vector<std::string> said;
    pthread_mutex_lock(&lock);
    for (const char *p = msg; *p; ++p)
    {
        if (*p == '^' && p[1] >= '0' && p[1] <= '9') // color codes
        {
            ++p;
            continue;
        }
        if (*p == '\n' || lineLen == (int)sizeof(line) - 1)
        {
            line[lineLen] = 0;
            emscripten_log(EM_LOG_CONSOLE, "%s", line);
            if (!strncmp(line, "bo1_lobby: ", 11) ||
                (!strncmp(line, "bo1_player: ", 12) && s_pageHearsPlayer.load(std::memory_order_acquire)))
                said.emplace_back(line);
            lineLen = 0;
            if (*p == '\n')
                continue;
        }
        line[lineLen++] = *p;
    }
    pthread_mutex_unlock(&lock);
    for (const std::string &s : said)
        Bo1_JsNotify("game", s.c_str());
}

char *__cdecl Sys_GetClipboardData() { return NULL; }

void __cdecl Sys_QueEvent(unsigned int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    sysEvent_t *ev;

    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    ev = &eventQue[(unsigned __int8)eventHead];
    if (eventHead - eventTail >= 256)
    {
        Com_Printf(16, "Sys_QueEvent: overflow\n");
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 11);
        ++eventTail;
    }
    ++eventHead;
    if (!time)
        time = Sys_Milliseconds();
    ev->evTime = time;
    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

void Sys_ShutdownEvents()
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    while (eventHead > eventTail)
    {
        sysEvent_t *ev = &eventQue[(unsigned __int8)eventTail++];
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 11);
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

sysEvent_t *__cdecl Win_GetEvent(sysEvent_t *result)
{
    sysEvent_t ev;

    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    if (eventHead <= eventTail)
    {
        if (Sys_QueryWin32QuitEvent())
        {
            Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
            Com_Quit_f();
        }
        // a line the page queued (Bo1_ConsoleCommand) runs as if typed into the console, as on Windows
        char *s = Sys_ConsoleInput();
        if (s)
        {
            int len = (int)strlen(s);
            char *b = (char *)Com_AllocEvent(len + 1);
            I_strncpyz(b, s, len);
            Sys_QueEvent(0, SE_CONSOLE, 0, 0, len + 1, b);
        }
        if (eventHead <= eventTail)
        {
            memset(&ev, 0, sizeof(ev));
            ev.evTime = Sys_Milliseconds();
        }
        else
        {
            ev = eventQue[(unsigned __int8)eventTail++];
        }
    }
    else
    {
        ev = eventQue[(unsigned __int8)eventTail++];
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    *result = ev;
    return result;
}

// The player's Full Screen setting: whether the page goes full screen (the browser's own) as a click, a key or a
// gamepad's press takes the player into the game (web/src/library_bo1.js: clickGoesFullScreen). One setting, whoever
// changes it: the lobby's options menu, in the player's config (zombies-hub's web/src/settings.ts); the game's, whose
// Full Screen row is this in the browser, live in a game (src/ui/ui_main.cpp: UI_WebFullScreen_PatchMenus), which the
// page follows into full screen or out of it; and the player who enters or leaves full screen themselves (F11, holding
// Esc), which the page tells the engine. r_fullscreen, the Windows client's exclusive full screen, stays 0 here.
static const dvar_t *bo1_web_fullscreen;

// As the game thread takes its events, each frame (the window's messages are the render thread's to pump:
// RB_SwapBuffers): what the page set comes first, then the page has the setting as it is. Not before the dvar is
// there, the config read: the page has the config's until then (zombies-hub's web/public/player.js:
// Module.bo1FullScreen).
static void Bo1_SyncFullScreen()
{
    if (!bo1_web_fullscreen)
        return;
    int set = __atomic_exchange_n(&g_bo1Input.fullScreenSet, -1, __ATOMIC_ACQ_REL);
    if (set >= 0 && (set != 0) != bo1_web_fullscreen->current.enabled)
        Dvar_SetBool((dvar_s *)bo1_web_fullscreen, set != 0);
    __atomic_store_n(&g_bo1Input.fullScreen, bo1_web_fullscreen->current.enabled ? 1 : 0, __ATOMIC_RELEASE);
}

sysEvent_t *__cdecl Sys_GetEvent(sysEvent_t *result)
{
    Bo1_SyncFullScreen();
    return Win_GetEvent(result);
}

void __cdecl Sys_LoadingKeepAlive()
{
    sysEvent_t ev;
    do
    {
        Win_GetEvent(&ev);
    } while (ev.evType);
}

void __cdecl Sys_Mjpeg() {}
void __cdecl Sys_MjpegClose() {}

void __cdecl Sys_In_Restart_f()
{
    IN_Shutdown();
    IN_Init();
}

void __cdecl Sys_Net_Restart_f() { NET_Restart(); }

cmd_function_s Sys_In_Restart_f_VAR;
cmd_function_s Sys_Net_Restart_f_VAR;

void __cdecl Sys_Init()
{
    Cmd_AddCommandInternal("in_restart", Sys_In_Restart_f, &Sys_In_Restart_f_VAR);
    Cmd_AddCommandInternal("net_restart", Sys_Net_Restart_f, &Sys_Net_Restart_f_VAR);
    bo1_web_fullscreen = _Dvar_RegisterBool("bo1_web_fullscreen", 1, 1u,
        "web client: the page goes full screen as a click, a key or a gamepad's press takes the player into the game");
    // the game's resolution, which the lobby's options menu sets and the page reads as it sizes the canvas before the
    // engine starts (zombies-hub's web/src/settings.ts: backBuffer); the engine only keeps it in the config
    _Dvar_RegisterString("bo1_web_resolution", (char *)"auto", 1u,
        "web client: the game's resolution, auto (the screen's) or WxH, from the next game on");
    // zombies: no CPU / memory / video card report: the figures are stand-ins (Sys_FindInfo), not this machine's
    IN_Init();
}

int __cdecl Sys_CheckCrashOrRerun() { return 1; }
void Sys_NoFreeFilesError() { Sys_WebFatal("Sys_NoFreeFilesError", "No free file handles."); }
int __cdecl Sys_IsGameProcess(unsigned int) { return 0; }
void __cdecl Sys_NormalExit() {}

void Sys_FindInfo()
{
    sys_info.logicalCpuCount = Sys_GetCpuCount();
    sys_info.physicalCpuCount = sys_info.logicalCpuCount;
    sys_info.cpuGHz = 3.0;
    sys_info.configureGHz = 3.0f * (float)sys_info.logicalCpuCount;
    sys_info.sysMB = 4096;
    I_strncpyz(sys_info.gpuDescription, "WebGPU", sizeof(sys_info.gpuDescription));
    sys_info.SSE = 0; // the MMX/SSE skinner (r_model_skin_sse.cpp) is not built; the scalar one runs
    I_strncpyz(sys_info.cpuVendor, "WebAssembly", sizeof(sys_info.cpuVendor));
    I_strncpyz(sys_info.cpuName, "WebAssembly", sizeof(sys_info.cpuName));
}

int Sys_GetSemaphoreFileName() { return 0; }

void Win_RegisterClass()
{
    WNDCLASSEXA wce;
    memset(&wce, 0, sizeof(wce));
    wce.cbSize = sizeof(wce);
    wce.lpfnWndProc = (WNDPROC)MainWndProc;
    wce.hInstance = g_wv.hInstance;
    wce.lpszClassName = "CoDBlackOps";
    if (!RegisterClassExA(&wce))
        Com_Error(ERR_FATAL, "EXE_ERR_COULDNT_REGISTER_WINDOW");
}

void PrintWorkingDir()
{
    char cwd[260];
    _getcwd(cwd, 256);
    Com_VerbosePrintf(16, "Working directory: %s\n", cwd);
}

char __cdecl CheckRemoteSession() { return 0; }
bool __cdecl StartingDedicatedServer(char *cmdline) { return false; }

// win_mini_dumper.cpp
void __cdecl Sys_StartMiniDump(bool) {}
bool __cdecl Sys_IsMiniDumpStarted() { return false; }

// win_syscon.cpp: no console window; its text goes to the browser console through Sys_Print
void __cdecl Sys_CreateConsole(HINSTANCE__ *) {}
void __cdecl Sys_DestroyConsole() {}
void __cdecl Sys_ShowConsole() {}

// Its input is what the page queues: Module.bo1Command(text) in zombies-hub's web/public/player.js, for tests
// (zombies-hub's e2e/game/player.ts) and DevTools. Win_GetEvent takes a line at a time, and Sys_Print echoes it as the
// console window would.
static std::mutex s_consoleLock;
static std::deque<std::string> s_consoleLines;
static char s_consoleLine[512];

extern "C" EMSCRIPTEN_KEEPALIVE void Bo1_ConsoleCommand(const char *text)
{
    std::lock_guard<std::mutex> guard(s_consoleLock);
    s_consoleLines.emplace_back(text);
}

char *__cdecl Sys_ConsoleInput()
{
    {
        std::lock_guard<std::mutex> guard(s_consoleLock);
        if (s_consoleLines.empty())
            return NULL;
        Com_sprintf(s_consoleLine, sizeof(s_consoleLine), "%s\n", s_consoleLines.front().c_str());
        s_consoleLines.pop_front();
    }
    char echo[sizeof(s_consoleLine) + 1];
    Com_sprintf(echo, sizeof(echo), "]%s", s_consoleLine);
    Sys_Print(echo);
    return s_consoleLine;
}

void __cdecl Conbuf_AppendText(char *pMsg) { Sys_Print(pMsg); }
void __cdecl Conbuf_AppendTextInMainThread(char *msg) { Sys_Print(msg); }
void __cdecl Sys_SetErrorText(const char *buf) { Bo1_JsNotify("error", buf); }

// ============================================================================================================
// file system
// ============================================================================================================

EM_JS(char *, Bo1_FetchText, (const char *url), {
    // a worker may block on XHR; the manifest is small
    var xhr = new XMLHttpRequest();
    xhr.open('GET', UTF8ToString(url), false);
    try
    {
        xhr.send(null);
    }
    catch (e)
    {
        return 0;
    }
    if (xhr.status !== 200)
        return 0;
    return stringToNewUTF8(xhr.responseText);
});

static void Bo1_MakeDirs(const std::string &path)
{
    for (size_t i = 1; i < path.size(); ++i)
    {
        if (path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0777);
    }
}

static bool Bo1_MountFileSystem()
{
    char *manifest = Bo1_FetchText("game/manifest.txt");
    if (!manifest)
    {
        Bo1_Log("game/manifest.txt is missing: open the page the server serves (zombies-hub's docker/server.sh)");
        return false;
    }
    // the version ("v" and 16 hex digits), then one relative path per line, with the file's size after a tab (which
    // the file system backend reads, web/src/library_bo1.js); a server from before versions sends only the paths
    std::string base = "game";
    char *line = manifest;
    if (line[0] == 'v' && strspn(line + 1, "0123456789abcdef") == 16 && (line[17] == '\n' || line[17] == '\r'))
    {
        base += "/" + std::string(line, 17);
        line += 17 + strspn(line + 17, "\r\n");
    }

    // /game: the install, read from the browser's store (library_bo1.js); win32_compat.cpp folds case. main/'s image
    // archives (.iwd), read in pieces all over, an image at a time (through crt_compat.cpp's cache), have a backend of
    // their own, below, so that each has a thread of its own: the images' reads don't hold up the fastfiles'. The
    // backends' chunk sizes go unused: the store reads what is asked.
    backend_t game = wasmfs_create_fetch_backend(base.c_str(), 1u << 20);
    backend_t archives = nullptr; // main/, mounted as the manifest first names it, in the case it spells it
    if (wasmfs_create_directory(BO1_WEB_GAME_ROOT, 0777, game))
    {
        Bo1_Log("could not mount %s", BO1_WEB_GAME_ROOT);
        free(manifest);
        return false;
    }
    int files = 0;
    while (*line)
    {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[--len] = 0;
        if (char *tab = strchr(line, '\t'))
        {
            *tab = 0;
            len = tab - line;
        }
        if (len)
        {
            if (!archives && len > 5 && line[4] == '/' && !strncasecmp(line, "main", 4))
            {
                std::string dir(line, 4);
                archives = wasmfs_create_fetch_backend((base + "/" + dir).c_str(), 256u << 10);
                if (wasmfs_create_directory((std::string(BO1_WEB_GAME_ROOT "/") + dir).c_str(), 0777, archives))
                    Bo1_Log("could not mount %s/%s: it is read in 1 MB chunks", BO1_WEB_GAME_ROOT, dir.c_str());
            }
            std::string path = std::string(BO1_WEB_GAME_ROOT "/") + line;
            Bo1_RegisterGameFile(line);
            Bo1_MakeDirs(path);
            int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0444);
            if (fd >= 0)
            {
                close(fd);
                ++files;
            }
        }
        if (!end)
            break;
        line = end + 1;
    }
    free(manifest);
    Bo1_Log("%d game files listed, read from %s", files, base.c_str());

    // /home: configs, profiles, logs, screenshots, in memory: each visit starts them afresh
    backend_t home = wasmfs_create_memory_backend();
    if (wasmfs_create_directory(BO1_WEB_HOME_ROOT, 0777, home))
    {
        Bo1_Log("could not mount %s", BO1_WEB_HOME_ROOT);
        return false;
    }
    mkdir(BO1_WEB_HOME_ROOT "/main", 0777);
    // /home/players: the player's own folder, as the Windows client's build\client\players\: their config.cfg, which
    // the engine writes as they change a setting or a key (Com_WriteConfiguration) and starts with
    // (Com_StartupConfigs). It is the browser's store's players/ directory (zombies-hub's web/public/store.js), through
    // Emscripten's OPFS backend, whose root library_bo1.js sets, so that their settings keep from one visit to the
    // next. Its reads and writes are proxied to the backend's own thread, which holds the store's handles: the config
    // is read as the game starts, and written once per change.
    backend_t players = wasmfs_create_opfs_backend();
    if (!players || wasmfs_create_directory(BO1_WEB_HOME_ROOT "/players", 0777, players))
    {
        Bo1_Log("could not mount %s/players in the browser's store: the player's settings keep for this visit only",
            BO1_WEB_HOME_ROOT);
        mkdir(BO1_WEB_HOME_ROOT "/players", 0777);
    }
    mkdir("/tmp", 0777);
    _chdir(BO1_WEB_GAME_ROOT);
    return true;
}

// ============================================================================================================
// main: WinMain
// ============================================================================================================

void Bo1_InitGraphics();  // d3d9_device.cpp
void Bo1_InitInput();     // web_input.cpp

int main(int argc, char **argv)
{
    s_startMs = emscripten_get_now();
    Bo1_Log("BO1 Zombies (WebAssembly) starting");
    Bo1_JsNotify("status", "Mounting the game files...");
    if (!Bo1_MountFileSystem())
    {
        Bo1_JsNotify("error", "The game files could not be found. Open the page the server serves.");
        return 1;
    }

    // the page passes the command line (zombies-hub's web/public/player.js builds it: start)
    s_cmdline[0] = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (i > 1)
            I_strncat(s_cmdline, sizeof(s_cmdline), " ");
        I_strncat(s_cmdline, sizeof(s_cmdline), argv[i]);
    }
    Bo1_Log("command line: %s", s_cmdline);
    s_logFiles = strstr(s_cmdline, "bo1_web_logfiles 1") != nullptr;
    Bo1_StartLoadingMovie();

    Bo1_InitInput();
    Bo1_InitGraphics();

    Bo1_JsNotify("status", "Starting the engine...");
    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    PMem_Init();
    track_init();
    Win_InitLocalization();
    s_nosnd = I_stristr(s_cmdline, "nosnd") != 0;

    Com_InitParse();
    Dvar_Init();
    InitTiming();
    Sys_FindInfo();
    g_wv.hInstance = (HINSTANCE)GetModuleHandleA(NULL);
    I_strncpyz(sys_cmdline, s_cmdline, 1024);
    Win_RegisterClass();
    Sys_Milliseconds();
    Sys_SetupTLCallbacks(0x900000);
    Steam_Init();
    Com_Init(sys_cmdline);
    Steam_PrintInitStatus();
    // zombies: no "readStats": SP queues it here too (0x0050A8D0), and neither exe has the command
    PrintWorkingDir();

    Bo1_JsNotify("status", "");
    for (;;)
    {
        // serial pacing: the render thread has drawn the last frame, and the page has taken it (the render thread
        // completes it then: rb_backend.cpp, RB_RenderCommandFrame)
        if (Bo1_SerialPacing())
            Sys_WaitRenderer();
        // what the frame took the game thread, less what it waited for: the render thread, the frame rate cap
        double began = emscripten_get_now(), waited = Bo1_ThreadWaitedMs();
        Com_Frame();
        double worked = emscripten_get_now() - began - (Bo1_ThreadWaitedMs() - waited);
        uint32_t us = (uint32_t)(worked > 0.0 ? worked * 1000.0 : 0.0);
        g_bo1GameUs += us;
        g_bo1GameFrameUs = us;
        ++g_bo1GameFrames;
    }
}
