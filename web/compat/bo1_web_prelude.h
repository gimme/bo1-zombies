// Force-included first in every web translation unit (-include bo1_web_prelude.h).
//
// The engine is MSVC code: this supplies the MSVC language and CRT extensions it relies on that clang targeting
// wasm32 does not (secure CRT functions, _stricmp & co., SEH keywords, x86-only intrinsics). Windows API types and
// functions are in windows.h (web/compat), which the engine includes itself.
#pragma once

#ifndef BO1_WEB_PRELUDE_H
#define BO1_WEB_PRELUDE_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
// POSIX's long random(void) and BSD's index() would clash with the engine's own random() (com_math.h) and
// globals named index
#define random bo1_libc_random
#define index bo1_libc_index
#define rindex bo1_libc_rindex
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#undef random
#undef index
#undef rindex
// The shipped game had its asserts compiled out; the decompiled source checks them again (Assert_MyHandler, then
// __debugbreak). Here a failed assert is logged with its call stack (web_main.cpp) and the game carries on, as the
// release build did.
#define __debugbreak() ((void)0)
// MSVC's rand() returns 0..0x7FFF (musl's goes to 2^31), and the engine divides by 32768 to get [0, 1): use MSVC's
// generator, per thread as there (web/src/crt_compat.cpp)
#ifdef __cplusplus
extern "C" {
#endif
int bo1_rand(void);
void bo1_srand(unsigned int seed);
void bo1_qsort(void *base, size_t num, size_t width, int (*comp)(const void *, const void *));
#ifdef __cplusplus
}
#endif
#define rand() bo1_rand()
#define srand(seed) bo1_srand(seed)
#undef RAND_MAX
#define RAND_MAX 0x7FFF
// MSVC's qsort too: equal elements must end in the order they do on Windows, as the network's Huffman tree is built
// with it (crt_compat.cpp)
#define qsort bo1_qsort
#include <setjmp.h>
// The engine keeps its jump buffers as MSVC's int[16] and passes int*. Emscripten's wasm setjmp stores 16 bytes of
// its 156-byte jmp_buf (a struct jmp_buf_impl), so the 64-byte buffers hold it. setjmp must stay a direct call.
#define _setjmp(buf) setjmp((struct __jmp_buf_tag *)(buf))
#define setjmp(buf) setjmp((struct __jmp_buf_tag *)(buf))
#define longjmp(buf, val) longjmp((struct __jmp_buf_tag *)(buf), (val))
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <alloca.h>

#ifdef __cplusplus
#define BO1_EXTERN_C extern "C"
#define BO1_EXTERN_C_BEGIN extern "C" {
#define BO1_EXTERN_C_END }
#else
#define BO1_EXTERN_C extern
#define BO1_EXTERN_C_BEGIN
#define BO1_EXTERN_C_END
#endif

#define EXTERN_C BO1_EXTERN_C
#define interface struct
typedef struct _GUID GUID;

// ----- keywords (IDA's calling-convention annotations; clang's -fms-extensions knows the MSVC ones) -----
#define __usercall
#define __userpurge
#define __spoils(...)
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define __assume(x) ((void)0)
#ifndef __noop
#define __noop(...) ((void)0)
#endif

// SEH: the browser has no structured exceptions; a __try body just runs (the only user is SetThreadName's
// debugger notification, whose __except path is for "no debugger attached").
#define __try if (1)
#define __except(x) else if (0)
#define __finally
#define __leave
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

// ----- CRT names -----
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _strcmpi strcasecmp
#define strcmpi strcasecmp
#define _memicmp bo1_memicmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _strdup strdup
#define _fileno fileno
#define _isnan isnan
#define _finite(x) isfinite(x)
#define _copysign copysign
#define _hypot hypot
#define _chgsign(x) (-(x))
#define _atoi64(s) strtoll((s), NULL, 10)
#define _strtoi64 strtoll
#define _strtoui64 strtoull
#define _alloca alloca
#define _getcwd bo1_getcwd
#define _chdir bo1_chdir
#define _unlink unlink
#define _access access
#define _close close
#define _read read
#define _write write
#define _lseek lseek
#define _open open
#define _O_RDONLY O_RDONLY
#define _O_BINARY 0
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _getpid getpid
#define __max(a, b) (((a) > (b)) ? (a) : (b))
#define __min(a, b) (((a) < (b)) ? (a) : (b))
#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256
#define _TRUNCATE ((size_t)-1)
#define _CRT_ERRNO_DEFINED
#define _OUT_TO_STDERR 1
#define _WRITE_ABORT_MSG 1
#define _set_error_mode(x) 0
#define _set_abort_behavior(a, b) 0
#define _CrtSetDbgFlag(x) 0
#define _CrtDbgReport(...) 0
#define _ASSERTE(x) ((void)0)
#define _RPT0(...) ((void)0)
#define _RPT1(...) ((void)0)
#define _heapchk() (-2)
#define _HEAPOK (-2)
#define _aligned_malloc(size, align) bo1_aligned_malloc((size), (align))
#define _aligned_free(p) free(p)
#define _msize(p) bo1_msize(p)
#define _isatty isatty
#define _environ environ
#define _rmdir rmdir

// CRT calls the engine makes with Windows paths ('\' separators): web/src/crt_compat.cpp normalizes them
BO1_EXTERN_C FILE *bo1_fopen(const char *path, const char *mode);
BO1_EXTERN_C int bo1_rename(const char *from, const char *to);
BO1_EXTERN_C int bo1_mkdir(const char *path);
BO1_EXTERN_C char *bo1_getcwd(char *buf, size_t size);
BO1_EXTERN_C int bo1_chdir(const char *path);
#define fopen(p, m) bo1_fopen((p), (m))
#define rename(a, b) bo1_rename((a), (b))
#define _mkdir(p) bo1_mkdir(p)

typedef long long __time64_t;
typedef FILE _iobuf; // MSVC's FILE struct tag, which the decompiled code names directly
typedef int(__cdecl *_CoreCrtNonSecureSearchSortCompareFunction)(void const *, void const *);
typedef int(__cdecl *_CoreCrtSecureSearchSortCompareFunction)(void *, void const *, void const *);
#define _m_empty() ((void)0)
#define _mm_empty() ((void)0)
#define _errno() (&errno)
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define _ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef int errno_t;
typedef unsigned short wchar16_t;

BO1_EXTERN_C_BEGIN
void *bo1_aligned_malloc(size_t size, size_t align);
size_t bo1_msize(void *p);
int bo1_memicmp(const void *a, const void *b, size_t n);
char *_itoa(int value, char *str, int radix);
char *_ltoa(long value, char *str, int radix);
char *_ultoa(unsigned long value, char *str, int radix);
char *_i64toa(long long value, char *str, int radix);
char *_ui64toa(unsigned long long value, char *str, int radix);
char *_strlwr(char *s);
char *_strupr(char *s);
char *_strrev(char *s);
int _putenv(const char *envstring);
void *_aligned_realloc(void *memblock, size_t size, size_t alignment);
int _vscprintf(const char *fmt, va_list ap);
int _scprintf(const char *fmt, ...);
long _filelength(int fd);
void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext);
void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext);
char *_fullpath(char *absPath, const char *relPath, size_t maxLength);
int _kbhit(void);
int _getch(void);
unsigned int _controlfp(unsigned int newval, unsigned int mask);
unsigned int _control87(unsigned int newval, unsigned int mask);
void _fpreset(void);
int _vsnprintf_s(char *buffer, size_t sizeOfBuffer, size_t count, const char *format, va_list argptr);
int _snprintf_s(char *buffer, size_t sizeOfBuffer, size_t count, const char *format, ...);
int vsprintf_s(char *buffer, size_t numberOfElements, const char *format, va_list argptr);
int sprintf_s(char *buffer, size_t sizeOfBuffer, const char *format, ...);
int vsnprintf_s(char *buffer, size_t sizeOfBuffer, size_t count, const char *format, va_list argptr);
errno_t strcpy_s(char *dest, size_t destsz, const char *src);
errno_t strncpy_s(char *dest, size_t destsz, const char *src, size_t count);
errno_t strcat_s(char *dest, size_t destsz, const char *src);
errno_t strncat_s(char *dest, size_t destsz, const char *src, size_t count);
errno_t _itoa_s(int value, char *buffer, size_t size, int radix);
errno_t _ltoa_s(long value, char *buffer, size_t size, int radix);
errno_t memcpy_s(void *dest, size_t destsz, const void *src, size_t count);
errno_t memmove_s(void *dest, size_t destsz, const void *src, size_t count);
errno_t fopen_s(FILE **pFile, const char *filename, const char *mode);
errno_t _strlwr_s(char *s, size_t size);
errno_t _strupr_s(char *s, size_t size);
errno_t localtime_s(struct tm *tmDest, const long long *sourceTime);
errno_t _localtime64_s(struct tm *tmDest, const long long *sourceTime);
errno_t _gmtime64_s(struct tm *tmDest, const long long *sourceTime);
errno_t ctime_s(char *buffer, size_t numberOfElements, const long long *sourceTime);
errno_t _ctime64_s(char *buffer, size_t numberOfElements, const long long *sourceTime);
errno_t asctime_s(char *buffer, size_t numberOfElements, const struct tm *tmSource);
errno_t _dupenv_s(char **buffer, size_t *numberOfElements, const char *varname);
errno_t getenv_s(size_t *pReturnValue, char *buffer, size_t numberOfElements, const char *varname);
errno_t _get_timezone(long *seconds);
int sscanf_s(const char *buffer, const char *format, ...);
char *strtok_s(char *strToken, const char *strDelimit, char **context);
size_t strnlen_s(const char *str, size_t numberOfElements);
long long _time64(long long *t);
struct tm *_gmtime64(const long long *t);
long long _mktime64(struct tm *t);
struct tm *_localtime64(const long long *t);
char *_ctime64(const long long *t);
BO1_EXTERN_C_END

#ifdef __cplusplus
// MSVC's C++ template overloads of the secure CRT that take the destination array's size from its type
template <size_t N> inline int sprintf_s(char (&buffer)[N], const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = vsprintf_s(buffer, N, format, ap);
    va_end(ap);
    return r;
}
template <size_t N> inline int vsprintf_s(char (&buffer)[N], const char *format, va_list ap)
{
    return vsprintf_s(buffer, N, format, ap);
}
template <size_t N> inline int _snprintf_s(char (&buffer)[N], size_t count, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = _vsnprintf_s(buffer, N, count, format, ap);
    va_end(ap);
    return r;
}
template <size_t N> inline int _vsnprintf_s(char (&buffer)[N], size_t count, const char *format, va_list ap)
{
    return _vsnprintf_s(buffer, N, count, format, ap);
}
template <size_t N> inline errno_t strcpy_s(char (&dest)[N], const char *src) { return strcpy_s(dest, N, src); }
template <size_t N> inline errno_t strncpy_s(char (&dest)[N], const char *src, size_t count) { return strncpy_s(dest, N, src, count); }
template <size_t N> inline errno_t strcat_s(char (&dest)[N], const char *src) { return strcat_s(dest, N, src); }
template <size_t N> inline errno_t strncat_s(char (&dest)[N], const char *src, size_t count) { return strncat_s(dest, N, src, count); }
template <size_t N> inline errno_t _itoa_s(int value, char (&buffer)[N], int radix) { return _itoa_s(value, buffer, N, radix); }
template <size_t N> inline errno_t _strlwr_s(char (&s)[N]) { return _strlwr_s(s, N); }
template <size_t N> inline errno_t _strupr_s(char (&s)[N]) { return _strupr_s(s, N); }
#endif

// ----- MSVC intrinsics with no wasm counterpart -----
// __unaligned is an MSVC keyword under -fms-extensions, and the name of a helper struct in Emscripten's SSE headers
#define __unaligned __bo1_unaligned_t

static inline unsigned char _BitScanForward(unsigned long *index, unsigned long mask)
{
    if (!mask)
        return 0;
    *index = (unsigned long)__builtin_ctz(mask);
    return 1;
}
static inline unsigned char _BitScanReverse(unsigned long *index, unsigned long mask)
{
    if (!mask)
        return 0;
    *index = 31ul - (unsigned long)__builtin_clz(mask);
    return 1;
}
static inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
static inline unsigned long _byteswap_ulong(unsigned long v) { return __builtin_bswap32((uint32_t)v); }
static inline unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }
BO1_EXTERN_C unsigned long long __rdtsc(void);
// TEB reads: 0x24 is the current thread id (profile.h)
BO1_EXTERN_C unsigned long __readfsdword(unsigned long offset);
static inline void __cpuid(int info[4], int function_id)
{
    (void)function_id;
    info[0] = info[1] = info[2] = info[3] = 0;
}
static inline void __cpuidex(int info[4], int function_id, int subfunction_id)
{
    (void)function_id;
    (void)subfunction_id;
    info[0] = info[1] = info[2] = info[3] = 0;
}
#define _ReturnAddress() __builtin_return_address(0)
#define _AddressOfReturnAddress() ((void *)0)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _ReadBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _WriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() ((void)0)

#ifdef __cplusplus
#include <new> // MSVC declares placement new without it

// MSVC's _Interlocked* intrinsics accept any 32-bit integer pointer; clang's builtins want exactly long
template <class T, class V> inline T bo1_InterlockedExchangeAdd(volatile T *p, V v) { return __atomic_fetch_add(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T> inline T bo1_InterlockedIncrement(volatile T *p) { return __atomic_add_fetch(p, (T)1, __ATOMIC_SEQ_CST); }
template <class T> inline T bo1_InterlockedDecrement(volatile T *p) { return __atomic_sub_fetch(p, (T)1, __ATOMIC_SEQ_CST); }
template <class T, class V> inline T bo1_InterlockedExchange(volatile T *p, V v) { return __atomic_exchange_n(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class V> inline T bo1_InterlockedOr(volatile T *p, V v) { return __atomic_fetch_or(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class V> inline T bo1_InterlockedAnd(volatile T *p, V v) { return __atomic_fetch_and(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class V> inline T bo1_InterlockedXor(volatile T *p, V v) { return __atomic_fetch_xor(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class V, class W> inline T bo1_InterlockedCompareExchange(volatile T *p, V exchange, W comparand)
{
    T expected = (T)comparand;
    __atomic_compare_exchange_n(p, &expected, (T)exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
template <class T> inline T *bo1_InterlockedCompareExchangePointer(T *volatile *p, T *exchange, T *comparand)
{
    __atomic_compare_exchange_n(p, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline void *bo1_InterlockedExchangePointer(void *volatile *p, void *v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline void *bo1_InterlockedCompareExchangePointer(void *volatile *p, void *exchange, void *comparand)
{
    __atomic_compare_exchange_n(p, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
template <class T> inline T *bo1_InterlockedExchangePointer(T *volatile *p, T *v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
#define _InterlockedExchangeAdd bo1_InterlockedExchangeAdd
#define _InterlockedIncrement bo1_InterlockedIncrement
#define _InterlockedDecrement bo1_InterlockedDecrement
#define _InterlockedExchange bo1_InterlockedExchange
#define _InterlockedOr bo1_InterlockedOr
#define _InterlockedAnd bo1_InterlockedAnd
#define _InterlockedXor bo1_InterlockedXor
#define _InterlockedCompareExchange bo1_InterlockedCompareExchange
#define _InterlockedCompareExchange64 bo1_InterlockedCompareExchange
#define _InterlockedExchangeAdd64 bo1_InterlockedExchangeAdd
#define _InterlockedIncrement64 bo1_InterlockedIncrement
#define _InterlockedDecrement64 bo1_InterlockedDecrement
#define _InterlockedExchange64 bo1_InterlockedExchange
#define _InterlockedIncrement16 bo1_InterlockedIncrement
#define _InterlockedDecrement16 bo1_InterlockedDecrement
#define _InterlockedCompareExchange16 bo1_InterlockedCompareExchange
#define _InterlockedCompareExchangePointer bo1_InterlockedCompareExchangePointer
#define _InterlockedExchangePointer bo1_InterlockedExchangePointer
#endif

// The DirectX SDK's comdecl.h names interfaces with __declspec(uuid) / __uuidof (MSVC only): claim its include
// guard so the SDK headers get these plain extern IIDs instead (defined in web/src where something uses them).
#define _COMDECL_H_
#define DEFINE_CLSID(className, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) EXTERN_C const GUID CLSID_##className
#ifdef __cplusplus
#define DEFINE_IID(interfaceName, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)     interface interfaceName;                                                  EXTERN_C const GUID IID_##interfaceName
#define __uuidof(x) IID_##x
#else
#define DEFINE_IID(interfaceName, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) EXTERN_C const GUID IID_##interfaceName
#endif

#endif // BO1_WEB_PRELUDE_H
