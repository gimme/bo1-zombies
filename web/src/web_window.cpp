// user32 / gdi32 for the WebAssembly build: one window (the page's canvas), its message queue and the cursor.
//
// The engine creates its game window on the render thread (R_CreateGameWindow) and pumps its messages there
// (RB_RenderThread). The page queues browser input as Windows messages (library_bo1.js -> g_bo1Input); PeekMessage
// moves them into this queue, and DispatchMessage hands them to the engine's MainWndProc, so keys, mouse buttons,
// the wheel and focus changes take the same path as on Windows. The cursor is virtual: in pointer lock the page adds
// the mouse deltas to it, otherwise it follows the mouse over the canvas; SetCursorPos (the engine's recentering)
// sets it.
#include <windows.h>

#include <deque>
#include <map>
#include <mutex>
#include <string>

#include <emscripten.h>

#include <win32/win_wndproc.h>

#include "web_platform.h"

Bo1InputShared g_bo1Input;

extern "C" EMSCRIPTEN_KEEPALIVE Bo1InputShared *Bo1_GetInputShared() { return &g_bo1Input; }

struct Bo1Window
{
    WNDPROC proc = nullptr;
    LONG style = 0;
    LONG exStyle = 0;
    LONG_PTR userData = 0;
    bool visible = false;
    std::string title;
};

static std::mutex s_windowLock;
static std::map<std::string, WNDPROC> s_classes;
static std::map<HWND, Bo1Window> s_windows;
static HWND s_mainWindow;
static uintptr_t s_nextHwnd = 0x10010;
static std::deque<MSG> s_messages;
static int s_showCursorCount;

static WNDPROC Bo1_WindowProc(HWND hWnd)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    auto it = s_windows.find(hWnd);
    return it != s_windows.end() ? it->second.proc : nullptr;
}

static LRESULT Bo1_Send(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WNDPROC proc = Bo1_WindowProc(hWnd);
    return proc ? proc(hWnd, msg, wParam, lParam) : 0;
}

static void Bo1_Post(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD time)
{
    MSG m = {};
    m.hwnd = hWnd;
    m.message = msg;
    m.wParam = wParam;
    m.lParam = lParam;
    m.time = time;
    m.pt.x = g_bo1Input.cursorX;
    m.pt.y = g_bo1Input.cursorY;
    std::lock_guard<std::mutex> guard(s_windowLock);
    s_messages.push_back(m);
}

// drain the page's queue into the window's message queue
static void Bo1_PumpInput()
{
    uint32_t head = __atomic_load_n(&g_bo1Input.head, __ATOMIC_ACQUIRE);
    uint32_t tail = g_bo1Input.tail;
    while (tail != head)
    {
        const Bo1InputEvent &ev = g_bo1Input.events[tail % BO1_INPUT_QUEUE_SIZE];
        DWORD time = timeGetTime();
        HWND hwnd = s_mainWindow;
        switch (ev.type)
        {
        case BO1_IN_MESSAGE:
            if (hwnd)
                Bo1_Post(hwnd, (UINT)ev.a, (WPARAM)(uint32_t)ev.b, (LPARAM)ev.c, time);
            break;
        case BO1_IN_FOCUS:
            if (hwnd)
            {
                Bo1_Post(hwnd, WM_ACTIVATE, ev.a ? WA_ACTIVE : WA_INACTIVE, 0, time);
                Bo1_Post(hwnd, ev.a ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0, time);
            }
            break;
        default:
            break;
        }
        ++tail;
    }
    __atomic_store_n(&g_bo1Input.tail, tail, __ATOMIC_RELEASE);
}

static bool Bo1_TakeMessage(LPMSG lpMsg, bool remove)
{
    Bo1_PumpInput();
    std::lock_guard<std::mutex> guard(s_windowLock);
    if (s_messages.empty())
        return false;
    *lpMsg = s_messages.front();
    if (remove)
        s_messages.pop_front();
    return true;
}

extern "C" BOOL WINAPI PeekMessageA(LPMSG lpMsg, HWND, UINT, UINT, UINT wRemoveMsg)
{
    return Bo1_TakeMessage(lpMsg, (wRemoveMsg & PM_REMOVE) != 0);
}

extern "C" BOOL WINAPI GetMessageA(LPMSG lpMsg, HWND, UINT, UINT)
{
    while (!Bo1_TakeMessage(lpMsg, true))
        Sleep(5);
    return lpMsg->message != WM_QUIT;
}

// the page sends WM_CHAR itself (from KeyboardEvent.key), so there is nothing to translate
extern "C" BOOL WINAPI TranslateMessage(const MSG *) { return FALSE; }

extern "C" LRESULT WINAPI DispatchMessageA(const MSG *lpMsg)
{
    if (!lpMsg->hwnd)
        return 0;
    return Bo1_Send(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

extern "C" BOOL WINAPI PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    Bo1_Post(hWnd ? hWnd : s_mainWindow, Msg, wParam, lParam, timeGetTime());
    return TRUE;
}

extern "C" LRESULT WINAPI SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) { return Bo1_Send(hWnd, Msg, wParam, lParam); }
extern "C" BOOL WINAPI PostThreadMessageA(DWORD, UINT Msg, WPARAM wParam, LPARAM lParam) { return PostMessageA(NULL, Msg, wParam, lParam); }
extern "C" void WINAPI PostQuitMessage(int nExitCode) { Bo1_Post(NULL, WM_QUIT, (WPARAM)nExitCode, 0, timeGetTime()); }
extern "C" DWORD WINAPI GetMessageTime(void) { return timeGetTime(); }

extern "C" LRESULT WINAPI DefWindowProcA(HWND hWnd, UINT Msg, WPARAM, LPARAM)
{
    if (Msg == WM_CLOSE)
        DestroyWindow(hWnd);
    return 0;
}

extern "C" LRESULT WINAPI CallWindowProcA(WNDPROC proc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    return proc ? proc(hWnd, Msg, wParam, lParam) : 0;
}

extern "C" ATOM WINAPI RegisterClassA(const WNDCLASSA *wc)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    s_classes[wc->lpszClassName] = wc->lpfnWndProc;
    return 1;
}

extern "C" ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *wc)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    s_classes[wc->lpszClassName] = wc->lpfnWndProc;
    return 1;
}

extern "C" BOOL WINAPI UnregisterClassA(LPCSTR lpClassName, HINSTANCE)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    s_classes.erase(lpClassName);
    return TRUE;
}

extern "C" HWND WINAPI CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int, int, int, int, HWND, HMENU,
    HINSTANCE, LPVOID lpParam)
{
    HWND hwnd;
    WNDPROC proc = nullptr;
    {
        std::lock_guard<std::mutex> guard(s_windowLock);
        auto it = s_classes.find(lpClassName ? lpClassName : "");
        if (it != s_classes.end())
            proc = it->second;
        hwnd = (HWND)(s_nextHwnd += 0x10);
        Bo1Window &w = s_windows[hwnd];
        w.proc = proc;
        w.style = (LONG)dwStyle;
        w.exStyle = (LONG)dwExStyle;
        w.visible = (dwStyle & WS_VISIBLE) != 0;
        w.title = lpWindowName ? lpWindowName : "";
        // the game window is the one with a window procedure ("CoDBlackOps"); helper windows ("static") have none
        if (proc)
            s_mainWindow = hwnd;
    }
    if (proc)
    {
        CREATESTRUCTA cs = {};
        cs.lpCreateParams = lpParam;
        cs.style = (LONG)dwStyle;
        cs.dwExStyle = dwExStyle;
        cs.lpszClass = lpClassName;
        cs.lpszName = lpWindowName;
        cs.cx = g_bo1Input.width;
        cs.cy = g_bo1Input.height;
        proc(hwnd, WM_CREATE, 0, (LPARAM)&cs);
        if (g_bo1Input.hasFocus)
            Bo1_Post(hwnd, WM_ACTIVATE, WA_ACTIVE, 0, timeGetTime());
    }
    if (lpWindowName && proc)
        MAIN_THREAD_EM_ASM({ document.title = UTF8ToString($0); }, lpWindowName);
    return hwnd;
}

extern "C" BOOL WINAPI DestroyWindow(HWND hWnd)
{
    if (!Bo1_WindowProc(hWnd))
    {
        std::lock_guard<std::mutex> guard(s_windowLock);
        return s_windows.erase(hWnd) != 0;
    }
    Bo1_Send(hWnd, WM_DESTROY, 0, 0);
    std::lock_guard<std::mutex> guard(s_windowLock);
    s_windows.erase(hWnd);
    if (s_mainWindow == hWnd)
        s_mainWindow = NULL;
    return TRUE;
}

extern "C" BOOL WINAPI IsWindow(HWND hWnd)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    return s_windows.count(hWnd) != 0;
}

extern "C" BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    auto it = s_windows.find(hWnd);
    if (it == s_windows.end())
        return FALSE;
    bool was = it->second.visible;
    it->second.visible = nCmdShow != SW_HIDE;
    return was;
}

extern "C" BOOL WINAPI IsWindowVisible(HWND hWnd)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    auto it = s_windows.find(hWnd);
    return it != s_windows.end() && it->second.visible;
}

extern "C" BOOL WINAPI UpdateWindow(HWND) { return TRUE; }
extern "C" BOOL WINAPI IsIconic(HWND) { return FALSE; }
extern "C" BOOL WINAPI IsZoomed(HWND) { return FALSE; }
extern "C" HWND WINAPI FindWindowA(LPCSTR, LPCSTR) { return NULL; }
extern "C" HWND WINAPI GetDesktopWindow(void) { return (HWND)0x10000; }

// the page's focus stands for the foreground window
extern "C" HWND WINAPI GetForegroundWindow(void) { return g_bo1Input.hasFocus ? s_mainWindow : NULL; }
extern "C" BOOL WINAPI SetForegroundWindow(HWND) { return TRUE; }
extern "C" HWND WINAPI GetActiveWindow(void) { return s_mainWindow; }
extern "C" HWND WINAPI SetActiveWindow(HWND h) { return s_mainWindow; }
extern "C" HWND WINAPI GetFocus(void) { return g_bo1Input.hasFocus ? s_mainWindow : NULL; }
extern "C" HWND WINAPI SetFocus(HWND) { return s_mainWindow; }
extern "C" BOOL WINAPI SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
extern "C" BOOL WINAPI MoveWindow(HWND, int, int, int, int, BOOL) { return TRUE; }

// the window fills the canvas and sits at the origin of the "screen"
extern "C" BOOL WINAPI GetWindowRect(HWND, LPRECT r)
{
    r->left = 0;
    r->top = 0;
    r->right = g_bo1Input.width;
    r->bottom = g_bo1Input.height;
    return TRUE;
}

extern "C" BOOL WINAPI GetClientRect(HWND h, LPRECT r) { return GetWindowRect(h, r); }
extern "C" BOOL WINAPI AdjustWindowRect(LPRECT, DWORD, BOOL) { return TRUE; }
extern "C" BOOL WINAPI AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }
extern "C" BOOL WINAPI ClientToScreen(HWND, LPPOINT) { return TRUE; }
extern "C" BOOL WINAPI ScreenToClient(HWND, LPPOINT) { return TRUE; }

extern "C" BOOL WINAPI EqualRect(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

static int s_readCursorX, s_readCursorY; // what GetCursorPos returned last

extern "C" BOOL WINAPI GetCursorPos(LPPOINT p)
{
    // the time first: the page moves the cursor before it stamps the movement
    Bo1_FrameReadInput();
    // whether the mouse turned the view last frame, for the page to capture it then (IN_MouseMove reads the cursor
    // each frame, before CL_MouseEvent says again)
    __atomic_store_n(&g_bo1Input.mouseLook, g_wv.recenterMouse ? 1 : 0, __ATOMIC_RELEASE);
    p->x = s_readCursorX = __atomic_load_n(&g_bo1Input.cursorX, __ATOMIC_ACQUIRE);
    p->y = s_readCursorY = __atomic_load_n(&g_bo1Input.cursorY, __ATOMIC_ACQUIRE);
    return TRUE;
}

extern "C" BOOL WINAPI SetCursorPos(int X, int Y)
{
    if (__atomic_load_n(&g_bo1Input.pointerLocked, __ATOMIC_ACQUIRE))
    {
        // recentering under pointer lock: move by the offset from the last read, keeping the deltas the page added
        // since then
        __atomic_add_fetch(&g_bo1Input.cursorX, X - s_readCursorX, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&g_bo1Input.cursorY, Y - s_readCursorY, __ATOMIC_ACQ_REL);
        s_readCursorX = X;
        s_readCursorY = Y;
        return TRUE;
    }
    __atomic_store_n(&g_bo1Input.cursorX, X, __ATOMIC_RELEASE);
    __atomic_store_n(&g_bo1Input.cursorY, Y, __ATOMIC_RELEASE);
    return TRUE;
}

extern "C" int WINAPI ShowCursor(BOOL bShow)
{
    // the system cursor: hidden while the engine draws its own (the page hides it over the canvas then) or the page holds
    // pointer lock
    int count = bShow ? ++s_showCursorCount : --s_showCursorCount;
    __atomic_store_n(&g_bo1Input.cursorShown, count >= 0 ? 1 : 0, __ATOMIC_RELEASE);
    return count;
}

extern "C" HCURSOR WINAPI SetCursor(HCURSOR c) { return c; }
extern "C" HCURSOR WINAPI LoadCursorA(HINSTANCE, LPCSTR) { return (HCURSOR)1; }
extern "C" HICON WINAPI LoadIconA(HINSTANCE, LPCSTR) { return (HICON)1; }
extern "C" HANDLE WINAPI LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return NULL; }
extern "C" BOOL WINAPI ClipCursor(const RECT *) { return TRUE; }
extern "C" HWND WINAPI SetCapture(HWND h) { return NULL; }
extern "C" BOOL WINAPI ReleaseCapture(void) { return TRUE; }
extern "C" HWND WINAPI GetCapture(void) { return NULL; }
extern "C" BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT) { return TRUE; }
extern "C" HDC WINAPI GetDC(HWND) { return (HDC)1; }
extern "C" int WINAPI ReleaseDC(HWND, HDC) { return 1; }

extern "C" int WINAPI GetDeviceCaps(HDC, int index)
{
    switch (index)
    {
    case HORZRES:
        return g_bo1Input.width;
    case VERTRES:
        return g_bo1Input.height;
    case BITSPIXEL:
        return 32;
    case VREFRESH:
        return Bo1_DisplayHz();
    case LOGPIXELSX:
    case LOGPIXELSY:
        return 96;
    default:
        return 0;
    }
}

extern "C" BOOL WINAPI SetWindowTextA(HWND, LPCSTR) { return TRUE; }
extern "C" int WINAPI GetWindowTextA(HWND, LPSTR s, int n)
{
    if (n)
        s[0] = 0;
    return 0;
}

extern "C" LONG WINAPI GetWindowLongA(HWND hWnd, int nIndex)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    auto it = s_windows.find(hWnd);
    if (it == s_windows.end())
        return 0;
    switch (nIndex)
    {
    case GWL_STYLE:
        return it->second.style;
    case GWL_EXSTYLE:
        return it->second.exStyle;
    case GWL_USERDATA:
        return (LONG)it->second.userData;
    case GWL_WNDPROC:
        return (LONG)(LONG_PTR)it->second.proc;
    default:
        return 0;
    }
}

extern "C" LONG WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
    std::lock_guard<std::mutex> guard(s_windowLock);
    auto it = s_windows.find(hWnd);
    if (it == s_windows.end())
        return 0;
    LONG old = 0;
    switch (nIndex)
    {
    case GWL_STYLE:
        old = it->second.style;
        it->second.style = dwNewLong;
        break;
    case GWL_EXSTYLE:
        old = it->second.exStyle;
        it->second.exStyle = dwNewLong;
        break;
    case GWL_USERDATA:
        old = (LONG)it->second.userData;
        it->second.userData = dwNewLong;
        break;
    case GWL_WNDPROC:
        old = (LONG)(LONG_PTR)it->second.proc;
        it->second.proc = (WNDPROC)(LONG_PTR)dwNewLong;
        break;
    }
    return old;
}

extern "C" BOOL WINAPI EnumWindows(WNDENUMPROC, LPARAM) { return TRUE; }
extern "C" BOOL WINAPI EnumThreadWindows(DWORD, WNDENUMPROC, LPARAM) { return TRUE; }
extern "C" DWORD WINAPI GetWindowThreadProcessId(HWND, LPDWORD pid)
{
    if (pid)
        *pid = GetCurrentProcessId();
    return 0;
}

// ----- display: one monitor, the size of the canvas -----

// The display's refresh, as the page measured it before the game started (zombies-hub's web/public/player.js:
// Module.bo1RefreshHz), which is the one display mode's (d3d9_device.cpp: EnumAdapterModes) and the desktop's
// (EnumDisplaySettingsA, below): the Graphics menu's Frame Rate Limit goes up to it (src/ui/ui_main.cpp:
// UI_FrameLimit_PatchMenus). 60 where the page has none. Read once, as the game takes its modes, so that the display
// and the modes agree all game.
int Bo1_DisplayHz()
{
    static std::atomic<int> s_hz;
    int hz = s_hz.load(std::memory_order_relaxed);
    if (!hz)
    {
        int measured = MAIN_THREAD_EM_ASM_INT({
            const hz = typeof Module['bo1RefreshHz'] === 'function' ? Module['bo1RefreshHz']() : 0;
            return hz >= 24 && hz <= 500 ? hz : 0;
        });
        hz = measured ? measured : 60;
        int none = 0;
        if (!s_hz.compare_exchange_strong(none, hz))
            hz = none;
        else
            Bo1_Log("display: %d Hz%s", hz, measured ? ", as the page measured it" : " (the page measured none)");
    }
    return hz;
}

extern "C" HMONITOR WINAPI MonitorFromWindow(HWND, DWORD) { return (HMONITOR)0x20000; }
extern "C" HMONITOR WINAPI MonitorFromPoint(POINT, DWORD) { return (HMONITOR)0x20000; }
extern "C" HMONITOR WINAPI MonitorFromRect(LPCRECT, DWORD) { return (HMONITOR)0x20000; }

extern "C" BOOL WINAPI GetMonitorInfoA(HMONITOR, LPMONITORINFO mi)
{
    mi->rcMonitor.left = 0;
    mi->rcMonitor.top = 0;
    mi->rcMonitor.right = g_bo1Input.width;
    mi->rcMonitor.bottom = g_bo1Input.height;
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = MONITORINFOF_PRIMARY;
    if (mi->cbSize >= sizeof(MONITORINFOEXA))
        strcpy(((MONITORINFOEXA *)mi)->szDevice, "\\\\.\\DISPLAY1");
    return TRUE;
}

extern "C" BOOL WINAPI EnumDisplayMonitors(HDC, LPCRECT, MONITORENUMPROC fn, LPARAM data)
{
    RECT r = {0, 0, g_bo1Input.width, g_bo1Input.height};
    fn((HMONITOR)0x20000, NULL, &r, data);
    return TRUE;
}

extern "C" BOOL WINAPI EnumDisplaySettingsA(LPCSTR, DWORD iModeNum, DEVMODEA *dm)
{
    if (iModeNum != ENUM_CURRENT_SETTINGS && iModeNum != ENUM_REGISTRY_SETTINGS && iModeNum != 0)
        return FALSE;
    memset(dm, 0, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    dm->dmBitsPerPel = 32;
    dm->dmPelsWidth = g_bo1Input.width;
    dm->dmPelsHeight = g_bo1Input.height;
    dm->dmDisplayFrequency = Bo1_DisplayHz();
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    return TRUE;
}

extern "C" LONG WINAPI ChangeDisplaySettingsA(DEVMODEA *, DWORD) { return DISP_CHANGE_SUCCESSFUL; }

extern "C" int WINAPI GetSystemMetrics(int nIndex)
{
    switch (nIndex)
    {
    case SM_CXSCREEN:
    case SM_CXVIRTUALSCREEN:
        return g_bo1Input.width;
    case SM_CYSCREEN:
    case SM_CYVIRTUALSCREEN:
        return g_bo1Input.height;
    case SM_CMONITORS:
        return 1;
    default:
        return 0;
    }
}

extern "C" BOOL WINAPI SystemParametersInfoA(UINT uiAction, UINT, PVOID pvParam, UINT)
{
    if (uiAction == SPI_GETWORKAREA && pvParam)
    {
        RECT *r = (RECT *)pvParam;
        r->left = 0;
        r->top = 0;
        r->right = g_bo1Input.width;
        r->bottom = g_bo1Input.height;
    }
    return TRUE;
}

extern "C" BOOL WINAPI InvalidateRect(HWND, const RECT *, BOOL) { return TRUE; }
extern "C" BOOL WINAPI ValidateRect(HWND, const RECT *) { return TRUE; }
extern "C" BOOL WINAPI FlashWindow(HWND, BOOL) { return TRUE; }
extern "C" HGDIOBJ WINAPI GetStockObject(int) { return (HGDIOBJ)1; }
extern "C" HBRUSH WINAPI CreateSolidBrush(COLORREF) { return (HBRUSH)1; }
extern "C" HGDIOBJ WINAPI SelectObject(HDC, HGDIOBJ) { return (HGDIOBJ)1; }
extern "C" BOOL WINAPI DeleteObject(HGDIOBJ) { return TRUE; }
extern "C" BOOL WINAPI DeleteDC(HDC) { return TRUE; }
extern "C" HDC WINAPI CreateCompatibleDC(HDC) { return (HDC)1; }
extern "C" BOOL WINAPI SetDeviceGammaRamp(HDC, LPVOID) { return FALSE; }
extern "C" BOOL WINAPI GetDeviceGammaRamp(HDC, LPVOID) { return FALSE; }
extern "C" UINT_PTR WINAPI SetTimer(HWND, UINT_PTR id, UINT, void *) { return id ? id : 1; }
extern "C" BOOL WINAPI KillTimer(HWND, UINT_PTR) { return TRUE; }
extern "C" HINSTANCE WINAPI ShellExecuteA(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT) { return (HINSTANCE)2; }
extern "C" EXECUTION_STATE WINAPI SetThreadExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }

extern "C" UINT WINAPI RegisterWindowMessageA(LPCSTR)
{
    // a private message number no input event ever uses (MSH_MOUSEWHEEL)
    return 0xC123;
}

extern "C" HHOOK WINAPI SetWindowsHookExA(int, HOOKPROC, HINSTANCE, DWORD) { return NULL; }
extern "C" BOOL WINAPI UnhookWindowsHookEx(HHOOK) { return TRUE; }
extern "C" LRESULT WINAPI CallNextHookEx(HHOOK, int, WPARAM, LPARAM) { return 0; }

extern "C" int WINAPI MessageBoxA(HWND, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
    Bo1_Log("MessageBox \"%s\": %s", lpCaption ? lpCaption : "", lpText ? lpText : "");
    // yes/no questions (config changed, safe mode) take the safe default
    switch (uType & 0xF)
    {
    case MB_YESNO:
    case MB_YESNOCANCEL:
        return IDNO;
    case MB_OKCANCEL:
    case MB_RETRYCANCEL:
        return IDCANCEL;
    default:
        return IDOK;
    }
}

// ----- clipboard: empty (reading the page's clipboard needs a user gesture and a promise) -----

extern "C" BOOL WINAPI OpenClipboard(HWND) { return TRUE; }
extern "C" HANDLE WINAPI GetClipboardData(UINT) { return NULL; }
extern "C" HANDLE WINAPI SetClipboardData(UINT, HANDLE h) { return h; }
extern "C" BOOL WINAPI EmptyClipboard(void) { return TRUE; }
extern "C" BOOL WINAPI CloseClipboard(void) { return TRUE; }

// ----- keyboard state -----

extern "C" SHORT WINAPI GetKeyState(int nVirtKey)
{
    return (nVirtKey >= 0 && nVirtKey < 256 && g_bo1Input.keyState[nVirtKey]) ? (SHORT)0x8000 : 0;
}

extern "C" SHORT WINAPI GetAsyncKeyState(int vKey) { return GetKeyState(vKey); }

extern "C" BOOL WINAPI GetKeyboardState(PBYTE lpKeyState)
{
    for (int i = 0; i < 256; ++i)
        lpKeyState[i] = g_bo1Input.keyState[i] ? 0x80 : 0;
    return TRUE;
}

// MAPVK_VK_TO_CHAR for a US layout: what MapKey falls back to for keys virtualKeyConvert leaves at 0
extern "C" UINT WINAPI MapVirtualKeyA(UINT uCode, UINT uMapType)
{
    if (uMapType != MAPVK_VK_TO_CHAR)
        return 0;
    if ((uCode >= '0' && uCode <= '9') || (uCode >= 'A' && uCode <= 'Z'))
        return uCode;
    switch (uCode)
    {
    case VK_OEM_1:
        return ';';
    case VK_OEM_PLUS:
        return '=';
    case VK_OEM_COMMA:
        return ',';
    case VK_OEM_MINUS:
        return '-';
    case VK_OEM_PERIOD:
        return '.';
    case VK_OEM_2:
        return '/';
    case VK_OEM_3:
        return '`';
    case VK_OEM_4:
        return '[';
    case VK_OEM_5:
        return '\\';
    case VK_OEM_6:
        return ']';
    case VK_OEM_7:
        return '\'';
    case VK_SPACE:
        return ' ';
    default:
        return 0;
    }
}

extern "C" UINT WINAPI MapVirtualKeyExA(UINT uCode, UINT uMapType, HKL) { return MapVirtualKeyA(uCode, uMapType); }
extern "C" HKL WINAPI GetKeyboardLayout(DWORD) { return (HKL)0x04090409; }
extern "C" int WINAPI GetKeyboardLayoutList(int n, HKL *list)
{
    if (n && list)
        list[0] = (HKL)0x04090409;
    return 1;
}
extern "C" int WINAPI ToAscii(UINT vk, UINT, const BYTE *, LPWORD ch, UINT)
{
    UINT c = MapVirtualKeyA(vk, MAPVK_VK_TO_CHAR);
    if (!c)
        return 0;
    *ch = (WORD)c;
    return 1;
}
extern "C" int WINAPI ToAsciiEx(UINT vk, UINT sc, const BYTE *ks, LPWORD ch, UINT f, HKL) { return ToAscii(vk, sc, ks, ch, f); }

// raw input is not used: the cursor deltas reach the engine through GetCursorPos
extern "C" BOOL WINAPI RegisterRawInputDevices(PCRAWINPUTDEVICE, UINT, UINT) { return FALSE; }
extern "C" UINT WINAPI GetRawInputData(HRAWINPUT, UINT, LPVOID, PUINT, UINT) { return (UINT)-1; }
