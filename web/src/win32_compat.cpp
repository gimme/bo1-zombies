// Win32 for the WebAssembly build: kernel32 (threads, waits, critical sections, time, memory, files), the parts of
// user32 the engine's window code calls, and advapi32 / winmm / winsock leftovers.
//
// Threads are pthreads (Web Workers). Every engine thread runs in a worker (PROXY_TO_PTHREAD), so blocking waits are
// real futex waits. Events, mutexes, semaphores and thread handles share one lock and condition variable: Windows
// lets a thread wait for several of them at once, and the engine signals them a few hundred times a frame at most.
//
// Files go through the WasmFS tree web_main.cpp builds: /game (the install, fetched over HTTP on demand) and /home
// (writable). Windows paths are normalized here ('\' -> '/'); the CRT calls the engine makes with such paths are
// routed through the same normalization by bo1_web_prelude.h.
//
// There is one window, the canvas. Its messages (keys, mouse buttons, focus, close) come from the browser through
// web_input.cpp's queue and reach the engine's own MainWndProc through PeekMessage / DispatchMessage, as on Windows;
// the cursor is virtual (pointer-lock deltas move it, SetCursorPos sets it).
#include <windows.h>
#include <process.h>
#include <io.h>
#include <shlobj.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <xinput.h>
#include <dsound.h>

#include <atomic>
#include <unordered_map>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include "web_platform.h"

// ============================================================================================================
// handles
// ============================================================================================================

enum Bo1HandleType
{
    BO1H_EVENT = 1,
    BO1H_MUTEX,
    BO1H_SEMAPHORE,
    BO1H_THREAD,
    BO1H_FILE,
    BO1H_FIND,
    BO1H_SNAPSHOT,
};

static const uint32_t BO1H_MAGIC = 0xB01D0A11;

struct Bo1Handle
{
    uint32_t magic = BO1H_MAGIC;
    Bo1HandleType type;
    explicit Bo1Handle(Bo1HandleType t) : type(t) {}
    virtual ~Bo1Handle() { magic = 0; }
};

struct Bo1Event : Bo1Handle
{
    bool manualReset;
    bool signaled;
    Bo1Event(bool manual, bool initial) : Bo1Handle(BO1H_EVENT), manualReset(manual), signaled(initial) {}
};

struct Bo1Mutex : Bo1Handle
{
    DWORD owner = 0;
    int recursion = 0;
    Bo1Mutex() : Bo1Handle(BO1H_MUTEX) {}
};

struct Bo1Semaphore : Bo1Handle
{
    LONG count;
    LONG maxCount;
    Bo1Semaphore(LONG c, LONG m) : Bo1Handle(BO1H_SEMAPHORE), count(c), maxCount(m) {}
};

struct Bo1Thread : Bo1Handle
{
    pthread_t pthread = 0;
    DWORD id = 0;
    LPTHREAD_START_ROUTINE start = nullptr;
    LPVOID param = nullptr;
    int suspendCount = 0;
    bool finished = false;
    DWORD exitCode = STILL_ACTIVE;
    std::atomic<int> refs{1}; // the handle and the running thread
    Bo1Thread() : Bo1Handle(BO1H_THREAD) {}
};

struct Bo1File : Bo1Handle
{
    int fd = -1;
    bool overlapped = false;
    std::string path;
    Bo1File() : Bo1Handle(BO1H_FILE) {}
};

struct Bo1Find : Bo1Handle
{
    std::vector<std::string> names;
    std::vector<std::string> dirs; // per name: the folder it was found in
    size_t next = 0;
    Bo1Find() : Bo1Handle(BO1H_FIND) {}
};

static Bo1Handle *Bo1_GetHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return nullptr;
    Bo1Handle *obj = (Bo1Handle *)h;
    if (obj->magic != BO1H_MAGIC)
        return nullptr;
    return obj;
}

template <class T> static T *Bo1_GetHandleAs(HANDLE h, Bo1HandleType type)
{
    Bo1Handle *obj = Bo1_GetHandle(h);
    return obj && obj->type == type ? static_cast<T *>(obj) : nullptr;
}

// one lock + condition for every waitable object (see the top of the file)
static pthread_mutex_t s_syncLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_syncCond = PTHREAD_COND_INITIALIZER;

static thread_local DWORD t_lastError;
static thread_local DWORD t_threadId;
static thread_local Bo1Thread *t_thread;
static std::atomic<DWORD> s_nextThreadId{0x1000};

extern "C" DWORD WINAPI GetLastError(void) { return t_lastError; }
extern "C" void WINAPI SetLastError(DWORD err) { t_lastError = err; }

static void Bo1_SetErrnoError()
{
    switch (errno)
    {
    case ENOENT:
        t_lastError = ERROR_FILE_NOT_FOUND;
        break;
    case ENOTDIR:
        t_lastError = ERROR_PATH_NOT_FOUND;
        break;
    case EEXIST:
        t_lastError = ERROR_ALREADY_EXISTS;
        break;
    case EACCES:
    case EPERM:
        t_lastError = ERROR_ACCESS_DENIED;
        break;
    case ENOSPC:
        t_lastError = ERROR_DISK_FULL;
        break;
    default:
        t_lastError = ERROR_INVALID_FUNCTION;
        break;
    }
}

// ============================================================================================================
// threads
// ============================================================================================================

extern "C" DWORD WINAPI GetCurrentThreadId(void)
{
    if (!t_threadId)
        t_threadId = s_nextThreadId++;
    return t_threadId;
}

extern "C" DWORD WINAPI GetCurrentProcessId(void) { return 4242; }
extern "C" HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(LONG_PTR)-1; }
extern "C" HANDLE WINAPI GetCurrentThread(void) { return (HANDLE)(LONG_PTR)-2; }

static void Bo1_ReleaseThread(Bo1Thread *t)
{
    if (--t->refs == 0)
        delete t;
}

static void *Bo1_ThreadEntry(void *arg)
{
    Bo1Thread *t = (Bo1Thread *)arg;
    t_thread = t;
    t_threadId = t->id;
    pthread_mutex_lock(&s_syncLock);
    while (t->suspendCount > 0)
        pthread_cond_wait(&s_syncCond, &s_syncLock);
    pthread_mutex_unlock(&s_syncLock);

    DWORD code = t->start(t->param);

    pthread_mutex_lock(&s_syncLock);
    t->finished = true;
    t->exitCode = code;
    pthread_cond_broadcast(&s_syncCond);
    pthread_mutex_unlock(&s_syncLock);
    Bo1_ReleaseThread(t);
    return nullptr;
}

extern "C" HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
    DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    Bo1Thread *t = new Bo1Thread;
    t->start = lpStartAddress;
    t->param = lpParameter;
    t->id = s_nextThreadId++;
    t->suspendCount = (dwCreationFlags & CREATE_SUSPENDED) ? 1 : 0;
    t->refs = 2;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // the engine's threads keep large arrays on the stack (MSVC's default reserve is 1 MB, several ask for more)
    SIZE_T stack = dwStackSize > (4u << 20) ? dwStackSize : (4u << 20);
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&t->pthread, &attr, Bo1_ThreadEntry, t);
    pthread_attr_destroy(&attr);
    if (err)
    {
        Bo1_Log("CreateThread: pthread_create failed (%d)", err);
        delete t;
        t_lastError = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (lpThreadId)
        *lpThreadId = t->id;
    return (HANDLE)t;
}

extern "C" uintptr_t _beginthreadex(void *, unsigned stack_size, unsigned(__stdcall *start_address)(void *), void *arglist, unsigned initflag,
    unsigned *thrdaddr)
{
    DWORD id = 0;
    HANDLE h = CreateThread(NULL, stack_size, (LPTHREAD_START_ROUTINE)start_address, arglist, initflag, &id);
    if (thrdaddr)
        *thrdaddr = id;
    return (uintptr_t)h;
}

extern "C" void _endthreadex(unsigned retval) { ExitThread(retval); }

extern "C" void WINAPI ExitThread(DWORD dwExitCode)
{
    if (Bo1Thread *t = t_thread)
    {
        pthread_mutex_lock(&s_syncLock);
        t->finished = true;
        t->exitCode = dwExitCode;
        pthread_cond_broadcast(&s_syncCond);
        pthread_mutex_unlock(&s_syncLock);
        Bo1_ReleaseThread(t);
    }
    pthread_exit(nullptr);
}

extern "C" DWORD WINAPI ResumeThread(HANDLE hThread)
{
    Bo1Thread *t = Bo1_GetHandleAs<Bo1Thread>(hThread, BO1H_THREAD);
    if (!t)
        return (DWORD)-1;
    pthread_mutex_lock(&s_syncLock);
    DWORD prev = (DWORD)t->suspendCount;
    if (t->suspendCount > 0 && --t->suspendCount == 0)
        pthread_cond_broadcast(&s_syncCond);
    pthread_mutex_unlock(&s_syncLock);
    return prev;
}

// a running worker cannot be stopped from outside; only the error path and profilers ask
extern "C" DWORD WINAPI SuspendThread(HANDLE hThread) { return Bo1_GetHandleAs<Bo1Thread>(hThread, BO1H_THREAD) ? 0 : (DWORD)-1; }
extern "C" BOOL WINAPI TerminateThread(HANDLE, DWORD) { return FALSE; }

extern "C" BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
    Bo1Thread *t = Bo1_GetHandleAs<Bo1Thread>(hThread, BO1H_THREAD);
    if (!t)
        return FALSE;
    pthread_mutex_lock(&s_syncLock);
    *lpExitCode = t->finished ? t->exitCode : STILL_ACTIVE;
    pthread_mutex_unlock(&s_syncLock);
    return TRUE;
}

extern "C" DWORD WINAPI GetThreadId(HANDLE Thread)
{
    if (Thread == GetCurrentThread())
        return GetCurrentThreadId();
    Bo1Thread *t = Bo1_GetHandleAs<Bo1Thread>(Thread, BO1H_THREAD);
    return t ? t->id : 0;
}

extern "C" HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD) { return NULL; }
extern "C" BOOL WINAPI SetThreadPriority(HANDLE, int) { return TRUE; }
extern "C" int WINAPI GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
extern "C" BOOL WINAPI SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }
extern "C" DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE, DWORD_PTR) { return 1; }
extern "C" DWORD WINAPI SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }
extern "C" BOOL WINAPI SetPriorityClass(HANDLE, DWORD) { return TRUE; }
extern "C" DWORD WINAPI GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
extern "C" BOOL WINAPI SetProcessAffinityMask(HANDLE, DWORD_PTR) { return TRUE; }
extern "C" DWORD WINAPI GetCurrentProcessorNumber(void) { return 0; }

extern "C" BOOL WINAPI GetProcessAffinityMask(HANDLE, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask)
{
    // Win_InitThreads derives the worker thread count from this (capped at 8)
    DWORD_PTR mask = (1u << Bo1_GetCpuCount()) - 1;
    *lpProcessAffinityMask = mask;
    *lpSystemAffinityMask = mask;
    return TRUE;
}

static void Bo1_FileTimeFromMs(double ms, LPFILETIME ft)
{
    ULONGLONG t = (ULONGLONG)(ms * 10000.0);
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

extern "C" BOOL WINAPI GetThreadTimes(HANDLE, LPFILETIME c, LPFILETIME e, LPFILETIME k, LPFILETIME u)
{
    double ms = Bo1_Milliseconds();
    Bo1_FileTimeFromMs(0, c);
    Bo1_FileTimeFromMs(0, e);
    Bo1_FileTimeFromMs(0, k);
    Bo1_FileTimeFromMs(ms, u);
    return TRUE;
}

extern "C" BOOL WINAPI GetProcessTimes(HANDLE h, LPFILETIME c, LPFILETIME e, LPFILETIME k, LPFILETIME u)
{
    return GetThreadTimes(h, c, e, k, u);
}

extern "C" BOOL WINAPI QueryThreadCycleTime(HANDLE, PULONG64 CycleTime)
{
    *CycleTime = (ULONG64)(Bo1_Milliseconds() * 1000000.0);
    return TRUE;
}

extern "C" BOOL WINAPI QueryProcessCycleTime(HANDLE h, PULONG64 CycleTime) { return QueryThreadCycleTime(h, CycleTime); }
extern "C" BOOL WINAPI GetThreadContext(HANDLE, LPCONTEXT) { return FALSE; }
extern "C" DWORD WINAPI QueueUserAPC(void(WINAPI *)(ULONG_PTR), HANDLE, ULONG_PTR) { return 0; }

extern "C" BOOL WINAPI DuplicateHandle(HANDLE, HANDLE hSourceHandle, HANDLE, LPHANDLE lpTargetHandle, DWORD, BOOL, DWORD)
{
    // Sys_InitMainThread duplicates the pseudo handle of the main thread
    if (hSourceHandle == GetCurrentThread())
    {
        Bo1Thread *t = t_thread;
        if (!t)
        {
            t = new Bo1Thread;
            t->id = GetCurrentThreadId();
            t->pthread = pthread_self();
            t_thread = t;
        }
        ++t->refs;
        *lpTargetHandle = (HANDLE)t;
        return TRUE;
    }
    *lpTargetHandle = hSourceHandle;
    return TRUE;
}

// ============================================================================================================
// events, mutexes, semaphores, waits
// ============================================================================================================

extern "C" HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES, BOOL bManualReset, BOOL bInitialState, LPCSTR)
{
    return (HANDLE) new Bo1Event(bManualReset != 0, bInitialState != 0);
}

extern "C" HANDLE WINAPI OpenEventA(DWORD, BOOL, LPCSTR) { return NULL; }

extern "C" BOOL WINAPI SetEvent(HANDLE hEvent)
{
    Bo1Event *e = Bo1_GetHandleAs<Bo1Event>(hEvent, BO1H_EVENT);
    if (!e)
        return FALSE;
    pthread_mutex_lock(&s_syncLock);
    e->signaled = true;
    pthread_cond_broadcast(&s_syncCond);
    pthread_mutex_unlock(&s_syncLock);
    return TRUE;
}

extern "C" BOOL WINAPI ResetEvent(HANDLE hEvent)
{
    Bo1Event *e = Bo1_GetHandleAs<Bo1Event>(hEvent, BO1H_EVENT);
    if (!e)
        return FALSE;
    pthread_mutex_lock(&s_syncLock);
    e->signaled = false;
    pthread_mutex_unlock(&s_syncLock);
    return TRUE;
}

extern "C" BOOL WINAPI PulseEvent(HANDLE hEvent)
{
    // waiters see the signal on their next check; good enough for the one caller (a wake-up nudge)
    SetEvent(hEvent);
    return ResetEvent(hEvent);
}

extern "C" HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL bInitialOwner, LPCSTR)
{
    Bo1Mutex *m = new Bo1Mutex;
    if (bInitialOwner)
    {
        m->owner = GetCurrentThreadId();
        m->recursion = 1;
    }
    return (HANDLE)m;
}

extern "C" HANDLE WINAPI OpenMutexA(DWORD, BOOL, LPCSTR) { return NULL; }

extern "C" BOOL WINAPI ReleaseMutex(HANDLE hMutex)
{
    Bo1Mutex *m = Bo1_GetHandleAs<Bo1Mutex>(hMutex, BO1H_MUTEX);
    if (!m)
        return FALSE;
    pthread_mutex_lock(&s_syncLock);
    BOOL ok = m->owner == GetCurrentThreadId();
    if (ok && --m->recursion == 0)
    {
        m->owner = 0;
        pthread_cond_broadcast(&s_syncCond);
    }
    pthread_mutex_unlock(&s_syncLock);
    return ok;
}

extern "C" HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG lInitialCount, LONG lMaximumCount, LPCSTR)
{
    return (HANDLE) new Bo1Semaphore(lInitialCount, lMaximumCount);
}

extern "C" BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount)
{
    Bo1Semaphore *s = Bo1_GetHandleAs<Bo1Semaphore>(hSemaphore, BO1H_SEMAPHORE);
    if (!s)
        return FALSE;
    pthread_mutex_lock(&s_syncLock);
    if (lpPreviousCount)
        *lpPreviousCount = s->count;
    BOOL ok = s->count + lReleaseCount <= s->maxCount;
    if (ok)
    {
        s->count += lReleaseCount;
        pthread_cond_broadcast(&s_syncCond);
    }
    pthread_mutex_unlock(&s_syncLock);
    return ok;
}

// under s_syncLock: is h signaled for the calling thread, and if so take it (auto-reset, mutex, semaphore)
static bool Bo1_TryAcquire(HANDLE h, bool take)
{
    Bo1Handle *obj = Bo1_GetHandle(h);
    if (!obj)
        return true; // pseudo / unknown handles never block
    switch (obj->type)
    {
    case BO1H_EVENT: {
        Bo1Event *e = static_cast<Bo1Event *>(obj);
        if (!e->signaled)
            return false;
        if (take && !e->manualReset)
            e->signaled = false;
        return true;
    }
    case BO1H_MUTEX: {
        Bo1Mutex *m = static_cast<Bo1Mutex *>(obj);
        DWORD me = GetCurrentThreadId();
        if (m->owner && m->owner != me)
            return false;
        if (take)
        {
            m->owner = me;
            ++m->recursion;
        }
        return true;
    }
    case BO1H_SEMAPHORE: {
        Bo1Semaphore *s = static_cast<Bo1Semaphore *>(obj);
        if (s->count <= 0)
            return false;
        if (take)
            --s->count;
        return true;
    }
    case BO1H_THREAD:
        return static_cast<Bo1Thread *>(obj)->finished;
    default:
        return true;
    }
}

static void Bo1_Deadline(DWORD ms, timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int Bo1_DrainApcs(); // overlapped file reads, below

// what this thread has waited for, for the game thread's working time (web_main.cpp: the frame rate overlay's "game")
static thread_local double t_waitedMs;

double Bo1_ThreadWaitedMs() { return t_waitedMs; }

static DWORD Bo1_WaitForObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable)
{
    if (bAlertable && Bo1_DrainApcs())
        return WAIT_IO_COMPLETION;
    for (DWORD i = 0; i < nCount; ++i)
    {
        // as on Windows: a NULL or closed handle fails the wait (it is not "signaled")
        if (!Bo1_GetHandle(lpHandles[i]))
        {
            t_lastError = ERROR_INVALID_HANDLE;
            return WAIT_FAILED;
        }
    }
    timespec deadline;
    if (dwMilliseconds != INFINITE)
        Bo1_Deadline(dwMilliseconds, &deadline);
    bool stalled = false;
    pthread_mutex_lock(&s_syncLock);
    for (;;)
    {
        if (bWaitAll)
        {
            bool all = true;
            for (DWORD i = 0; i < nCount && all; ++i)
                all = Bo1_TryAcquire(lpHandles[i], false);
            if (all)
            {
                for (DWORD i = 0; i < nCount; ++i)
                    Bo1_TryAcquire(lpHandles[i], true);
                pthread_mutex_unlock(&s_syncLock);
                return WAIT_OBJECT_0;
            }
        }
        else
        {
            for (DWORD i = 0; i < nCount; ++i)
            {
                if (Bo1_TryAcquire(lpHandles[i], true))
                {
                    pthread_mutex_unlock(&s_syncLock);
                    return WAIT_OBJECT_0 + i;
                }
            }
        }
        if (dwMilliseconds == 0)
            break;
        if (dwMilliseconds == INFINITE)
        {
            if (!stalled)
            {
                // block in slices to notice (and report) a wait that never ends
                timespec slice;
                Bo1_Deadline(20000, &slice);
                if (pthread_cond_timedwait(&s_syncCond, &s_syncLock, &slice) == ETIMEDOUT)
                {
                    stalled = true;
                    Bo1_LogStall("WaitForMultipleObjects(INFINITE)");
                }
            }
            else
                pthread_cond_wait(&s_syncCond, &s_syncLock);
        }
        else if (pthread_cond_timedwait(&s_syncCond, &s_syncLock, &deadline) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&s_syncLock);
    return WAIT_TIMEOUT;
}

extern "C" DWORD WINAPI WaitForMultipleObjectsEx(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable)
{
    if (!dwMilliseconds)
        return Bo1_WaitForObjects(nCount, lpHandles, bWaitAll, 0, bAlertable);
    double began = emscripten_get_now();
    DWORD result = Bo1_WaitForObjects(nCount, lpHandles, bWaitAll, dwMilliseconds, bAlertable);
    t_waitedMs += emscripten_get_now() - began;
    return result;
}

extern "C" DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
    return WaitForMultipleObjectsEx(nCount, lpHandles, bWaitAll, dwMilliseconds, FALSE);
}

extern "C" DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable)
{
    return WaitForMultipleObjectsEx(1, &hHandle, FALSE, dwMilliseconds, bAlertable);
}

extern "C" DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    return WaitForMultipleObjectsEx(1, &hHandle, FALSE, dwMilliseconds, FALSE);
}

extern "C" DWORD WINAPI MsgWaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles, BOOL fWaitAll, DWORD dwMilliseconds, DWORD)
{
    return WaitForMultipleObjectsEx(nCount, pHandles, fWaitAll, dwMilliseconds, FALSE);
}

extern "C" DWORD WINAPI SignalObjectAndWait(HANDLE hObjectToSignal, HANDLE hObjectToWaitOn, DWORD dwMilliseconds, BOOL bAlertable)
{
    SetEvent(hObjectToSignal);
    return WaitForSingleObjectEx(hObjectToWaitOn, dwMilliseconds, bAlertable);
}

extern "C" BOOL WINAPI CloseHandle(HANDLE hObject)
{
    Bo1Handle *obj = Bo1_GetHandle(hObject);
    if (!obj)
        return hObject && hObject != INVALID_HANDLE_VALUE;
    switch (obj->type)
    {
    case BO1H_THREAD:
        Bo1_ReleaseThread(static_cast<Bo1Thread *>(obj));
        return TRUE;
    case BO1H_FILE: {
        Bo1File *f = static_cast<Bo1File *>(obj);
        if (f->fd >= 0)
            close(f->fd);
        delete f;
        return TRUE;
    }
    case BO1H_EVENT:
    case BO1H_MUTEX:
    case BO1H_SEMAPHORE:
        // the engine closes its sync objects only at shutdown, when nothing waits on them any more
        pthread_mutex_lock(&s_syncLock);
        delete obj;
        pthread_mutex_unlock(&s_syncLock);
        return TRUE;
    default:
        delete obj;
        return TRUE;
    }
}

extern "C" void WINAPI Sleep(DWORD dwMilliseconds)
{
    if (dwMilliseconds == 0)
    {
        sched_yield();
        return;
    }
    double began = emscripten_get_now();
    usleep(dwMilliseconds * 1000u);
    t_waitedMs += emscripten_get_now() - began;
}

extern "C" DWORD WINAPI SleepEx(DWORD dwMilliseconds, BOOL bAlertable)
{
    if (bAlertable && Bo1_DrainApcs())
        return WAIT_IO_COMPLETION;
    if (dwMilliseconds != INFINITE)
        Sleep(dwMilliseconds);
    return 0;
}

extern "C" BOOL WINAPI SwitchToThread(void)
{
    sched_yield();
    return TRUE;
}

// ============================================================================================================
// critical sections / SRW locks / TLS
// ============================================================================================================

static const int BO1_CS_INITIALIZED = 0x0C51C51C;

extern "C" void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->SpinCount = 0;
    cs->initialized = BO1_CS_INITIALIZED;
}

extern "C" BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD)
{
    InitializeCriticalSection(cs);
    return TRUE;
}

extern "C" BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD, DWORD)
{
    InitializeCriticalSection(cs);
    return TRUE;
}

extern "C" DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION, DWORD) { return 0; }

static void Bo1_CheckCS(LPCRITICAL_SECTION cs)
{
    // a zero-filled static critical section that never saw InitializeCriticalSection
    if (cs->initialized != BO1_CS_INITIALIZED)
    {
        static pthread_mutex_t initLock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&initLock);
        if (cs->initialized != BO1_CS_INITIALIZED)
            InitializeCriticalSection(cs);
        pthread_mutex_unlock(&initLock);
    }
}

extern "C" void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    Bo1_CheckCS(cs);
    timespec slice;
    Bo1_Deadline(20000, &slice);
    if (pthread_mutex_timedlock(&cs->mutex, &slice) == ETIMEDOUT)
    {
        Bo1_LogStall("EnterCriticalSection");
        pthread_mutex_lock(&cs->mutex);
    }
    cs->OwningThread = (HANDLE)(ULONG_PTR)GetCurrentThreadId();
    ++cs->RecursionCount;
}

extern "C" BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    Bo1_CheckCS(cs);
    if (pthread_mutex_trylock(&cs->mutex))
        return FALSE;
    cs->OwningThread = (HANDLE)(ULONG_PTR)GetCurrentThreadId();
    ++cs->RecursionCount;
    return TRUE;
}

extern "C" void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (--cs->RecursionCount == 0)
        cs->OwningThread = 0;
    pthread_mutex_unlock(&cs->mutex);
}

extern "C" void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->initialized == BO1_CS_INITIALIZED)
        pthread_mutex_destroy(&cs->mutex);
    cs->initialized = 0;
}

// SRW locks hold a pthread_rwlock_t allocated on first use
static pthread_rwlock_t *Bo1_SrwLock(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)__atomic_load_n(&lock->Ptr, __ATOMIC_ACQUIRE);
    if (rw)
        return rw;
    pthread_rwlock_t *fresh = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    pthread_rwlock_init(fresh, nullptr);
    void *expected = nullptr;
    if (__atomic_compare_exchange_n(&lock->Ptr, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    pthread_rwlock_destroy(fresh);
    free(fresh);
    return (pthread_rwlock_t *)expected;
}

extern "C" void WINAPI InitializeSRWLock(PSRWLOCK SRWLock) { SRWLock->Ptr = nullptr; }
extern "C" void WINAPI AcquireSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_wrlock(Bo1_SrwLock(l)); }
extern "C" void WINAPI ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_unlock(Bo1_SrwLock(l)); }
extern "C" void WINAPI AcquireSRWLockShared(PSRWLOCK l) { pthread_rwlock_rdlock(Bo1_SrwLock(l)); }
extern "C" void WINAPI ReleaseSRWLockShared(PSRWLOCK l) { pthread_rwlock_unlock(Bo1_SrwLock(l)); }

extern "C" DWORD WINAPI TlsAlloc(void)
{
    pthread_key_t key;
    if (pthread_key_create(&key, nullptr))
        return TLS_OUT_OF_INDEXES;
    return (DWORD)key;
}
extern "C" LPVOID WINAPI TlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }
extern "C" BOOL WINAPI TlsSetValue(DWORD i, LPVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }
extern "C" BOOL WINAPI TlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }

// ============================================================================================================
// time
// ============================================================================================================

extern "C" BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000;
    return TRUE;
}

extern "C" BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *c)
{
    c->QuadPart = (LONGLONG)(Bo1_Milliseconds() * 1000.0);
    return TRUE;
}

extern "C" unsigned long long __rdtsc(void) { return (unsigned long long)(Bo1_Milliseconds() * 1000000.0); }

extern "C" unsigned long __readfsdword(unsigned long offset)
{
    return offset == 0x24 ? GetCurrentThreadId() : 0;
}

extern "C" DWORD WINAPI GetTickCount(void) { return (DWORD)Bo1_Milliseconds(); }
extern "C" ULONGLONG WINAPI GetTickCount64(void) { return (ULONGLONG)Bo1_Milliseconds(); }
extern "C" DWORD WINAPI timeGetTime(void) { return (DWORD)Bo1_Milliseconds(); }
extern "C" MMRESULT WINAPI timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
extern "C" MMRESULT WINAPI timeEndPeriod(UINT) { return TIMERR_NOERROR; }

extern "C" MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS ptc, UINT)
{
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 1000000;
    return TIMERR_NOERROR;
}

// FILETIME: 100 ns since 1601-01-01
static const ULONGLONG BO1_FILETIME_UNIX_EPOCH = 116444736000000000ULL;

static void Bo1_UnixToFileTime(time_t t, long nsec, LPFILETIME ft)
{
    ULONGLONG v = (ULONGLONG)t * 10000000ULL + (ULONGLONG)(nsec / 100) + BO1_FILETIME_UNIX_EPOCH;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static void Bo1_TmToSystemTime(const struct tm *tm, WORD ms, LPSYSTEMTIME st)
{
    st->wYear = (WORD)(tm->tm_year + 1900);
    st->wMonth = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek = (WORD)tm->tm_wday;
    st->wDay = (WORD)tm->tm_mday;
    st->wHour = (WORD)tm->tm_hour;
    st->wMinute = (WORD)tm->tm_min;
    st->wSecond = (WORD)tm->tm_sec;
    st->wMilliseconds = ms;
}

extern "C" void WINAPI GetSystemTime(LPSYSTEMTIME st)
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    Bo1_TmToSystemTime(&tm, (WORD)(ts.tv_nsec / 1000000), st);
}

extern "C" void WINAPI GetLocalTime(LPSYSTEMTIME st)
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    Bo1_TmToSystemTime(&tm, (WORD)(ts.tv_nsec / 1000000), st);
}

extern "C" void WINAPI GetSystemTimeAsFileTime(LPFILETIME ft)
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    Bo1_UnixToFileTime(ts.tv_sec, ts.tv_nsec, ft);
}

extern "C" BOOL WINAPI FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG v = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (v < BO1_FILETIME_UNIX_EPOCH)
        v = BO1_FILETIME_UNIX_EPOCH;
    v -= BO1_FILETIME_UNIX_EPOCH;
    time_t t = (time_t)(v / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    Bo1_TmToSystemTime(&tm, (WORD)((v / 10000ULL) % 1000ULL), st);
    return TRUE;
}

extern "C" BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm = {};
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    Bo1_UnixToFileTime(timegm(&tm), (long)st->wMilliseconds * 1000000L, ft);
    return TRUE;
}

extern "C" BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *in, LPFILETIME out)
{
    *out = *in;
    return TRUE;
}

extern "C" BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *in, LPFILETIME out)
{
    *out = *in;
    return TRUE;
}

extern "C" LONG WINAPI CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    ULONGLONG x = ((ULONGLONG)a->dwHighDateTime << 32) | a->dwLowDateTime;
    ULONGLONG y = ((ULONGLONG)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}

extern "C" DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz)
{
    memset(tz, 0, sizeof(*tz));
    return TIME_ZONE_ID_UNKNOWN;
}

extern "C" BOOL WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *, const SYSTEMTIME *in, LPSYSTEMTIME out)
{
    *out = *in;
    return TRUE;
}

extern "C" BOOL WINAPI GetSystemTimes(LPFILETIME idle, LPFILETIME kernel, LPFILETIME user)
{
    Bo1_FileTimeFromMs(0, idle);
    Bo1_FileTimeFromMs(0, kernel);
    Bo1_FileTimeFromMs(Bo1_Milliseconds(), user);
    return TRUE;
}

// ============================================================================================================
// memory
// ============================================================================================================
// Wasm memory has no reserve/commit: a reservation is allocated (zeroed) in full and commits are bookkeeping. A
// decommit zeroes its range, so the next commit hands back zero pages as Windows does.

struct Bo1Region
{
    SIZE_T size;
};

static std::mutex s_regionLock;
static std::map<uintptr_t, Bo1Region> s_regions; // base -> region

static bool Bo1_FindRegion(uintptr_t addr, uintptr_t *base, SIZE_T *size)
{
    auto it = s_regions.upper_bound(addr);
    if (it == s_regions.begin())
        return false;
    --it;
    if (addr >= it->first + it->second.size)
        return false;
    *base = it->first;
    *size = it->second.size;
    return true;
}

extern "C" LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD)
{
    if (!dwSize)
        return NULL;
    std::lock_guard<std::mutex> guard(s_regionLock);
    if (lpAddress)
    {
        // commit inside an existing reservation
        uintptr_t base;
        SIZE_T size;
        if (!Bo1_FindRegion((uintptr_t)lpAddress, &base, &size))
        {
            t_lastError = ERROR_INVALID_PARAMETER;
            return NULL;
        }
        return lpAddress;
    }
    if (!(flAllocationType & (MEM_RESERVE | MEM_COMMIT)))
        return NULL;
    SIZE_T size = (dwSize + 0xFFFF) & ~(SIZE_T)0xFFFF;
    void *p = aligned_alloc(0x10000, size);
    if (!p)
    {
        Bo1_Log("VirtualAlloc: out of memory reserving %u bytes", (unsigned)size);
        t_lastError = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    memset(p, 0, size);
    s_regions[(uintptr_t)p] = Bo1Region{size};
    return p;
}

extern "C" BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    std::lock_guard<std::mutex> guard(s_regionLock);
    if (dwFreeType & MEM_RELEASE)
    {
        auto it = s_regions.find((uintptr_t)lpAddress);
        if (it == s_regions.end())
            return FALSE;
        free(lpAddress);
        s_regions.erase(it);
        return TRUE;
    }
    if (dwFreeType & MEM_DECOMMIT)
    {
        uintptr_t base;
        SIZE_T size;
        if (!Bo1_FindRegion((uintptr_t)lpAddress, &base, &size))
            return FALSE;
        SIZE_T end = dwSize ? (uintptr_t)lpAddress + dwSize : base + size;
        if (end > base + size)
            end = base + size;
        memset(lpAddress, 0, end - (uintptr_t)lpAddress);
        return TRUE;
    }
    return FALSE;
}

extern "C" SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION mbi, SIZE_T dwLength)
{
    if (dwLength < sizeof(*mbi))
        return 0;
    std::lock_guard<std::mutex> guard(s_regionLock);
    uintptr_t base;
    SIZE_T size;
    memset(mbi, 0, sizeof(*mbi));
    if (!Bo1_FindRegion((uintptr_t)lpAddress, &base, &size))
    {
        mbi->BaseAddress = (PVOID)((uintptr_t)lpAddress & ~(uintptr_t)0xFFF);
        mbi->RegionSize = 0x1000;
        mbi->State = MEM_COMMIT; // heap or static memory
        mbi->Protect = PAGE_READWRITE;
        mbi->Type = MEM_PRIVATE;
        return sizeof(*mbi);
    }
    mbi->BaseAddress = (PVOID)((uintptr_t)lpAddress & ~(uintptr_t)0xFFF);
    mbi->AllocationBase = (PVOID)base;
    mbi->AllocationProtect = PAGE_READWRITE;
    mbi->RegionSize = base + size - (uintptr_t)mbi->BaseAddress;
    mbi->State = MEM_COMMIT;
    mbi->Protect = PAGE_READWRITE;
    mbi->Type = MEM_PRIVATE;
    return sizeof(*mbi);
}

extern "C" BOOL WINAPI VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD lpflOldProtect)
{
    if (lpflOldProtect)
        *lpflOldProtect = PAGE_READWRITE;
    return TRUE;
}

extern "C" BOOL WINAPI VirtualLock(LPVOID, SIZE_T) { return TRUE; }
extern "C" BOOL WINAPI VirtualUnlock(LPVOID, SIZE_T) { return TRUE; }
extern "C" SIZE_T WINAPI GetLargePageMinimum(void) { return 0; }

extern "C" void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS ms)
{
    memset(ms, 0, sizeof(*ms));
    ms->dwLength = sizeof(*ms);
    ms->dwTotalPhys = 0x80000000u;
    ms->dwAvailPhys = 0x40000000u;
    ms->dwTotalVirtual = 0xFFFE0000u;
    ms->dwAvailVirtual = 0x80000000u;
    ms->dwTotalPageFile = 0x80000000u;
    ms->dwAvailPageFile = 0x40000000u;
}

extern "C" BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms)
{
    memset(ms, 0, sizeof(*ms));
    ms->dwLength = sizeof(*ms);
    ms->ullTotalPhys = 8ull << 30;
    ms->ullAvailPhys = 4ull << 30;
    ms->ullTotalVirtual = 4ull << 30;
    ms->ullAvailVirtual = 2ull << 30;
    ms->ullTotalPageFile = 8ull << 30;
    ms->ullAvailPageFile = 4ull << 30;
    return TRUE;
}

extern "C" void WINAPI GetSystemInfo(LPSYSTEM_INFO si)
{
    memset(si, 0, sizeof(*si));
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
    si->dwPageSize = 0x1000;
    si->lpMinimumApplicationAddress = (LPVOID)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)0xFFFEFFFF;
    si->dwNumberOfProcessors = Bo1_GetCpuCount();
    si->dwActiveProcessorMask = (1u << si->dwNumberOfProcessors) - 1;
    si->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
    si->dwAllocationGranularity = 0x10000;
}

extern "C" void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }

extern "C" BOOL WINAPI GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS pmc, DWORD cb)
{
    memset(pmc, 0, cb);
    pmc->cb = cb;
    pmc->WorkingSetSize = (SIZE_T)EM_ASM_DOUBLE({ return HEAPU8.length; });
    pmc->PeakWorkingSetSize = pmc->WorkingSetSize;
    pmc->PagefileUsage = pmc->WorkingSetSize;
    return TRUE;
}

static const ULONG_PTR BO1_PROCESS_HEAP = 0x48454150;

extern "C" HANDLE WINAPI GetProcessHeap(void) { return (HANDLE)BO1_PROCESS_HEAP; }
extern "C" HANDLE WINAPI HeapCreate(DWORD, SIZE_T, SIZE_T) { return (HANDLE)BO1_PROCESS_HEAP; }
extern "C" BOOL WINAPI HeapDestroy(HANDLE) { return TRUE; }

extern "C" LPVOID WINAPI HeapAlloc(HANDLE, DWORD dwFlags, SIZE_T dwBytes)
{
    return (dwFlags & HEAP_ZERO_MEMORY) ? calloc(1, dwBytes ? dwBytes : 1) : malloc(dwBytes ? dwBytes : 1);
}

extern "C" LPVOID WINAPI HeapReAlloc(HANDLE, DWORD, LPVOID lpMem, SIZE_T dwBytes) { return realloc(lpMem, dwBytes); }

extern "C" BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID lpMem)
{
    free(lpMem);
    return TRUE;
}

extern "C" SIZE_T WINAPI HeapSize(HANDLE, DWORD, LPCVOID lpMem) { return bo1_msize((void *)lpMem); }
extern "C" BOOL WINAPI HeapValidate(HANDLE, DWORD, LPCVOID) { return TRUE; }
extern "C" HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes) { return (uFlags & LMEM_ZEROINIT) ? calloc(1, uBytes) : malloc(uBytes); }
extern "C" HLOCAL WINAPI LocalFree(HLOCAL hMem)
{
    free(hMem);
    return NULL;
}
extern "C" HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes) { return (uFlags & GMEM_ZEROINIT) ? calloc(1, dwBytes) : malloc(dwBytes); }
extern "C" LPVOID WINAPI GlobalLock(HGLOBAL hMem) { return hMem; }
extern "C" BOOL WINAPI GlobalUnlock(HGLOBAL) { return TRUE; }
extern "C" HGLOBAL WINAPI GlobalFree(HGLOBAL hMem)
{
    free(hMem);
    return NULL;
}
extern "C" SIZE_T WINAPI GlobalSize(HGLOBAL hMem) { return bo1_msize(hMem); }

// ============================================================================================================
// files
// ============================================================================================================

std::string Bo1_NormalizePath(const char *path)
{
    std::string out;
    if (!path)
        return out;
    // "C:\..." drive letters: everything lives under the WasmFS root
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        path += 2;
    out.reserve(strlen(path));
    char prev = 0;
    for (const char *p = path; *p; ++p)
    {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && prev == '/')
            continue;
        out.push_back(c);
        prev = c;
    }
    return out;
}

static void Bo1_MakeParentDirs(const std::string &path)
{
    for (size_t i = 1; i < path.size(); ++i)
    {
        if (path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0777);
    }
}

// the working directory, kept here: WasmFS' getcwd cannot name folders under the case-insensitive mounts
static char s_cwd[MAX_PATH] = BO1_WEB_GAME_ROOT;

extern "C" char *bo1_getcwd(char *buf, size_t size)
{
    if (!buf)
        return strdup(s_cwd);
    if (strlen(s_cwd) + 1 > size)
    {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, s_cwd);
    return buf;
}

extern "C" int bo1_chdir(const char *path)
{
    std::string p = Bo1_NormalizePath(path);
    if (p.empty() || p[0] != '/')
        p = std::string(s_cwd) + "/" + p;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    struct stat st;
    if (stat(p.c_str(), &st) || !S_ISDIR(st.st_mode))
        return -1;
    I_strncpyz_web(s_cwd, p.c_str(), sizeof(s_cwd));
    return 0;
}

static std::string Bo1_Lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

// the install's real names: lowercase "/game/..." -> "/game/..." as on disk (files and folders). Filled before the
// engine's threads start, read only afterwards.
static std::unordered_map<std::string, std::string> s_gameCase;

void Bo1_RegisterGameFile(const std::string &relPath)
{
    std::string real = std::string(BO1_WEB_GAME_ROOT "/") + relPath;
    for (size_t i = strlen(BO1_WEB_GAME_ROOT) + 1; i <= real.size(); ++i)
    {
        if (i == real.size() || real[i] == '/')
            s_gameCase.emplace(Bo1_Lower(real.substr(0, i)), real.substr(0, i));
    }
}

// a /game path in the install's real case; the unknown tail (a file that does not exist) keeps the case asked for
static std::string Bo1_GameCase(const std::string &p)
{
    std::string lower = Bo1_Lower(p);
    auto it = s_gameCase.find(lower);
    if (it != s_gameCase.end())
        return it->second;
    size_t slash = p.rfind('/');
    if (slash == std::string::npos || slash <= strlen(BO1_WEB_GAME_ROOT))
        return p;
    return Bo1_GameCase(p.substr(0, slash)) + p.substr(slash);
}

std::string Bo1_ResolvePath(const char *path, bool forWrite)
{
    std::string p = Bo1_NormalizePath(path);
    if (p.empty())
        return p;
    if (p[0] != '/')
        p = std::string(s_cwd) + "/" + p;
    static const size_t gameLen = strlen(BO1_WEB_GAME_ROOT);
    static const size_t homeLen = strlen(BO1_WEB_HOME_ROOT);
    if (p.compare(0, homeLen, BO1_WEB_HOME_ROOT) == 0 && (p.size() == homeLen || p[homeLen] == '/'))
    {
        std::string home = BO1_WEB_HOME_ROOT + Bo1_Lower(p.substr(homeLen));
        if (forWrite)
            Bo1_MakeParentDirs(home);
        return home;
    }
    if (p.compare(0, gameLen, BO1_WEB_GAME_ROOT) != 0 || (p.size() > gameLen && p[gameLen] != '/'))
        return p;
    std::string home = BO1_WEB_HOME_ROOT + Bo1_Lower(p.substr(gameLen));
    if (forWrite)
    {
        Bo1_MakeParentDirs(home);
        return home;
    }
    struct stat st;
    if (stat(home.c_str(), &st) == 0)
    {
        if (Bo1_LogFiles())
            Bo1_Log("resolve %s: overlay %s", p.c_str(), home.c_str());
        return home;
    }
    std::string real = Bo1_GameCase(p);
    if (Bo1_LogFiles())
        Bo1_Log("resolve %s -> %s", p.c_str(), real.c_str());
    return real;
}

static bool Bo1_WildcardMatch(const char *pattern, const char *name)
{
    // Windows-style '*' and '?', case-insensitive
    if (!*pattern)
        return !*name;
    if (*pattern == '*')
    {
        for (const char *n = name;; ++n)
        {
            if (Bo1_WildcardMatch(pattern + 1, n))
                return true;
            if (!*n)
                return false;
        }
    }
    if (!*name)
        return false;
    if (*pattern == '?' || tolower((unsigned char)*pattern) == tolower((unsigned char)*name))
        return Bo1_WildcardMatch(pattern + 1, name + 1);
    return false;
}

static void Bo1_FillFindData(const std::string &dir, const std::string &name, LPWIN32_FIND_DATAA fd)
{
    memset(fd, 0, sizeof(*fd));
    I_strncpyz_web(fd->cFileName, name.c_str(), sizeof(fd->cFileName));
    std::string full = dir.empty() ? name : dir + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) == 0)
    {
        fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        fd->nFileSizeLow = (DWORD)st.st_size;
        fd->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
        Bo1_UnixToFileTime(st.st_mtime, 0, &fd->ftLastWriteTime);
        fd->ftCreationTime = fd->ftLastWriteTime;
        fd->ftLastAccessTime = fd->ftLastWriteTime;
    }
    else
    {
        fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
}

static void Bo1_ListDir(const std::string &dir, const std::string &pattern, std::vector<std::string> &names, std::vector<std::string> &dirs)
{
    DIR *d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (!Bo1_WildcardMatch(pattern.c_str(), e->d_name))
            continue;
        bool dup = false;
        for (const std::string &n : names)
            dup = dup || !strcasecmp(n.c_str(), e->d_name);
        if (!dup)
        {
            names.push_back(e->d_name);
            dirs.push_back(dir);
        }
    }
    closedir(d);
}

extern "C" HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    std::string spec = Bo1_NormalizePath(lpFileName);
    size_t slash = spec.rfind('/');
    std::string dirSpec = slash == std::string::npos ? std::string(".") : spec.substr(0, slash);
    std::string pattern = slash == std::string::npos ? spec : spec.substr(slash + 1);
    std::string gameDir = Bo1_ResolvePath(dirSpec.c_str(), false);
    std::string homeDir = Bo1_ResolvePath((dirSpec + "/x").c_str(), true);
    homeDir = homeDir.substr(0, homeDir.size() - 2);
    Bo1Find *find = new Bo1Find;
    if (Bo1_WildcardMatch(pattern.c_str(), "."))
    {
        find->names.push_back(".");
        find->dirs.push_back(gameDir);
    }
    if (Bo1_WildcardMatch(pattern.c_str(), ".."))
    {
        find->names.push_back("..");
        find->dirs.push_back(gameDir);
    }
    if (homeDir != gameDir)
        Bo1_ListDir(homeDir, pattern, find->names, find->dirs);
    std::string installDir = Bo1_NormalizePath(dirSpec.c_str());
    if (installDir.empty() || installDir[0] != '/')
        installDir = std::string(s_cwd) + "/" + installDir;
    if (!installDir.compare(0, strlen(BO1_WEB_GAME_ROOT), BO1_WEB_GAME_ROOT))
        installDir = Bo1_GameCase(installDir);
    Bo1_ListDir(installDir, pattern, find->names, find->dirs);
    if (find->names.empty())
    {
        delete find;
        t_lastError = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    Bo1_FillFindData(find->dirs[0], find->names[0], lpFindFileData);
    find->next = 1;
    return (HANDLE)find;
}

extern "C" BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    Bo1Find *find = Bo1_GetHandleAs<Bo1Find>(hFindFile, BO1H_FIND);
    if (!find || find->next >= find->names.size())
    {
        t_lastError = ERROR_NO_MORE_FILES;
        return FALSE;
    }
    Bo1_FillFindData(find->dirs[find->next], find->names[find->next], lpFindFileData);
    ++find->next;
    return TRUE;
}

extern "C" BOOL WINAPI FindClose(HANDLE hFindFile)
{
    Bo1Find *find = Bo1_GetHandleAs<Bo1Find>(hFindFile, BO1H_FIND);
    if (!find)
        return FALSE;
    delete find;
    return TRUE;
}

// _findfirst family (MSVC's io.h), on top of FindFirstFile
static void Bo1_FindDataToFinddata(const WIN32_FIND_DATAA *fd, struct _finddata64i32_t *out)
{
    memset(out, 0, sizeof(*out));
    out->attrib = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _A_SUBDIR : _A_NORMAL;
    out->size = fd->nFileSizeLow;
    I_strncpyz_web(out->name, fd->cFileName, sizeof(out->name));
}

extern "C" intptr_t _findfirst64i32(const char *filespec, struct _finddata64i32_t *fileinfo)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(filespec, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    Bo1_FindDataToFinddata(&fd, fileinfo);
    return (intptr_t)h;
}

extern "C" int _findnext64i32(intptr_t handle, struct _finddata64i32_t *fileinfo)
{
    WIN32_FIND_DATAA fd;
    if (!FindNextFileA((HANDLE)handle, &fd))
        return -1;
    Bo1_FindDataToFinddata(&fd, fileinfo);
    return 0;
}

extern "C" intptr_t _findfirst(const char *filespec, struct _finddata_t *fileinfo)
{
    return _findfirst64i32(filespec, (struct _finddata64i32_t *)fileinfo);
}

extern "C" int _findnext(intptr_t handle, struct _finddata_t *fileinfo)
{
    return _findnext64i32(handle, (struct _finddata64i32_t *)fileinfo);
}

extern "C" int _findclose(intptr_t handle) { return FindClose((HANDLE)handle) ? 0 : -1; }

extern "C" HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD, LPSECURITY_ATTRIBUTES, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE)
{
    const DWORD FILE_APPEND_DATA_ = 0x0004;
    bool wantWrite = (dwDesiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_APPEND_DATA_)) != 0;
    std::string path = Bo1_ResolvePath(lpFileName, wantWrite);
    if (wantWrite && (dwCreationDisposition == OPEN_EXISTING || dwCreationDisposition == OPEN_ALWAYS))
    {
        // writing a file that so far exists only in the install: start from its contents
        std::string src = Bo1_ResolvePath(lpFileName, false);
        struct stat st;
        if (src != path && stat(path.c_str(), &st) != 0 && stat(src.c_str(), &st) == 0)
            CopyFileA(src.c_str(), path.c_str(), TRUE);
    }
    bool wantRead = (dwDesiredAccess & (GENERIC_READ | GENERIC_ALL)) != 0 || !wantWrite;
    int flags = wantWrite ? (wantRead ? O_RDWR : O_WRONLY) : O_RDONLY;
    if ((dwDesiredAccess & FILE_APPEND_DATA_) && !(dwDesiredAccess & GENERIC_WRITE))
        flags |= O_APPEND;
    switch (dwCreationDisposition)
    {
    case CREATE_NEW:
        flags |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        flags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_ALWAYS:
        flags |= O_CREAT;
        break;
    case TRUNCATE_EXISTING:
        flags |= O_TRUNC;
        break;
    default:
        break;
    }
    int fd = open(path.c_str(), flags, 0666);
    if (Bo1_LogFiles())
        Bo1_Log("CreateFile %s -> %s%s (errno %d, flags 0x%x)", lpFileName, path.c_str(), fd < 0 ? " FAILED" : "", fd < 0 ? errno : 0, flags);
    if (fd < 0)
    {
        Bo1_SetErrnoError();
        return INVALID_HANDLE_VALUE;
    }
    Bo1File *f = new Bo1File;
    f->fd = fd;
    f->overlapped = (dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0;
    f->path = path;
    t_lastError = (dwCreationDisposition == OPEN_ALWAYS || dwCreationDisposition == CREATE_ALWAYS) ? 0 : 0;
    return (HANDLE)f;
}

static off_t Bo1_OverlappedOffset(const OVERLAPPED *ov)
{
    return (off_t)(((unsigned long long)ov->OffsetHigh << 32) | ov->Offset);
}

extern "C" BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = 0;
    if (!f)
    {
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    ssize_t n;
    double start = Bo1_Milliseconds();
    if (lpOverlapped)
        n = pread(f->fd, lpBuffer, nNumberOfBytesToRead, Bo1_OverlappedOffset(lpOverlapped));
    else
        n = read(f->fd, lpBuffer, nNumberOfBytesToRead);
    Bo1_NoteFileRead(f->path.c_str(), Bo1_Milliseconds() - start);
    if (n < 0)
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = (DWORD)n;
    if (lpOverlapped)
    {
        lpOverlapped->Internal = 0;
        lpOverlapped->InternalHigh = (ULONG_PTR)n;
        if (lpOverlapped->hEvent)
            SetEvent(lpOverlapped->hEvent);
        if (n == 0 && nNumberOfBytesToRead)
        {
            t_lastError = ERROR_HANDLE_EOF;
            return FALSE;
        }
    }
    return TRUE;
}

// ReadFileEx: the read happens now; its completion routine is queued as an APC for this thread and runs in the
// next alertable wait (DB_WaitXFileStage's SleepEx), as Windows delivers it.
struct Bo1Apc
{
    LPOVERLAPPED_COMPLETION_ROUTINE routine;
    DWORD error;
    DWORD bytes;
    LPOVERLAPPED overlapped;
};
static thread_local std::vector<Bo1Apc> *t_apcs;

static int Bo1_DrainApcs()
{
    if (!t_apcs || t_apcs->empty())
        return 0;
    std::vector<Bo1Apc> run;
    run.swap(*t_apcs);
    for (const Bo1Apc &apc : run)
        apc.routine(apc.error, apc.bytes, apc.overlapped);
    return (int)run.size();
}

extern "C" BOOL WINAPI ReadFileEx(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPOVERLAPPED lpOverlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    if (!f || !lpOverlapped)
    {
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    off_t offset = Bo1_OverlappedOffset(lpOverlapped);
    struct stat st;
    if (fstat(f->fd, &st) == 0 && offset >= st.st_size)
    {
        t_lastError = ERROR_HANDLE_EOF;
        return FALSE;
    }
    double start = Bo1_Milliseconds();
    ssize_t n = pread(f->fd, lpBuffer, nNumberOfBytesToRead, offset);
    Bo1_NoteFileRead(f->path.c_str(), Bo1_Milliseconds() - start);
    if (n < 0)
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    lpOverlapped->Internal = 0;
    lpOverlapped->InternalHigh = (ULONG_PTR)n;
    if (!t_apcs)
        t_apcs = new std::vector<Bo1Apc>;
    t_apcs->push_back(Bo1Apc{lpCompletionRoutine, 0, (DWORD)n, lpOverlapped});
    return TRUE;
}

extern "C" BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = 0;
    if (!f)
    {
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    ssize_t n = lpOverlapped ? pwrite(f->fd, lpBuffer, nNumberOfBytesToWrite, Bo1_OverlappedOffset(lpOverlapped))
                             : write(f->fd, lpBuffer, nNumberOfBytesToWrite);
    if (n < 0)
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = (DWORD)n;
    if (lpOverlapped)
    {
        lpOverlapped->Internal = 0;
        lpOverlapped->InternalHigh = (ULONG_PTR)n;
        if (lpOverlapped->hEvent)
            SetEvent(lpOverlapped->hEvent);
    }
    return TRUE;
}

extern "C" BOOL WINAPI GetOverlappedResult(HANDLE, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL)
{
    // every overlapped operation above completed before it returned
    *lpNumberOfBytesTransferred = (DWORD)lpOverlapped->InternalHigh;
    return TRUE;
}

extern "C" BOOL WINAPI CancelIo(HANDLE) { return TRUE; }
extern "C" BOOL WINAPI CancelIoEx(HANDLE, LPOVERLAPPED) { return TRUE; }

extern "C" DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    if (!f)
        return INVALID_SET_FILE_POINTER;
    long long dist = lpDistanceToMoveHigh ? (((long long)*lpDistanceToMoveHigh << 32) | (DWORD)lDistanceToMove) : lDistanceToMove;
    int whence = dwMoveMethod == FILE_CURRENT ? SEEK_CUR : dwMoveMethod == FILE_END ? SEEK_END : SEEK_SET;
    off_t pos = lseek(f->fd, (off_t)dist, whence);
    if (pos < 0)
    {
        Bo1_SetErrnoError();
        return INVALID_SET_FILE_POINTER;
    }
    if (lpDistanceToMoveHigh)
        *lpDistanceToMoveHigh = (LONG)((unsigned long long)pos >> 32);
    return (DWORD)pos;
}

extern "C" BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    if (!f)
        return FALSE;
    int whence = dwMoveMethod == FILE_CURRENT ? SEEK_CUR : dwMoveMethod == FILE_END ? SEEK_END : SEEK_SET;
    off_t pos = lseek(f->fd, (off_t)liDistanceToMove.QuadPart, whence);
    if (pos < 0)
        return FALSE;
    if (lpNewFilePointer)
        lpNewFilePointer->QuadPart = pos;
    return TRUE;
}

extern "C" DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st))
        return INVALID_FILE_SIZE;
    if (lpFileSizeHigh)
        *lpFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    return (DWORD)st.st_size;
}

extern "C" BOOL WINAPI GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st))
        return FALSE;
    lpFileSize->QuadPart = st.st_size;
    return TRUE;
}

extern "C" DWORD WINAPI GetFileType(HANDLE hFile) { return Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE) ? FILE_TYPE_DISK : FILE_TYPE_UNKNOWN; }

extern "C" BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME c, LPFILETIME a, LPFILETIME w)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st))
        return FALSE;
    FILETIME ft;
    Bo1_UnixToFileTime(st.st_mtime, 0, &ft);
    if (c)
        *c = ft;
    if (a)
        *a = ft;
    if (w)
        *w = ft;
    return TRUE;
}

extern "C" BOOL WINAPI SetFileTime(HANDLE, const FILETIME *, const FILETIME *, const FILETIME *) { return TRUE; }

extern "C" BOOL WINAPI GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION info)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st))
        return FALSE;
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    info->nFileSizeLow = (DWORD)st.st_size;
    info->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    info->nNumberOfLinks = 1;
    Bo1_UnixToFileTime(st.st_mtime, 0, &info->ftLastWriteTime);
    return TRUE;
}

extern "C" BOOL WINAPI FlushFileBuffers(HANDLE hFile)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    return f && fsync(f->fd) == 0;
}

extern "C" BOOL WINAPI SetEndOfFile(HANDLE hFile)
{
    Bo1File *f = Bo1_GetHandleAs<Bo1File>(hFile, BO1H_FILE);
    if (!f)
        return FALSE;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, pos) == 0;
}

extern "C" BOOL WINAPI DeleteFileA(LPCSTR lpFileName)
{
    if (unlink(Bo1_ResolvePath(lpFileName, true).c_str()))
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    return TRUE;
}

extern "C" BOOL WINAPI MoveFileExA(LPCSTR from, LPCSTR to, DWORD dwFlags)
{
    std::string dst = Bo1_ResolvePath(to, true);
    if (!(dwFlags & MOVEFILE_REPLACE_EXISTING) && access(dst.c_str(), F_OK) == 0)
    {
        t_lastError = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    if (rename(Bo1_ResolvePath(from, false).c_str(), dst.c_str()))
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    return TRUE;
}

extern "C" BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, 0); }

extern "C" BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL bFailIfExists)
{
    std::string src = Bo1_ResolvePath(from, false), dst = Bo1_ResolvePath(to, true);
    if (bFailIfExists && access(dst.c_str(), F_OK) == 0)
    {
        t_lastError = ERROR_FILE_EXISTS;
        return FALSE;
    }
    int in = open(src.c_str(), O_RDONLY);
    if (in < 0)
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
    {
        Bo1_SetErrnoError();
        close(in);
        return FALSE;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(in);
    close(out);
    return TRUE;
}

extern "C" BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES)
{
    if (mkdir(Bo1_ResolvePath(lpPathName, true).c_str(), 0777))
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    return TRUE;
}

extern "C" BOOL WINAPI RemoveDirectoryA(LPCSTR lpPathName)
{
    if (rmdir(Bo1_ResolvePath(lpPathName, true).c_str()))
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    return TRUE;
}

extern "C" DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName)
{
    struct stat st;
    if (stat(Bo1_ResolvePath(lpFileName, false).c_str(), &st))
    {
        Bo1_SetErrnoError();
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

extern "C" BOOL WINAPI GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS, LPVOID lpFileInformation)
{
    struct stat st;
    if (stat(Bo1_ResolvePath(lpFileName, false).c_str(), &st))
    {
        Bo1_SetErrnoError();
        return FALSE;
    }
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation;
    memset(d, 0, sizeof(*d));
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    d->nFileSizeLow = (DWORD)st.st_size;
    d->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    Bo1_UnixToFileTime(st.st_mtime, 0, &d->ftLastWriteTime);
    d->ftCreationTime = d->ftLastWriteTime;
    d->ftLastAccessTime = d->ftLastWriteTime;
    return TRUE;
}

extern "C" BOOL WINAPI SetFileAttributesA(LPCSTR, DWORD) { return TRUE; }

extern "C" DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer)
{
    const char *cwd = s_cwd;
    DWORD len = (DWORD)strlen(cwd);
    if (len + 1 > nBufferLength)
        return len + 1;
    memcpy(lpBuffer, cwd, len + 1);
    return len;
}

extern "C" BOOL WINAPI SetCurrentDirectoryA(LPCSTR lpPathName) { return bo1_chdir(lpPathName) == 0; }

extern "C" DWORD WINAPI GetModuleFileNameA(HMODULE, LPSTR lpFilename, DWORD nSize)
{
    // the exe sits in the install folder, as on Windows
    const char *exe = BO1_WEB_GAME_ROOT "/BO1Zombies.exe";
    DWORD len = (DWORD)strlen(exe);
    if (!nSize)
        return 0;
    I_strncpyz_web(lpFilename, exe, nSize);
    return len < nSize ? len : nSize;
}

extern "C" HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName) { return lpModuleName ? NULL : (HMODULE)0x400000; }

extern "C" BOOL WINAPI GetModuleHandleExA(DWORD, LPCSTR, HMODULE *phModule)
{
    *phModule = (HMODULE)0x400000;
    return TRUE;
}

extern "C" DWORD WINAPI GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR *lpFilePart)
{
    std::string path = Bo1_NormalizePath(lpFileName);
    if (path.empty() || path[0] != '/')
        path = std::string(s_cwd) + "/" + path;
    if (path.size() + 1 > nBufferLength)
        return (DWORD)path.size() + 1;
    memcpy(lpBuffer, path.c_str(), path.size() + 1);
    if (lpFilePart)
    {
        char *slash = strrchr(lpBuffer, '/');
        *lpFilePart = slash ? slash + 1 : lpBuffer;
    }
    return (DWORD)path.size();
}

extern "C" DWORD WINAPI GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer)
{
    const char *tmp = "/tmp/";
    I_strncpyz_web(lpBuffer, tmp, nBufferLength);
    return (DWORD)strlen(tmp);
}

extern "C" UINT WINAPI GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName)
{
    static std::atomic<unsigned> counter{1};
    unsigned n = uUnique ? uUnique : counter++;
    sprintf(lpTempFileName, "%s/%.3s%04X.tmp", Bo1_NormalizePath(lpPathName).c_str(), lpPrefixString, n & 0xFFFF);
    return n;
}

extern "C" BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR, PULARGE_INTEGER a, PULARGE_INTEGER t, PULARGE_INTEGER f)
{
    if (a)
        a->QuadPart = 4ull << 30;
    if (t)
        t->QuadPart = 8ull << 30;
    if (f)
        f->QuadPart = 4ull << 30;
    return TRUE;
}

extern "C" BOOL WINAPI GetDiskFreeSpaceA(LPCSTR, LPDWORD spc, LPDWORD bps, LPDWORD freeClusters, LPDWORD totalClusters)
{
    *spc = 8;
    *bps = 512;
    *freeClusters = 0x100000;
    *totalClusters = 0x200000;
    return TRUE;
}

extern "C" UINT WINAPI GetDriveTypeA(LPCSTR) { return DRIVE_FIXED; }
extern "C" DWORD WINAPI GetLogicalDrives(void) { return 1u << 2; }

extern "C" BOOL WINAPI GetVolumeInformationA(LPCSTR, LPSTR name, DWORD nameSize, LPDWORD serial, LPDWORD maxComp, LPDWORD flags, LPSTR fsName,
    DWORD fsNameSize)
{
    if (name && nameSize)
        I_strncpyz_web(name, "WEB", nameSize);
    if (serial)
        *serial = 0xB01D;
    if (maxComp)
        *maxComp = 255;
    if (flags)
        *flags = 0;
    if (fsName && fsNameSize)
        I_strncpyz_web(fsName, "WASMFS", fsNameSize);
    return TRUE;
}

extern "C" HRESULT WINAPI SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR pszPath)
{
    strcpy(pszPath, BO1_WEB_HOME_ROOT);
    return S_OK;
}

extern "C" BOOL WINAPI SHGetSpecialFolderPathA(HWND, LPSTR pszPath, int, BOOL)
{
    strcpy(pszPath, BO1_WEB_HOME_ROOT);
    return TRUE;
}

extern "C" int WINAPI SHCreateDirectoryExA(HWND, LPCSTR pszPath, const SECURITY_ATTRIBUTES *)
{
    std::string path = Bo1_NormalizePath(pszPath);
    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0777);
    }
    return ERROR_SUCCESS;
}

extern "C" HANDLE WINAPI GetStdHandle(DWORD) { return NULL; }
extern "C" BOOL WINAPI AllocConsole(void) { return FALSE; }
extern "C" BOOL WINAPI FreeConsole(void) { return TRUE; }
extern "C" BOOL WINAPI SetConsoleTitleA(LPCSTR) { return TRUE; }

// ============================================================================================================
// process / misc kernel32
// ============================================================================================================

extern "C" HMODULE WINAPI LoadLibraryA(LPCSTR) { return NULL; }
extern "C" HMODULE WINAPI LoadLibraryExA(LPCSTR, HANDLE, DWORD) { return NULL; }
extern "C" FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR) { return NULL; }
extern "C" BOOL WINAPI FreeLibrary(HMODULE) { return TRUE; }

extern "C" void WINAPI OutputDebugStringA(LPCSTR lpOutputString)
{
    // Sys_Print sends every console line here as well as to the console; the page shows the console
    (void)lpOutputString;
}

extern "C" BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }
extern "C" void WINAPI DebugBreak(void) { Bo1_Log("DebugBreak"); }
extern "C" void WINAPI RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR *)
{
    if (code != 0x406D1388) // the debugger thread-name notification
        Bo1_Log("RaiseException 0x%08x", (unsigned)code);
}
extern "C" LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { return NULL; }
extern "C" LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *) { return EXCEPTION_EXECUTE_HANDLER; }
extern "C" UINT WINAPI SetErrorMode(UINT) { return 0; }
extern "C" LPSTR WINAPI GetCommandLineA(void) { return Bo1_GetCommandLine(); }

extern "C" DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize)
{
    const char *v = getenv(lpName);
    if (!v)
    {
        t_lastError = 203; // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    DWORD len = (DWORD)strlen(v);
    if (len + 1 > nSize)
        return len + 1;
    memcpy(lpBuffer, v, len + 1);
    return len;
}

extern "C" BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue)
{
    return (lpValue ? setenv(lpName, lpValue, 1) : unsetenv(lpName)) == 0;
}

extern "C" DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize)
{
    DWORD len = (DWORD)strlen(lpSrc);
    if (lpDst && nSize > len)
        memcpy(lpDst, lpSrc, len + 1);
    return len + 1;
}

extern "C" void WINAPI ExitProcess(UINT uExitCode)
{
    Bo1_Exit((int)uExitCode);
    __builtin_unreachable();
}

extern "C" BOOL WINAPI TerminateProcess(HANDLE, UINT uExitCode)
{
    Bo1_Exit((int)uExitCode);
    return TRUE;
}

extern "C" BOOL WINAPI GetExitCodeProcess(HANDLE, LPDWORD lpExitCode)
{
    *lpExitCode = STILL_ACTIVE;
    return TRUE;
}

extern "C" HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD) { return NULL; }

extern "C" BOOL WINAPI GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize)
{
    I_strncpyz_web(lpBuffer, "BROWSER", *nSize);
    *nSize = 7;
    return TRUE;
}

extern "C" BOOL WINAPI GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer)
{
    I_strncpyz_web(lpBuffer, "player", *pcbBuffer);
    *pcbBuffer = 7;
    return TRUE;
}

extern "C" BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA v)
{
    // Windows 7 SP1, the newest the engine knows about
    DWORD size = v->dwOSVersionInfoSize;
    memset(v, 0, size);
    v->dwOSVersionInfoSize = size;
    v->dwMajorVersion = 6;
    v->dwMinorVersion = 1;
    v->dwBuildNumber = 7601;
    v->dwPlatformId = VER_PLATFORM_WIN32_NT;
    strcpy(v->szCSDVersion, "Service Pack 1");
    return TRUE;
}

extern "C" DWORD WINAPI GetVersion(void) { return (7601u << 16) | (1u << 8) | 6u; }

extern "C" DWORD WINAPI FormatMessageA(DWORD dwFlags, LPCVOID, DWORD dwMessageId, DWORD, LPSTR lpBuffer, DWORD nSize, va_list *)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "error %u", (unsigned)dwMessageId);
    if (dwFlags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        *(LPSTR *)lpBuffer = strdup(msg);
        return (DWORD)strlen(msg);
    }
    I_strncpyz_web(lpBuffer, msg, nSize);
    return (DWORD)strlen(lpBuffer);
}

extern "C" LCID WINAPI GetUserDefaultLCID(void) { return 0x0409; }
extern "C" LCID WINAPI GetSystemDefaultLCID(void) { return 0x0409; }
extern "C" LCID WINAPI GetThreadLocale(void) { return 0x0409; }
extern "C" LANGID WINAPI GetUserDefaultLangID(void) { return 0x0409; }
extern "C" LANGID WINAPI GetSystemDefaultLangID(void) { return 0x0409; }
extern "C" LANGID WINAPI GetUserDefaultUILanguage(void) { return 0x0409; }
extern "C" LANGID WINAPI GetSystemDefaultUILanguage(void) { return 0x0409; }
extern "C" UINT WINAPI GetACP(void) { return 1252; }

extern "C" int WINAPI GetLocaleInfoA(LCID, DWORD LCType, LPSTR lpLCData, int cchData)
{
    const char *v = "";
    switch (LCType)
    {
    case LOCALE_SENGLANGUAGE:
        v = "English";
        break;
    case LOCALE_SABBREVLANGNAME:
        v = "ENU";
        break;
    case LOCALE_SISO639LANGNAME:
        v = "en";
        break;
    case LOCALE_SISO3166CTRYNAME:
        v = "US";
        break;
    case LOCALE_IDEFAULTANSICODEPAGE:
        v = "1252";
        break;
    }
    int len = (int)strlen(v) + 1;
    if (!cchData)
        return len;
    I_strncpyz_web(lpLCData, v, cchData);
    return len;
}

extern "C" int WINAPI MultiByteToWideChar(UINT, DWORD, LPCSTR src, int cb, LPWSTR dst, int cch)
{
    int len = cb < 0 ? (int)strlen(src) + 1 : cb;
    if (!cch)
        return len;
    int n = len < cch ? len : cch;
    for (int i = 0; i < n; ++i)
        dst[i] = (unsigned char)src[i];
    return n;
}

extern "C" int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int cch, LPSTR dst, int cb, LPCSTR, LPBOOL)
{
    int len = 0;
    if (cch < 0)
    {
        while (src[len])
            ++len;
        ++len;
    }
    else
    {
        len = cch;
    }
    if (!cb)
        return len;
    int n = len < cb ? len : cb;
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] < 0x100 ? (char)src[i] : '?';
    return n;
}

extern "C" int WINAPI MulDiv(int a, int b, int c) { return c ? (int)(((long long)a * b) / c) : -1; }

extern "C" int WINAPI wvsprintfA(LPSTR buf, LPCSTR fmt, va_list ap) { return vsprintf(buf, fmt, ap); }

extern "C" int WINAPIV wsprintfA(LPSTR buf, LPCSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

extern "C" LPSTR WINAPI lstrcpyA(LPSTR a, LPCSTR b) { return strcpy(a, b); }
extern "C" LPSTR WINAPI lstrcpynA(LPSTR a, LPCSTR b, int n)
{
    I_strncpyz_web(a, b, n);
    return a;
}
extern "C" LPSTR WINAPI lstrcatA(LPSTR a, LPCSTR b) { return strcat(a, b); }
extern "C" int WINAPI lstrlenA(LPCSTR a) { return a ? (int)strlen(a) : 0; }
extern "C" int WINAPI lstrcmpA(LPCSTR a, LPCSTR b) { return strcmp(a, b); }
extern "C" int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b) { return strcasecmp(a, b); }
extern "C" BOOL WINAPI IsBadReadPtr(const void *lp, UINT_PTR) { return lp == NULL; }
extern "C" BOOL WINAPI IsBadWritePtr(LPVOID lp, UINT_PTR) { return lp == NULL; }
extern "C" BOOL WINAPI IsProcessorFeaturePresent(DWORD) { return FALSE; }
extern "C" BOOL WINAPI FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return TRUE; }

extern "C" BOOL WINAPI ReadProcessMemory(HANDLE, LPCVOID base, LPVOID buf, SIZE_T n, SIZE_T *read)
{
    memcpy(buf, base, n);
    if (read)
        *read = n;
    return TRUE;
}

extern "C" BOOL WINAPI WriteProcessMemory(HANDLE, LPVOID base, LPCVOID buf, SIZE_T n, SIZE_T *written)
{
    memcpy(base, buf, n);
    if (written)
        *written = n;
    return TRUE;
}

extern "C" HRSRC WINAPI FindResourceA(HMODULE, LPCSTR, LPCSTR) { return NULL; }
extern "C" HGLOBAL WINAPI LoadResource(HMODULE, HRSRC) { return NULL; }
extern "C" LPVOID WINAPI LockResource(HGLOBAL) { return NULL; }
extern "C" DWORD WINAPI SizeofResource(HMODULE, HRSRC) { return 0; }

extern "C" BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG ReturnedLength, HANDLE, ULONG)
{
    if (ReturnedLength)
        *ReturnedLength = 0;
    t_lastError = ERROR_NOT_SUPPORTED;
    return FALSE;
}

extern "C" BOOL WINAPI OpenProcessToken(HANDLE, DWORD, PHANDLE) { return FALSE; }
extern "C" BOOL WINAPI LookupPrivilegeValueA(LPCSTR, LPCSTR, PLUID) { return FALSE; }
extern "C" BOOL WINAPI AdjustTokenPrivileges(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD) { return FALSE; }
extern "C" BOOL WINAPI GetProcessHandleCount(HANDLE, PDWORD c)
{
    *c = 0;
    return TRUE;
}
extern "C" BOOL WINAPI IsProcessDPIAware(void) { return TRUE; }
extern "C" BOOL WINAPI SetProcessDPIAware(void) { return TRUE; }

extern "C" HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD) { return INVALID_HANDLE_VALUE; }
extern "C" BOOL WINAPI Thread32First(HANDLE, LPTHREADENTRY32) { return FALSE; }
extern "C" BOOL WINAPI Thread32Next(HANDLE, LPTHREADENTRY32) { return FALSE; }
extern "C" BOOL WINAPI Process32First(HANDLE, LPPROCESSENTRY32) { return FALSE; }
extern "C" BOOL WINAPI Process32Next(HANDLE, LPPROCESSENTRY32) { return FALSE; }
extern "C" BOOL WINAPI GetModuleInformation(HANDLE, HMODULE, LPMODULEINFO, DWORD) { return FALSE; }
extern "C" BOOL WINAPI EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD) { return FALSE; }
extern "C" DWORD WINAPI GetModuleBaseNameA(HANDLE, HMODULE, LPSTR, DWORD) { return 0; }
extern "C" BOOL WINAPI EnumProcesses(DWORD *, DWORD, LPDWORD) { return FALSE; }
extern "C" BOOL WINAPI SymInitialize(HANDLE, PCSTR, BOOL) { return FALSE; }
extern "C" BOOL WINAPI SymCleanup(HANDLE) { return TRUE; }
extern "C" DWORD WINAPI SymSetOptions(DWORD o) { return o; }
extern "C" DWORD WINAPI SymGetOptions(void) { return 0; }
extern "C" BOOL WINAPI SymFromAddr(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO) { return FALSE; }
extern "C" BOOL WINAPI SymGetLineFromAddr64(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64) { return FALSE; }

extern "C" BOOL WINAPI CryptAcquireContextA(HCRYPTPROV *phProv, LPCSTR, LPCSTR, DWORD, DWORD)
{
    *phProv = 1;
    return TRUE;
}

extern "C" BOOL WINAPI CryptGenRandom(HCRYPTPROV, DWORD dwLen, BYTE *pbBuffer)
{
    // getentropy is limited to 256 bytes per call
    for (DWORD done = 0; done < dwLen;)
    {
        DWORD n = dwLen - done > 256 ? 256 : dwLen - done;
        getentropy(pbBuffer + done, n);
        done += n;
    }
    return TRUE;
}

extern "C" BOOL WINAPI CryptReleaseContext(HCRYPTPROV, DWORD) { return TRUE; }

const GUID bo1_GUID_NULL = {};
EXTERN_C const GUID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
EXTERN_C const GUID IID_IDirectDraw7 = {};

extern "C" HRESULT WINAPI CoInitialize(LPVOID) { return S_OK; }
extern "C" HRESULT WINAPI CoInitializeEx(LPVOID, DWORD) { return S_OK; }
extern "C" void WINAPI CoUninitialize(void) {}
HRESULT Bo1_CreateXAudio2(REFCLSID clsid, void **ppv); // xaudio2_web.cpp

extern "C" HRESULT WINAPI CoCreateInstance(REFCLSID rclsid, LPUNKNOWN, DWORD, REFIID, LPVOID *ppv)
{
    *ppv = NULL;
    // the only COM class the engine creates: XAudio2 (XAudio2Create, XAudio2.h)
    return Bo1_CreateXAudio2(rclsid, ppv);
}
extern "C" HRESULT WINAPI CoCreateGuid(GUID *g)
{
    getentropy(g, sizeof(*g));
    return S_OK;
}
extern "C" void WINAPI CoTaskMemFree(LPVOID pv) { free(pv); }
extern "C" LPVOID WINAPI CoTaskMemAlloc(SIZE_T cb) { return malloc(cb ? cb : 1); }

// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
extern "C" HRESULT WINAPI CLSIDFromString(const wchar_t *s, LPCLSID clsid)
{
    char a[39];
    for (int i = 0; i < 38; ++i)
    {
        if (!s[i] || s[i] > 0x7F)
            return E_INVALIDARG;
        a[i] = (char)s[i];
    }
    a[38] = 0;
    unsigned int d1, d2, d3, b[8];
    if (a[0] != '{' || a[37] != '}' ||
        sscanf(a, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return E_INVALIDARG;
    clsid->Data1 = d1;
    clsid->Data2 = (unsigned short)d2;
    clsid->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i)
        clsid->Data4[i] = (unsigned char)b[i];
    return S_OK;
}

// ============================================================================================================
// registry: none
// ============================================================================================================

extern "C" LONG WINAPI RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, PHKEY phkResult)
{
    *phkResult = NULL;
    return ERROR_FILE_NOT_FOUND;
}
extern "C" LONG WINAPI RegOpenKeyA(HKEY, LPCSTR, PHKEY phkResult)
{
    *phkResult = NULL;
    return ERROR_FILE_NOT_FOUND;
}
extern "C" LONG WINAPI RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY phkResult, LPDWORD)
{
    *phkResult = NULL;
    return ERROR_ACCESS_DENIED;
}
extern "C" LONG WINAPI RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
extern "C" LONG WINAPI RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD) { return ERROR_ACCESS_DENIED; }
extern "C" LONG WINAPI RegDeleteValueA(HKEY, LPCSTR) { return ERROR_FILE_NOT_FOUND; }
extern "C" LONG WINAPI RegDeleteKeyA(HKEY, LPCSTR) { return ERROR_FILE_NOT_FOUND; }
extern "C" LONG WINAPI RegCloseKey(HKEY) { return ERROR_SUCCESS; }
extern "C" LONG WINAPI RegEnumKeyExA(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR, LPDWORD, PFILETIME) { return ERROR_NO_MORE_ITEMS; }

// ============================================================================================================
// DirectSound / DirectDraw: not in a browser
// ============================================================================================================

extern "C" HRESULT WINAPI DirectSoundCreate8(LPCGUID, LPDIRECTSOUND8 *ppDS8, LPUNKNOWN)
{
    *ppDS8 = NULL;
    return DSERR_NODRIVER;
}

extern "C" HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID, LPDIRECTSOUNDCAPTURE *ppDSC, LPUNKNOWN)
{
    *ppDSC = NULL;
    return DSERR_NODRIVER;
}
