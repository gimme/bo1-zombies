// Shared by the web platform layer (web/src).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <string>

// WasmFS mount points (web_main.cpp): the install, fetched on demand over HTTP, and the writable home folder
#define BO1_WEB_GAME_ROOT "/game"
#define BO1_WEB_HOME_ROOT "/home"

// milliseconds since the page started, the same clock on every thread
double Bo1_Milliseconds();
// the logical CPU count the engine sees (navigator.hardwareConcurrency, capped)
unsigned Bo1_GetCpuCount();
// the command line main() assembled from the page's URL
char *Bo1_GetCommandLine();
// the display's refresh in hertz (web_window.cpp), the same for the whole game
int Bo1_DisplayHz();
// ends the game: tells the page, then stops every thread
[[noreturn]] void Bo1_Exit(int code);
// a line in the browser console, outside the engine's own console (startup, platform errors)
void Bo1_Log(const char *fmt, ...);
// a thread has been blocked for long: log it with the call stack (a limited number of times)
void Bo1_LogStall(const char *what);

// ----- lag spikes (web_main.cpp) -----
// What the game waited for, in microseconds since the page started. Bo1_FrameEnd, at each Present, logs a frame that
// took far longer than those before it ("lag spike:"), with what was waited for during it.
extern std::atomic<uint64_t> g_bo1FileUs;     // reads of the install, on any thread: a first read fetches from the server
extern std::atomic<uint64_t> g_bo1PageUs;     // the page replaying the game's draw commands (d3d9_webgpu.cpp)
extern std::atomic<uint64_t> g_bo1CompileUs;  // of that, translating shaders and starting pipelines, which compile in the
                                              // background
extern std::atomic<uint32_t> g_bo1Compiles;   // pipelines started
extern std::atomic<uint32_t> g_bo1PipelineWaits; // draws whose pipelines were compiling (drawn with a stand-in, or left out)
extern std::atomic<uint32_t> g_bo1PipelineStandIns; // and with a stand-in pipeline (d3d9_webgpu.cpp: stand-ins)
extern std::atomic<uint32_t> g_bo1PreloadPending; // pipelines from the map's list (web/pipelines/<map>.bin) to compile
                                                  // before going in
extern std::atomic<int> g_bo1ShadowMaps; // the player's shadow maps, on (1) or off (0), which the engine says as its
                                         // renderer starts (Bo1_PipelineSettings); -1 until then
extern std::atomic<uint64_t> g_bo1WaitPageUs; // the game waiting for the page (d3d9_device.cpp): ring room, read backs,
                                              // no more than two or three frames ahead
// a read of an install file took `ms` (the slowest of a frame is named in its lag spike)
void Bo1_NoteFileRead(const char *path, double ms);
// the game presented a frame
void Bo1_FrameEnd();

// ----- the loading movie, which the page shows (bink_web.cpp; src/client/cl_cin.cpp asks after it: Sys_LoadingMovie*) -----
enum Bo1MovieState
{
    BO1_MOVIE_NONE,    // none was started
    BO1_MOVIE_PENDING, // it opens, or the page has yet to show its first picture
    BO1_MOVIE_SHOWS,   // the page shows it
    BO1_MOVIE_OVER,    // it ended, was stopped, or could not be opened
};
// the page plays main/video/<name>.bik, the first time it's asked to, its sound at volume times the movie's own (the
// player's cinematics and master volumes: R_Cinematic_PlayerVolume): true when it plays it, or plays on with it
bool Bo1_MovieStart(const char *name, float volume);
int Bo1_MovieState();
// the engine is done with it: the page takes it away
void Bo1_MovieStop();
// the page shows it under the game's canvas, which lets it through (d3d9_webgpu.cpp: Present)
bool Bo1_MovieUnder();

// ----- the frame rate overlay (Module.bo1Perf: zombies-hub's web/src/perf.ts, filled by d3d9_webgpu.cpp) -----
extern std::atomic<uint64_t> g_bo1GameUs;     // the game thread working on its frames (Com_Frame, less its waits)
extern std::atomic<uint32_t> g_bo1GameFrames; // and how many
extern std::atomic<uint32_t> g_bo1GameFrameUs; // and on the last one
// serial pacing (d3d9_device.cpp: Present): the game thread starts a frame once the page has taken the last
bool Bo1_SerialPacing();
// how long this thread has waited in Sleep and WaitFor*Object*, in milliseconds (win32_compat.cpp)
double Bo1_ThreadWaitedMs();
// Mouse lag: the game thread reads the mouse as a frame starts (GetCursorPos); the render thread draws that frame
// while the game thread builds the next; the page replays it. When the mouse last moved before the frame read it
// (Bo1InputShared::inputTime) goes with it, by the frame data the two threads hand over (GfxBackEndData).
void Bo1_FrameReadInput();   // the game thread, reading the mouse
double Bo1_FrameTakeInput(); // the render thread, starting the frame (BeginScene): 0 if the mouse had not moved
// file opens are logged when the command line has +set bo1_web_logfiles 1
bool Bo1_LogFiles();
std::string Bo1_NormalizePath(const char *path);
// The install folder as the engine sees it: /game (read only, fetched) under /home (writable). Reads of /game/X get
// /home/X when it exists; writes to /game/X go to /home/X (folders made as needed). As on Windows, where the game
// runs with fs_b and fs_h both set to the install folder (Play.bat).
std::string Bo1_ResolvePath(const char *path, bool forWrite);
// web_main.cpp registers every install file (its relative path, real case) before the engine starts; lookups
// under /game then ignore case, as on Windows. The /home overlay keeps lowercase names.
void Bo1_RegisterGameFile(const std::string &relPath);

inline void I_strncpyz_web(char *dst, const char *src, size_t size)
{
    if (!size)
        return;
    size_t i = 0;
    for (; i + 1 < size && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

// ----- the browser -> engine input queue (web_input.cpp, filled by library_bo1.js on the page's thread) -----
enum Bo1InputType : int32_t
{
    BO1_IN_MESSAGE = 1, // a window message: a = msg, b = wParam, c = lParam
    BO1_IN_FOCUS = 2,   // a = has focus
    BO1_IN_RESIZE = 3,  // a = width, b = height of the canvas back buffer
};

struct Bo1InputEvent
{
    int32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    double time;
};

#define BO1_INPUT_QUEUE_SIZE 1024

struct Bo1InputShared
{
    // written by the page thread, read by the engine (single producer, single consumer)
    volatile uint32_t head;
    volatile uint32_t tail;
    Bo1InputEvent events[BO1_INPUT_QUEUE_SIZE];
    // the virtual cursor, client coordinates of the back buffer: the page adds pointer-lock deltas or sets the
    // absolute position, SetCursorPos sets it
    volatile int32_t cursorX;
    volatile int32_t cursorY;
    volatile int32_t pointerLocked;
    volatile int32_t hasFocus;
    // written by the engine, read by the page: the mouse turns the view (CL_MouseEvent: g_wv.recenterMouse), where a
    // menu's cursor follows it otherwise; the system cursor shows (ShowCursor), which it doesn't where the engine
    // draws its own; and the mouse is no one's, turning nothing with no cursor drawn for it, over the loading movie as
    // the game loads (Sys_MouseIdle), where a click captures it all the same, hiding the system's
    volatile int32_t mouseLook;
    volatile int32_t cursorShown;
    volatile int32_t mouseIdle;
    // The player's Full Screen setting (web_main.cpp: bo1_web_fullscreen). fullScreen: as the engine has it, written
    // by the engine, which the page starts with as the player's config has it and follows into full screen or out of
    // it as the game's menu changes it. fullScreenSet: written by the page as the player enters or leaves full screen
    // themselves, 1 or 0, and -1 once the engine has taken it.
    volatile int32_t fullScreen;
    volatile int32_t fullScreenSet;
    volatile int32_t keyState[256]; // VK down states for GetKeyState / GetAsyncKeyState
    volatile int32_t width;
    volatile int32_t height;
    // when the mouse last moved the cursor (its event's timeStamp, on emscripten_get_now's clock)
    volatile double inputTime;
};

extern Bo1InputShared g_bo1Input;
