// Windows API for the WebAssembly build: the ILP32 Win32 types the engine was written against, and the subset of
// kernel32 / user32 / gdi32 / advapi32 / winmm it calls. Implemented on top of Emscripten (pthreads, the virtual file
// system, the browser) in web/src/win32_compat.cpp; window, GDI and registry calls are inert there.
#pragma once

#ifndef BO1_WEB_WINDOWS_H
#define BO1_WEB_WINDOWS_H

#include "bo1_web_prelude.h"
#include <pthread.h>

// ----- calling conventions / annotations -----
#define WINAPI __stdcall
#define WINAPIV __cdecl
#define APIENTRY WINAPI
#define CALLBACK __stdcall
#define PASCAL __stdcall
#define STDMETHODCALLTYPE __stdcall
#define STDMETHODVCALLTYPE __cdecl
#define STDAPICALLTYPE __stdcall
#define NTAPI __stdcall
#define WINBASEAPI
#define WINUSERAPI
#define WINGDIAPI
#define WINADVAPI
#define WINMMAPI
#define DECLSPEC_IMPORT
#define DECLSPEC_NORETURN __declspec(noreturn)
#define DECLSPEC_ALIGN(x) __declspec(align(x))
#define DECLSPEC_NOVTABLE
#define DECLSPEC_UUID(x)
#define DECLSPEC_SELECTANY
#define FORCEINLINE __forceinline
#define CONST const
#define VOID void
#define IN
#define OUT
#define OPTIONAL
#define FAR
#define NEAR
#define far
#define near
#define UNALIGNED
#define RESTRICTED_POINTER
#define UNREFERENCED_PARAMETER(P) ((void)(P))
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _In_z_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _In_reads_opt_(x)
#define _Out_writes_(x)
#define _Out_writes_bytes_(x)
#define _Out_writes_opt_(x)
#define _Out_writes_z_(x)
#define _Printf_format_string_
#define _Check_return_
#define _Ret_maybenull_
#define _Success_(x)
#define __in
#define __in_opt
#define __out
#define __out_opt
#define __inout
#define __in_ecount(x)
#define __out_ecount(x)
#define __in_bcount(x)
#define __out_bcount(x)
#define __out_bcount_opt(x)
#define __in_bcount_opt(x)
#define __inout_bcount(x)
#define __deref_out
#define __deref_out_opt
#define __reserved
#define __nullterminated
#define __field_ecount(x)
#define __field_bcount(x)
#define __field_ecount_opt(x)
#define __field_bcount_opt(x)
#define __in_z
#define __in_z_opt
#define __in_ecount_opt(x)
#define __out_ecount_opt(x)
#define __inout_ecount_opt(x)
#define __deref_opt_out
#define __deref_opt_out_bcount(x)
#define __deref_out_bcount(x)
#define __checkReturn

// ----- base types (Win32 is ILP32 like wasm32: long and pointers are 4 bytes) -----
typedef int BOOL;
typedef BOOL *PBOOL, *LPBOOL;
typedef unsigned char BYTE, UCHAR, BOOLEAN;
typedef BYTE *PBYTE, *LPBYTE, *PUCHAR;
typedef BOOLEAN *PBOOLEAN;
typedef unsigned short WORD, USHORT, ATOM, LANGID;
typedef WORD *PWORD, *LPWORD;
typedef unsigned long DWORD, ULONG;
typedef DWORD *PDWORD, *LPDWORD;
typedef ULONG *PULONG;
typedef float FLOAT;
typedef double DOUBLE;
typedef FLOAT *PFLOAT;
typedef int INT;
typedef int *PINT, *LPINT;
typedef unsigned int UINT;
typedef unsigned int *PUINT;
typedef long LONG;
typedef LONG *PLONG, *LPLONG;
typedef short SHORT;
typedef char CHAR, CCHAR;
typedef wchar_t WCHAR;
typedef long long LONGLONG, INT64, LONG64;
typedef unsigned long long ULONGLONG, DWORDLONG, DWORD64, UINT64, ULONG64, QWORD;
typedef DWORD64 *PDWORD64;
typedef ULONG64 *PULONG64;
typedef int INT32, LONG32;
typedef unsigned int UINT32, ULONG32, DWORD32;
typedef signed char INT8;
typedef unsigned char UINT8;
typedef short INT16;
typedef unsigned short UINT16;
typedef int INT_PTR, *PINT_PTR;
typedef unsigned int UINT_PTR, *PUINT_PTR;
typedef long LONG_PTR, *PLONG_PTR;
typedef unsigned long ULONG_PTR, *PULONG_PTR;
typedef ULONG_PTR DWORD_PTR, *PDWORD_PTR;
typedef ULONG_PTR SIZE_T, *PSIZE_T;
typedef LONG_PTR SSIZE_T, *PSSIZE_T;
typedef void *PVOID, *LPVOID;
typedef const void *LPCVOID, *PCVOID;
typedef void *HANDLE;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef long HRESULT;
typedef char *LPSTR, *PSTR, *NPSTR;
typedef const char *LPCSTR, *PCSTR;
typedef WCHAR *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;
typedef char TCHAR, _TCHAR;
typedef LPSTR LPTSTR;
typedef LPCSTR LPCTSTR;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef DWORD COLORREF, *LPCOLORREF;
typedef DWORD LCID;
typedef DWORD ACCESS_MASK, REGSAM;
typedef LONG NTSTATUS;
typedef WORD FILEOP_FLAGS;
typedef HANDLE HGLOBAL, HLOCAL, HGDIOBJ;
typedef int HFILE;
typedef unsigned short wchar;

#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__ *name
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HINSTANCE);
typedef HINSTANCE HMODULE;
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HICON);
typedef HICON HCURSOR;
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HMONITOR);
#define HMONITOR_DECLARED
DECLARE_HANDLE(HKEY);
typedef HKEY *PHKEY;
DECLARE_HANDLE(HGLRC);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HRAWINPUT);
DECLARE_HANDLE(HHOOK);
DECLARE_HANDLE(HDESK);
DECLARE_HANDLE(HWINSTA);
DECLARE_HANDLE(HACCEL);
DECLARE_HANDLE(HDROP);
DECLARE_HANDLE(HKL);
DECLARE_HANDLE(HIMC);
DECLARE_HANDLE(HDEVNOTIFY);
DECLARE_HANDLE(HRSRC);
typedef HICON HMETAFILE;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFF
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define MAXDWORD 0xffffffff
#define MAXLONG 0x7fffffff
#define MAXWORD 0xffff
#define MAXBYTE 0xff
#define MINLONG 0x80000000
#define MAXUINT ((UINT)~((UINT)0))
#define MAXINT ((INT)(MAXUINT >> 1))
#define MININT ((INT)~MAXINT)

#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))
#define GET_X_LPARAM(lp) ((int)(short)((DWORD_PTR)(lp) & 0xffff))
#define GET_Y_LPARAM(lp) ((int)(short)(((DWORD_PTR)(lp) >> 16) & 0xffff))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define TEXT(x) x
#define _T(x) x
#define __TEXT(x) x
#define ZeroMemory(d, l) memset((d), 0, (l))
#define RtlZeroMemory(d, l) memset((d), 0, (l))
#define SecureZeroMemory(d, l) memset((d), 0, (l))
#define FillMemory(d, l, f) memset((d), (f), (l))
#define CopyMemory(d, s, l) memcpy((d), (s), (l))
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))
#define MoveMemory(d, s, l) memmove((d), (s), (l))
#define FIELD_OFFSET(type, field) ((LONG)offsetof(type, field))
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef LOBYTE
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#endif

// ----- HRESULT / errors -----
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_HANDLE ((HRESULT)0x80070006L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define HRESULT_CODE(hr) ((hr) & 0xFFFF)
#define HRESULT_FACILITY(hr) (((hr) >> 16) & 0x1fff)
#define SEVERITY_SUCCESS 0
#define SEVERITY_ERROR 1
#define MAKE_HRESULT(sev, fac, code) \
    ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#define MAKE_SCODE(sev, fac, code) MAKE_HRESULT(sev, fac, code)
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : ((HRESULT)(((x) & 0x0000FFFF) | (7 << 16) | 0x80000000)))
#define FACILITY_WIN32 7

#define NO_ERROR 0L
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_NOT_READY 21L
#define ERROR_CRC 23L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_HANDLE_EOF 38L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_DISK_FULL 112L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define WAIT_TIMEOUT 258L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_INCOMPLETE 996L
#define ERROR_IO_PENDING 997L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define ERROR_EMPTY 4306L
#define ERROR_CLASS_ALREADY_EXISTS 1410L

// ----- waits / threads -----
#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#define STATUS_ABANDONED_WAIT_0 ((DWORD)0x00000080L)
#define STATUS_TIMEOUT ((DWORD)0x00000102L)
#define WAIT_OBJECT_0 0
#define WAIT_ABANDONED 0x80
#define WAIT_ABANDONED_0 0x80
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define WAIT_IO_COMPLETION 0xC0
#define MAXIMUM_WAIT_OBJECTS 64
#define CREATE_SUSPENDED 0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_ERROR_RETURN (MAXLONG)
#define NORMAL_PRIORITY_CLASS 0x00000020
#define IDLE_PRIORITY_CLASS 0x00000040
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002
#define STILL_ACTIVE 259
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
#define SYNCHRONIZE 0x00100000L
#define EVENT_ALL_ACCESS 0x1F0003
#define MUTEX_ALL_ACCESS 0x1F0001
#define PROCESS_ALL_ACCESS 0x1FFFFF
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_VM_READ 0x0010
#define PROCESS_TERMINATE 0x0001
#define THREAD_ALL_ACCESS 0x1FFFFF
#define THREAD_TERMINATE 0x0001
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define LF_FACESIZE 32

typedef DWORD(WINAPI *PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;

typedef struct _SECURITY_ATTRIBUTES
{
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

// CRITICAL_SECTION: a recursive pthread mutex (initialized by InitializeCriticalSection)
typedef struct _RTL_CRITICAL_SECTION
{
    pthread_mutex_t mutex;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    ULONG_PTR SpinCount;
    int initialized;
} RTL_CRITICAL_SECTION, CRITICAL_SECTION, *PRTL_CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;

typedef struct _RTL_SRWLOCK
{
    PVOID Ptr;
} RTL_SRWLOCK, SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {0}

// ----- integers / time -----
typedef union _LARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        LONG HighPart;
    };
    struct
    {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct
    {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _LUID
{
    DWORD LowPart;
    LONG HighPart;
} LUID, *PLUID;

typedef struct _FILETIME
{
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME
{
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _TIME_ZONE_INFORMATION
{
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
} TIME_ZONE_INFORMATION, *PTIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2
#define TIME_ZONE_ID_INVALID ((DWORD)0xFFFFFFFF)

// ----- geometry -----
typedef struct tagPOINT
{
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT, POINTL, *PPOINTL;

typedef struct tagPOINTS
{
    SHORT x;
    SHORT y;
} POINTS, *PPOINTS, *LPPOINTS;

typedef struct tagRECT
{
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT, RECTL, *PRECTL, *LPRECTL;
typedef const RECT *LPCRECT;

typedef struct tagSIZE
{
    LONG cx;
    LONG cy;
} SIZE, *PSIZE, *LPSIZE, SIZEL;

typedef struct tagPALETTEENTRY
{
    BYTE peRed;
    BYTE peGreen;
    BYTE peBlue;
    BYTE peFlags;
} PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;

typedef struct _RGNDATAHEADER
{
    DWORD dwSize;
    DWORD iType;
    DWORD nCount;
    DWORD nRgnSize;
    RECT rcBound;
} RGNDATAHEADER, *PRGNDATAHEADER;

typedef struct _RGNDATA
{
    RGNDATAHEADER rdh;
    char Buffer[1];
} RGNDATA, *PRGNDATA, *LPRGNDATA;

// ----- GUID / COM -----
#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID
{
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
} GUID;
#endif
typedef GUID IID, CLSID, FMTID, UUID;
typedef GUID *LPGUID, *LPIID, *LPCLSID;
typedef const GUID *LPCGUID;
#ifdef __cplusplus
#define REFGUID const GUID &
#define REFIID const IID &
#define REFCLSID const IID &
#define REFFMTID const IID &
inline bool operator==(const GUID &a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
inline bool operator!=(const GUID &a, const GUID &b) { return !(a == b); }
#define IsEqualGUID(a, b) ((a) == (b))
#else
#define REFGUID const GUID *
#define REFIID const IID *
#define REFCLSID const IID *
#define IsEqualGUID(a, b) (!memcmp((a), (b), sizeof(GUID)))
#endif
#define IsEqualIID IsEqualGUID
#define IsEqualCLSID IsEqualGUID
#define GUID_NULL bo1_GUID_NULL
#define IID_NULL bo1_GUID_NULL
BO1_EXTERN_C const GUID bo1_GUID_NULL;

#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    BO1_EXTERN_C const GUID name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) BO1_EXTERN_C const GUID name
#endif

#ifdef __cplusplus
#define STDMETHOD(method) virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type, method) virtual type STDMETHODCALLTYPE method
#define STDMETHODV(method) virtual HRESULT STDMETHODVCALLTYPE method
#define STDMETHODV_(type, method) virtual type STDMETHODVCALLTYPE method
#define PURE = 0
#define THIS_
#define THIS void
#define DECLARE_INTERFACE(iface) interface DECLSPEC_NOVTABLE iface
#define DECLARE_INTERFACE_(iface, baseiface) interface DECLSPEC_NOVTABLE iface : public baseiface
#define DECLARE_INTERFACE_IID_(iface, baseiface, iid) interface DECLSPEC_NOVTABLE iface : public baseiface
#define MIDL_INTERFACE(x) struct DECLSPEC_NOVTABLE
#endif
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(type) type STDMETHODCALLTYPE
#define STDAPI BO1_EXTERN_C HRESULT STDAPICALLTYPE
#define STDAPI_(type) BO1_EXTERN_C type STDAPICALLTYPE

#ifdef __cplusplus
struct IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef(void) = 0;
    virtual ULONG STDMETHODCALLTYPE Release(void) = 0;
};
typedef IUnknown *LPUNKNOWN;
struct IStream;
#else
typedef void *LPUNKNOWN;
#endif

#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_ALL 0x17
#define COINIT_MULTITHREADED 0x0
#define COINIT_APARTMENTTHREADED 0x2
BO1_EXTERN_C HRESULT WINAPI CoInitialize(LPVOID pvReserved);
BO1_EXTERN_C HRESULT WINAPI CoInitializeEx(LPVOID pvReserved, DWORD dwCoInit);
BO1_EXTERN_C void WINAPI CoUninitialize(void);
BO1_EXTERN_C HRESULT WINAPI CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID *ppv);
BO1_EXTERN_C HRESULT WINAPI CoCreateGuid(GUID *pguid);
BO1_EXTERN_C void WINAPI CoTaskMemFree(LPVOID pv);
BO1_EXTERN_C LPVOID WINAPI CoTaskMemAlloc(SIZE_T cb);
BO1_EXTERN_C HRESULT WINAPI CLSIDFromString(const wchar_t *lpsz, LPCLSID pclsid);

// ----- files -----
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define GENERIC_EXECUTE 0x20000000L
#define GENERIC_ALL 0x10000000L
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_DEVICE 0x00000040
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_OFFLINE 0x00001000
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK 0x0001
#define FILE_TYPE_CHAR 0x0002
#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define DRIVE_CDROM 5
#define DRIVE_RAMDISK 6
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

typedef struct _OVERLAPPED
{
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union
    {
        struct
        {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef void(WINAPI *LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

typedef struct _WIN32_FIND_DATAA
{
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA, WIN32_FIND_DATA, *LPWIN32_FIND_DATA;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA
{
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
typedef enum _GET_FILEEX_INFO_LEVELS
{
    GetFileExInfoStandard,
    GetFileExMaxInfoLevel
} GET_FILEEX_INFO_LEVELS;

typedef struct _BY_HANDLE_FILE_INFORMATION
{
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

// ----- memory -----
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_PRIVATE 0x00020000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
#define HEAP_ZERO_MEMORY 0x00000008
#define HEAP_NO_SERIALIZE 0x00000001
#define LMEM_FIXED 0x0000
#define LMEM_ZEROINIT 0x0040
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)

typedef struct _MEMORY_BASIC_INFORMATION
{
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _MEMORYSTATUS
{
    DWORD dwLength;
    DWORD dwMemoryLoad;
    SIZE_T dwTotalPhys;
    SIZE_T dwAvailPhys;
    SIZE_T dwTotalPageFile;
    SIZE_T dwAvailPageFile;
    SIZE_T dwTotalVirtual;
    SIZE_T dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

typedef struct _MEMORYSTATUSEX
{
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORDLONG ullTotalPhys;
    DWORDLONG ullAvailPhys;
    DWORDLONG ullTotalPageFile;
    DWORDLONG ullAvailPageFile;
    DWORDLONG ullTotalVirtual;
    DWORDLONG ullAvailVirtual;
    DWORDLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _SYSTEM_INFO
{
    union
    {
        DWORD dwOemId;
        struct
        {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#define PROCESSOR_ARCHITECTURE_INTEL 0
#define PROCESSOR_INTEL_PENTIUM 586

typedef struct _OSVERSIONINFOA
{
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFOA, *POSVERSIONINFOA, *LPOSVERSIONINFOA, OSVERSIONINFO, *LPOSVERSIONINFO;

typedef struct _OSVERSIONINFOEXA
{
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXA, *POSVERSIONINFOEXA, *LPOSVERSIONINFOEXA, OSVERSIONINFOEX, *LPOSVERSIONINFOEX;
#define VER_PLATFORM_WIN32_NT 2
#define VER_NT_WORKSTATION 1

// ----- exceptions (the browser never delivers any) -----
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_ACCESS_VIOLATION ((DWORD)0xC0000005L)
#define EXCEPTION_BREAKPOINT ((DWORD)0x80000003L)
#define EXCEPTION_STACK_OVERFLOW ((DWORD)0xC00000FDL)
#define EXCEPTION_INT_DIVIDE_BY_ZERO ((DWORD)0xC0000094L)
#define EXCEPTION_NONCONTINUABLE 0x1
typedef struct _EXCEPTION_RECORD
{
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct _CONTEXT
{
    DWORD ContextFlags;
    DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    BYTE FloatSave[112];
    DWORD SegGs, SegFs, SegEs, SegDs;
    DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;
    DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    BYTE ExtendedRegisters[512];
} CONTEXT, *PCONTEXT, *LPCONTEXT;
#define CONTEXT_FULL 0x10007
#define CONTEXT_CONTROL 0x10001
typedef struct _EXCEPTION_POINTERS
{
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;
typedef LONG(WINAPI *PTOP_LEVEL_EXCEPTION_FILTER)(struct _EXCEPTION_POINTERS *ExceptionInfo);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000

// ----- window messages / input -----
typedef LRESULT(CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct tagMSG
{
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *PMSG, *LPMSG;

typedef struct tagWNDCLASSA
{
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASSA, *PWNDCLASSA, *LPWNDCLASSA, WNDCLASS, *LPWNDCLASS;

typedef struct tagWNDCLASSEXA
{
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXA, *PWNDCLASSEXA, *LPWNDCLASSEXA, WNDCLASSEX, *LPWNDCLASSEX;

typedef struct tagCREATESTRUCTA
{
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy;
    int cx;
    int y;
    int x;
    LONG style;
    LPCSTR lpszName;
    LPCSTR lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTA, *LPCREATESTRUCTA, CREATESTRUCT, *LPCREATESTRUCT;

typedef struct tagMONITORINFO
{
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;
#ifdef __cplusplus
typedef struct tagMONITORINFOEXA : public tagMONITORINFO
{
    CHAR szDevice[32];
} MONITORINFOEXA, *LPMONITORINFOEXA, MONITORINFOEX, *LPMONITORINFOEX;
#else
typedef struct tagMONITORINFOEXA
{
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
    CHAR szDevice[32];
} MONITORINFOEXA, *LPMONITORINFOEXA, MONITORINFOEX, *LPMONITORINFOEX;
#endif
typedef BOOL(CALLBACK *MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);
typedef BOOL(CALLBACK *WNDENUMPROC)(HWND, LPARAM);
#define MONITOR_DEFAULTTONULL 0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002
#define MONITORINFOF_PRIMARY 0x00000001

typedef struct _devicemodeA
{
    BYTE dmDeviceName[32];
    WORD dmSpecVersion;
    WORD dmDriverVersion;
    WORD dmSize;
    WORD dmDriverExtra;
    DWORD dmFields;
    union
    {
        struct
        {
            short dmOrientation;
            short dmPaperSize;
            short dmPaperLength;
            short dmPaperWidth;
            short dmScale;
            short dmCopies;
            short dmDefaultSource;
            short dmPrintQuality;
        };
        struct
        {
            POINTL dmPosition;
            DWORD dmDisplayOrientation;
            DWORD dmDisplayFixedOutput;
        };
    };
    short dmColor;
    short dmDuplex;
    short dmYResolution;
    short dmTTOption;
    short dmCollate;
    BYTE dmFormName[32];
    WORD dmLogPixels;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    union
    {
        DWORD dmDisplayFlags;
        DWORD dmNup;
    };
    DWORD dmDisplayFrequency;
    DWORD dmICMMethod;
    DWORD dmICMIntent;
    DWORD dmMediaType;
    DWORD dmDitherType;
    DWORD dmReserved1;
    DWORD dmReserved2;
    DWORD dmPanningWidth;
    DWORD dmPanningHeight;
} DEVMODEA, *PDEVMODEA, *LPDEVMODEA, DEVMODE, *LPDEVMODE;
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#define DM_BITSPERPEL 0x00040000L
#define DM_PELSWIDTH 0x00080000L
#define DM_PELSHEIGHT 0x00100000L
#define DM_DISPLAYFREQUENCY 0x00400000L
#define CDS_FULLSCREEN 0x00000004
#define CDS_TEST 0x00000002
#define DISP_CHANGE_SUCCESSFUL 0

typedef struct tagTRACKMOUSEEVENT
{
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
} TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;

typedef struct tagRAWINPUTDEVICE
{
    USHORT usUsagePage;
    USHORT usUsage;
    DWORD dwFlags;
    HWND hwndTarget;
} RAWINPUTDEVICE, *PRAWINPUTDEVICE, *LPRAWINPUTDEVICE;
typedef const RAWINPUTDEVICE *PCRAWINPUTDEVICE;
typedef struct tagRAWINPUTHEADER
{
    DWORD dwType;
    DWORD dwSize;
    HANDLE hDevice;
    WPARAM wParam;
} RAWINPUTHEADER, *PRAWINPUTHEADER;
typedef struct tagRAWMOUSE
{
    USHORT usFlags;
    union
    {
        ULONG ulButtons;
        struct
        {
            USHORT usButtonFlags;
            USHORT usButtonData;
        };
    };
    ULONG ulRawButtons;
    LONG lLastX;
    LONG lLastY;
    ULONG ulExtraInformation;
} RAWMOUSE, *PRAWMOUSE;
typedef struct tagRAWKEYBOARD
{
    USHORT MakeCode;
    USHORT Flags;
    USHORT Reserved;
    USHORT VKey;
    UINT Message;
    ULONG ExtraInformation;
} RAWKEYBOARD, *PRAWKEYBOARD;
typedef struct tagRAWINPUT
{
    RAWINPUTHEADER header;
    union
    {
        RAWMOUSE mouse;
        RAWKEYBOARD keyboard;
    } data;
} RAWINPUT, *PRAWINPUT, *LPRAWINPUT;
#define RIM_TYPEMOUSE 0
#define RIM_TYPEKEYBOARD 1
#define RID_INPUT 0x10000003
#define RIDEV_REMOVE 0x00000001
#define RIDEV_NOLEGACY 0x00000030
#define RIDEV_INPUTSINK 0x00000100
#define MOUSE_MOVE_RELATIVE 0
#define MOUSE_MOVE_ABSOLUTE 1
#define RI_MOUSE_LEFT_BUTTON_DOWN 0x0001
#define RI_MOUSE_LEFT_BUTTON_UP 0x0002
#define RI_MOUSE_RIGHT_BUTTON_DOWN 0x0004
#define RI_MOUSE_RIGHT_BUTTON_UP 0x0008
#define RI_MOUSE_MIDDLE_BUTTON_DOWN 0x0010
#define RI_MOUSE_MIDDLE_BUTTON_UP 0x0020
#define RI_MOUSE_BUTTON_4_DOWN 0x0040
#define RI_MOUSE_BUTTON_4_UP 0x0080
#define RI_MOUSE_BUTTON_5_DOWN 0x0100
#define RI_MOUSE_BUTTON_5_UP 0x0200
#define RI_MOUSE_WHEEL 0x0400

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SYSCOLORCHANGE 0x0015
#define WM_ENDSESSION 0x0016
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_DISPLAYCHANGE 0x007E
#define WM_GETICON 0x007F
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_INPUT 0x00FF
#define WM_KEYFIRST 0x0100
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_KEYLAST 0x0109
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION 0x010E
#define WM_IME_COMPOSITION 0x010F
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_MOUSEHWHEEL 0x020E
#define WM_MOUSELAST 0x020E
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_IME_SETCONTEXT 0x0281
#define WM_IME_NOTIFY 0x0282
#define WM_MOUSEHOVER 0x02A1
#define WM_MOUSELEAVE 0x02A3
#define WM_DEVICECHANGE 0x0219
#define WM_HOTKEY 0x0312
#define WM_APP 0x8000
#define WM_USER 0x0400
#define WHEEL_DELTA 120
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define MK_XBUTTON1 0x0020
#define MK_XBUTTON2 0x0040
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define SC_SCREENSAVE 0xF140
#define SC_MONITORPOWER 0xF170
#define SC_KEYMENU 0xF100
#define SC_CLOSE 0xF060
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define PM_NOYIELD 0x0002

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_EXECUTE 0x2B
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_SLEEP 0x5F
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_8 0xDF
#define VK_OEM_102 0xE2
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2
#define MAPVK_VSC_TO_VK_EX 3

#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_CLIENTEDGE 0x00000200L
#define WS_EX_LEFT 0x00000000L
#define CW_USEDEFAULT ((int)0x80000000)
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define GWLP_USERDATA (-21)
#define GWLP_WNDPROC (-4)
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)
#define HWND_DESKTOP ((HWND)0)
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOOWNERZORDER 0x0200
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CYCAPTION 4
#define SM_CMONITORS 80
#define SM_REMOTESESSION 0x1000
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)
#define COLOR_WINDOW 5
#define BLACK_BRUSH 4
#define WHITE_BRUSH 0
#define DEFAULT_GUI_FONT 17
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define VREFRESH 116
#define SPI_GETSCREENSAVEACTIVE 0x0010
#define SPI_SETSCREENSAVEACTIVE 0x0011
#define SPI_GETWORKAREA 0x0030
#define SPI_GETMOUSE 0x0003
#define SPI_SETMOUSE 0x0004
#define SPI_GETSTICKYKEYS 0x003A
#define SPI_SETSTICKYKEYS 0x003B
#define SPI_GETTOGGLEKEYS 0x0034
#define SPI_SETTOGGLEKEYS 0x0035
#define SPI_GETFILTERKEYS 0x0032
#define SPI_SETFILTERKEYS 0x0033
#define SPIF_SENDCHANGE 0x0002
#define CF_TEXT 1
#define TME_LEAVE 0x00000002
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define LR_LOADFROMFILE 0x00000010
#define LR_DEFAULTSIZE 0x00000040
#define LR_DEFAULTCOLOR 0x00000000
#define FLASHW_ALL 0x00000003
#define FLASHW_TIMERNOFG 0x0000000C

#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONERROR 0x00000010L
#define MB_ICONSTOP 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONWARNING 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONINFORMATION 0x00000040L
#define MB_DEFBUTTON1 0x00000000L
#define MB_DEFBUTTON2 0x00000100L
#define MB_APPLMODAL 0x00000000L
#define MB_SYSTEMMODAL 0x00001000L
#define MB_TASKMODAL 0x00002000L
#define MB_SETFOREGROUND 0x00010000L
#define MB_TOPMOST 0x00040000L
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define LANG_NEUTRAL 0x00
#define LANG_ENGLISH 0x09
#define SUBLANG_DEFAULT 0x01
#define SUBLANG_ENGLISH_US 0x01
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define PRIMARYLANGID(lgid) ((WORD)(lgid) & 0x3ff)
#define SUBLANGID(lgid) ((WORD)(lgid) >> 10)
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define LOCALE_SENGLANGUAGE 0x00001001
#define LOCALE_SABBREVLANGNAME 0x00000003
#define LOCALE_SISO639LANGNAME 0x00000059
#define LOCALE_SISO3166CTRYNAME 0x0000005A
#define LOCALE_IDEFAULTANSICODEPAGE 0x00001004
#define CP_ACP 0
#define CP_UTF8 65001

// registry (always empty)
#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)((LONG)0x80000000))
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#define HKEY_USERS ((HKEY)(ULONG_PTR)((LONG)0x80000003))
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE 0x0002
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F
#define KEY_WOW64_64KEY 0x0100
#define KEY_WOW64_32KEY 0x0200
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_OPTION_NON_VOLATILE 0x00000000L
#define REG_CREATED_NEW_KEY 0x00000001L

// ----- security tokens (large pages; never granted here) -----
typedef struct _LUID_AND_ATTRIBUTES
{
    LUID Luid;
    DWORD Attributes;
} LUID_AND_ATTRIBUTES, *PLUID_AND_ATTRIBUTES;
typedef struct _TOKEN_PRIVILEGES
{
    DWORD PrivilegeCount;
    LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;
#define TOKEN_ADJUST_PRIVILEGES 0x0020
#define TOKEN_QUERY 0x0008
#define SE_PRIVILEGE_ENABLED 0x00000002L
#define SE_LOCK_MEMORY_NAME "SeLockMemoryPrivilege"

// ----- CPU sets (Windows 10; reported as unavailable) -----
typedef enum _CPU_SET_INFORMATION_TYPE
{
    CpuSetInformation
} CPU_SET_INFORMATION_TYPE;
typedef struct _SYSTEM_CPU_SET_INFORMATION
{
    DWORD Size;
    CPU_SET_INFORMATION_TYPE Type;
    union
    {
        struct
        {
            DWORD Id;
            WORD Group;
            BYTE LogicalProcessorIndex;
            BYTE CoreIndex;
            BYTE LastLevelCacheIndex;
            BYTE NumaNodeIndex;
            BYTE EfficiencyClass;
            BYTE AllFlags;
            DWORD Reserved;
            DWORD64 AllocationTag;
        } CpuSet;
    };
} SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;

// ----- GDI text (only named by debug font code) -----
typedef struct tagTEXTMETRICA
{
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    BYTE tmFirstChar;
    BYTE tmLastChar;
    BYTE tmDefaultChar;
    BYTE tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICA, *PTEXTMETRICA, *LPTEXTMETRICA, TEXTMETRIC;
typedef struct tagTEXTMETRICW
{
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    WCHAR tmFirstChar;
    WCHAR tmLastChar;
    WCHAR tmDefaultChar;
    WCHAR tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICW, *PTEXTMETRICW, *LPTEXTMETRICW;
typedef struct _POINTFLOAT
{
    FLOAT x;
    FLOAT y;
} POINTFLOAT, *PPOINTFLOAT;
typedef struct _GLYPHMETRICSFLOAT
{
    FLOAT gmfBlackBoxX;
    FLOAT gmfBlackBoxY;
    POINTFLOAT gmfptGlyphOrigin;
    FLOAT gmfCellIncX;
    FLOAT gmfCellIncY;
} GLYPHMETRICSFLOAT, *PGLYPHMETRICSFLOAT, *LPGLYPHMETRICSFLOAT;

// ----- wave formats (mmreg.h; packed as audiodefs.h has them) -----
#ifndef _WAVEFORMATEX_
#define _WAVEFORMATEX_
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX
{
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *NPWAVEFORMATEX, *LPWAVEFORMATEX;
#pragma pack(pop)
#endif
#ifndef _LPCWAVEFORMATEX_DEFINED
#define _LPCWAVEFORMATEX_DEFINED
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;
#endif

// ----- multimedia timers (winmm) -----
typedef UINT MMRESULT;
#define TIMERR_NOERROR 0
#define TIME_ONESHOT 0x0000
#define TIME_PERIODIC 0x0001
typedef struct timecaps_tag
{
    UINT wPeriodMin;
    UINT wPeriodMax;
} TIMECAPS, *LPTIMECAPS;

// ----- interlocked (MSVC's are full barriers; any 32/64-bit integer counter is accepted, as MSVC's intrinsics do) -----
#ifdef __cplusplus
#define InterlockedIncrement bo1_InterlockedIncrement
#define InterlockedDecrement bo1_InterlockedDecrement
#define InterlockedExchange bo1_InterlockedExchange
#define InterlockedExchangeAdd bo1_InterlockedExchangeAdd
#define InterlockedOr bo1_InterlockedOr
#define InterlockedAnd bo1_InterlockedAnd
#define InterlockedXor bo1_InterlockedXor
#define InterlockedCompareExchange bo1_InterlockedCompareExchange
#define InterlockedCompareExchange64 bo1_InterlockedCompareExchange
#define InterlockedIncrement64 bo1_InterlockedIncrement
#define InterlockedDecrement64 bo1_InterlockedDecrement
#define InterlockedExchangeAdd64 bo1_InterlockedExchangeAdd
#define InterlockedExchange64 bo1_InterlockedExchange
#define InterlockedCompareExchangePointer bo1_InterlockedCompareExchangePointer
#define InterlockedExchangePointer bo1_InterlockedExchangePointer
template <class T, class V> inline T InterlockedAdd(volatile T *p, V v) { return __atomic_add_fetch(p, (T)v, __ATOMIC_SEQ_CST); }
#endif

EXTERN_C const GUID IID_IUnknown;
EXTERN_C const GUID IID_XAudio2;
EXTERN_C const GUID IID_XAudio2_Debug;

BO1_EXTERN_C_BEGIN

// ----- kernel32 -----
DWORD WINAPI GetLastError(void);
void WINAPI SetLastError(DWORD dwErrCode);
void WINAPI Sleep(DWORD dwMilliseconds);
DWORD WINAPI SleepEx(DWORD dwMilliseconds, BOOL bAlertable);
BOOL WINAPI SwitchToThread(void);
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
void WINAPI ExitThread(DWORD dwExitCode);
BOOL WINAPI TerminateThread(HANDLE hThread, DWORD dwExitCode);
BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
HANDLE WINAPI GetCurrentThread(void);
DWORD WINAPI GetCurrentThreadId(void);
DWORD WINAPI GetThreadId(HANDLE Thread);
HANDLE WINAPI GetCurrentProcess(void);
DWORD WINAPI GetCurrentProcessId(void);
DWORD WINAPI ResumeThread(HANDLE hThread);
DWORD WINAPI SuspendThread(HANDLE hThread);
BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
int WINAPI GetThreadPriority(HANDLE hThread);
BOOL WINAPI SetThreadPriorityBoost(HANDLE hThread, BOOL bDisablePriorityBoost);
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
DWORD WINAPI SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor);
BOOL WINAPI GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask);
BOOL WINAPI SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask);
BOOL WINAPI SetPriorityClass(HANDLE hProcess, DWORD dwPriorityClass);
DWORD WINAPI GetPriorityClass(HANDLE hProcess);
BOOL WINAPI GetThreadTimes(HANDLE hThread, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
BOOL WINAPI GetProcessTimes(HANDLE hProcess, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT lpContext);
DWORD WINAPI QueueUserAPC(void(WINAPI *pfnAPC)(ULONG_PTR), HANDLE hThread, ULONG_PTR dwData);
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
HANDLE WINAPI OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
BOOL WINAPI SetEvent(HANDLE hEvent);
BOOL WINAPI ResetEvent(HANDLE hEvent);
BOOL WINAPI PulseEvent(HANDLE hEvent);
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HANDLE WINAPI OpenMutexA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
BOOL WINAPI ReleaseMutex(HANDLE hMutex);
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount, LPCSTR lpName);
BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount);
DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable);
DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
DWORD WINAPI WaitForMultipleObjectsEx(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable);
DWORD WINAPI SignalObjectAndWait(HANDLE hObjectToSignal, HANDLE hObjectToWaitOn, DWORD dwMilliseconds, BOOL bAlertable);
BOOL WINAPI CloseHandle(HANDLE hObject);
BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle,
    DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);
void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount);
BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount, DWORD Flags);
DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount);
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI InitializeSRWLock(PSRWLOCK SRWLock);
void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock);
void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
void WINAPI AcquireSRWLockShared(PSRWLOCK SRWLock);
void WINAPI ReleaseSRWLockShared(PSRWLOCK SRWLock);
DWORD WINAPI TlsAlloc(void);
LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex);
BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue);
BOOL WINAPI TlsFree(DWORD dwTlsIndex);

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency);
DWORD WINAPI GetTickCount(void);
ULONGLONG WINAPI GetTickCount64(void);
void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
BOOL WINAPI FileTimeToSystemTime(const FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime);
BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *lpFileTime, LPFILETIME lpLocalFileTime);
BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *lpLocalFileTime, LPFILETIME lpFileTime);
LONG WINAPI CompareFileTime(const FILETIME *lpFileTime1, const FILETIME *lpFileTime2);
DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation);
BOOL WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *lpTimeZoneInformation, const SYSTEMTIME *lpUniversalTime, LPSYSTEMTIME lpLocalTime);

LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength);
BOOL WINAPI VirtualLock(LPVOID lpAddress, SIZE_T dwSize);
BOOL WINAPI VirtualUnlock(LPVOID lpAddress, SIZE_T dwSize);
void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer);
BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer);
void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo);
HANDLE WINAPI GetProcessHeap(void);
HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize);
BOOL WINAPI HeapDestroy(HANDLE hHeap);
LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
LPVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);
BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
BOOL WINAPI HeapValidate(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes);
HLOCAL WINAPI LocalFree(HLOCAL hMem);
HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
LPVOID WINAPI GlobalLock(HGLOBAL hMem);
BOOL WINAPI GlobalUnlock(HGLOBAL hMem);
HGLOBAL WINAPI GlobalFree(HGLOBAL hMem);
SIZE_T WINAPI GlobalSize(HGLOBAL hMem);

HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL WINAPI ReadFileEx(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPOVERLAPPED lpOverlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
BOOL WINAPI CancelIo(HANDLE hFile);
BOOL WINAPI CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod);
DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL WINAPI GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
DWORD WINAPI GetFileType(HANDLE hFile);
BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME *lpCreationTime, const FILETIME *lpLastAccessTime, const FILETIME *lpLastWriteTime);
BOOL WINAPI GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
BOOL WINAPI FlushFileBuffers(HANDLE hFile);
BOOL WINAPI SetEndOfFile(HANDLE hFile);
BOOL WINAPI DeleteFileA(LPCSTR lpFileName);
BOOL WINAPI MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
BOOL WINAPI MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags);
BOOL WINAPI CopyFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists);
BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL WINAPI RemoveDirectoryA(LPCSTR lpPathName);
DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName);
BOOL WINAPI GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
BOOL WINAPI SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes);
HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL WINAPI FindClose(HANDLE hFindFile);
DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
BOOL WINAPI SetCurrentDirectoryA(LPCSTR lpPathName);
DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
BOOL WINAPI GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule);
DWORD WINAPI GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR *lpFilePart);
DWORD WINAPI GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer);
UINT WINAPI GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName);
BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes,
    PULARGE_INTEGER lpTotalNumberOfFreeBytes);
BOOL WINAPI GetDiskFreeSpaceA(LPCSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector, LPDWORD lpNumberOfFreeClusters,
    LPDWORD lpTotalNumberOfClusters);
UINT WINAPI GetDriveTypeA(LPCSTR lpRootPathName);
DWORD WINAPI GetLogicalDrives(void);
BOOL WINAPI GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize, LPDWORD lpVolumeSerialNumber,
    LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);
HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
BOOL WINAPI AllocConsole(void);
BOOL WINAPI FreeConsole(void);
BOOL WINAPI SetConsoleTitleA(LPCSTR lpConsoleTitle);

HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
HMODULE WINAPI LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
typedef int(WINAPI *FARPROC)();
FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL WINAPI FreeLibrary(HMODULE hLibModule);
void WINAPI OutputDebugStringA(LPCSTR lpOutputString);
BOOL WINAPI IsDebuggerPresent(void);
void WINAPI DebugBreak(void);
void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments, const ULONG_PTR *lpArguments);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo);
UINT WINAPI SetErrorMode(UINT uMode);
LPSTR WINAPI GetCommandLineA(void);
DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize);
DECLSPEC_NORETURN void WINAPI ExitProcess(UINT uExitCode);
BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
HANDLE WINAPI OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
BOOL WINAPI GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize);
BOOL WINAPI GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer);
BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation);
DWORD WINAPI GetVersion(void);
DWORD WINAPI FormatMessageA(DWORD dwFlags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId, LPSTR lpBuffer, DWORD nSize, va_list *Arguments);
LCID WINAPI GetUserDefaultLCID(void);
LCID WINAPI GetSystemDefaultLCID(void);
LCID WINAPI GetThreadLocale(void);
LANGID WINAPI GetUserDefaultLangID(void);
LANGID WINAPI GetSystemDefaultLangID(void);
LANGID WINAPI GetUserDefaultUILanguage(void);
LANGID WINAPI GetSystemDefaultUILanguage(void);
int WINAPI GetLocaleInfoA(LCID Locale, DWORD LCType, LPSTR lpLCData, int cchData);
UINT WINAPI GetACP(void);
int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte,
    LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar);
int WINAPI MulDiv(int nNumber, int nNumerator, int nDenominator);
int WINAPIV wsprintfA(LPSTR, LPCSTR, ...);
int WINAPI wvsprintfA(LPSTR, LPCSTR, va_list arglist);
LPSTR WINAPI lstrcpyA(LPSTR lpString1, LPCSTR lpString2);
LPSTR WINAPI lstrcpynA(LPSTR lpString1, LPCSTR lpString2, int iMaxLength);
LPSTR WINAPI lstrcatA(LPSTR lpString1, LPCSTR lpString2);
int WINAPI lstrlenA(LPCSTR lpString);
int WINAPI lstrcmpA(LPCSTR lpString1, LPCSTR lpString2);
int WINAPI lstrcmpiA(LPCSTR lpString1, LPCSTR lpString2);
BOOL WINAPI IsBadReadPtr(const void *lp, UINT_PTR ucb);
BOOL WINAPI IsBadWritePtr(LPVOID lp, UINT_PTR ucb);
BOOL WINAPI IsProcessorFeaturePresent(DWORD ProcessorFeature);
BOOL WINAPI FlushInstructionCache(HANDLE hProcess, LPCVOID lpBaseAddress, SIZE_T dwSize);
BOOL WINAPI ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesRead);
BOOL WINAPI WriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesWritten);
HRSRC WINAPI FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType);
HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo);
LPVOID WINAPI LockResource(HGLOBAL hResData);
DWORD WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo);

// ----- user32 / gdi32 (inert: the game draws into a canvas) -----
int WINAPI MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
HWND WINAPI GetDesktopWindow(void);
HWND WINAPI GetForegroundWindow(void);
BOOL WINAPI SetForegroundWindow(HWND hWnd);
HWND WINAPI GetActiveWindow(void);
HWND WINAPI SetActiveWindow(HWND hWnd);
HWND WINAPI GetFocus(void);
HWND WINAPI SetFocus(HWND hWnd);
BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
BOOL WINAPI UpdateWindow(HWND hWnd);
BOOL WINAPI IsWindow(HWND hWnd);
BOOL WINAPI IsWindowVisible(HWND hWnd);
BOOL WINAPI IsIconic(HWND hWnd);
BOOL WINAPI IsZoomed(HWND hWnd);
BOOL WINAPI DestroyWindow(HWND hWnd);
HWND WINAPI CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
    HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
#define CreateWindowA(c, w, s, x, y, ww, hh, p, m, i, l) CreateWindowExA(0, c, w, s, x, y, ww, hh, p, m, i, l)
LRESULT WINAPI DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL WINAPI TranslateMessage(const MSG *lpMsg);
LRESULT WINAPI DispatchMessageA(const MSG *lpMsg);
BOOL WINAPI PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL WINAPI PostThreadMessageA(DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam);
void WINAPI PostQuitMessage(int nExitCode);
DWORD WINAPI GetMessageTime(void);
ATOM WINAPI RegisterClassA(const WNDCLASSA *lpWndClass);
ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *lpwcx);
BOOL WINAPI UnregisterClassA(LPCSTR lpClassName, HINSTANCE hInstance);
HWND WINAPI FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName);
BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
BOOL WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL WINAPI GetClientRect(HWND hWnd, LPRECT lpRect);
BOOL WINAPI AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu);
BOOL WINAPI AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
BOOL WINAPI ClientToScreen(HWND hWnd, LPPOINT lpPoint);
BOOL WINAPI ScreenToClient(HWND hWnd, LPPOINT lpPoint);
BOOL WINAPI SetCursorPos(int X, int Y);
BOOL WINAPI GetCursorPos(LPPOINT lpPoint);
int WINAPI ShowCursor(BOOL bShow);
HCURSOR WINAPI SetCursor(HCURSOR hCursor);
HCURSOR WINAPI LoadCursorA(HINSTANCE hInstance, LPCSTR lpCursorName);
HICON WINAPI LoadIconA(HINSTANCE hInstance, LPCSTR lpIconName);
HANDLE WINAPI LoadImageA(HINSTANCE hInst, LPCSTR name, UINT type, int cx, int cy, UINT fuLoad);
BOOL WINAPI ClipCursor(const RECT *lpRect);
HWND WINAPI SetCapture(HWND hWnd);
BOOL WINAPI ReleaseCapture(void);
HWND WINAPI GetCapture(void);
BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack);
HDC WINAPI GetDC(HWND hWnd);
int WINAPI ReleaseDC(HWND hWnd, HDC hDC);
int WINAPI GetDeviceCaps(HDC hdc, int index);
BOOL WINAPI SetWindowTextA(HWND hWnd, LPCSTR lpString);
int WINAPI GetWindowTextA(HWND hWnd, LPSTR lpString, int nMaxCount);
LONG WINAPI GetWindowLongA(HWND hWnd, int nIndex);
LONG WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
#define GetWindowLongPtrA GetWindowLongA
#define SetWindowLongPtrA SetWindowLongA
BOOL WINAPI EnumDisplaySettingsA(LPCSTR lpszDeviceName, DWORD iModeNum, DEVMODEA *lpDevMode);
LONG WINAPI ChangeDisplaySettingsA(DEVMODEA *lpDevMode, DWORD dwFlags);
HMONITOR WINAPI MonitorFromWindow(HWND hwnd, DWORD dwFlags);
HMONITOR WINAPI MonitorFromPoint(POINT pt, DWORD dwFlags);
HMONITOR WINAPI MonitorFromRect(LPCRECT lprc, DWORD dwFlags);
BOOL WINAPI GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi);
BOOL WINAPI EnumDisplayMonitors(HDC hdc, LPCRECT lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData);
int WINAPI GetSystemMetrics(int nIndex);
BOOL WINAPI SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
BOOL WINAPI OpenClipboard(HWND hWndNewOwner);
HANDLE WINAPI GetClipboardData(UINT uFormat);
HANDLE WINAPI SetClipboardData(UINT uFormat, HANDLE hMem);
BOOL WINAPI EmptyClipboard(void);
BOOL WINAPI CloseClipboard(void);
SHORT WINAPI GetKeyState(int nVirtKey);
SHORT WINAPI GetAsyncKeyState(int vKey);
BOOL WINAPI GetKeyboardState(PBYTE lpKeyState);
UINT WINAPI MapVirtualKeyA(UINT uCode, UINT uMapType);
UINT WINAPI MapVirtualKeyExA(UINT uCode, UINT uMapType, HKL dwhkl);
HKL WINAPI GetKeyboardLayout(DWORD idThread);
int WINAPI GetKeyboardLayoutList(int nBuff, HKL *lpList);
int WINAPI ToAscii(UINT uVirtKey, UINT uScanCode, const BYTE *lpKeyState, LPWORD lpChar, UINT uFlags);
int WINAPI ToAsciiEx(UINT uVirtKey, UINT uScanCode, const BYTE *lpKeyState, LPWORD lpChar, UINT uFlags, HKL dwhkl);
BOOL WINAPI RegisterRawInputDevices(PCRAWINPUTDEVICE pRawInputDevices, UINT uiNumDevices, UINT cbSize);
UINT WINAPI GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
BOOL WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase);
BOOL WINAPI ValidateRect(HWND hWnd, const RECT *lpRect);
BOOL WINAPI FlashWindow(HWND hWnd, BOOL bInvert);
HGDIOBJ WINAPI GetStockObject(int i);
HBRUSH WINAPI CreateSolidBrush(COLORREF color);
HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h);
BOOL WINAPI DeleteObject(HGDIOBJ ho);
BOOL WINAPI DeleteDC(HDC hdc);
HDC WINAPI CreateCompatibleDC(HDC hdc);
BOOL WINAPI SetDeviceGammaRamp(HDC hdc, LPVOID lpRamp);
BOOL WINAPI GetDeviceGammaRamp(HDC hdc, LPVOID lpRamp);
UINT_PTR WINAPI SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, void *lpTimerFunc);
BOOL WINAPI KillTimer(HWND hWnd, UINT_PTR uIDEvent);
HINSTANCE WINAPI ShellExecuteA(HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR lpParameters, LPCSTR lpDirectory, INT nShowCmd);
typedef DWORD EXECUTION_STATE;
#define ES_SYSTEM_REQUIRED 0x00000001
#define ES_DISPLAY_REQUIRED 0x00000002
#define ES_CONTINUOUS 0x80000000
EXECUTION_STATE WINAPI SetThreadExecutionState(EXECUTION_STATE esFlags);
UINT WINAPI RegisterWindowMessageA(LPCSTR lpString);
#define RegisterWindowMessage RegisterWindowMessageA
typedef LRESULT(CALLBACK *HOOKPROC)(int code, WPARAM wParam, LPARAM lParam);
#define WH_KEYBOARD_LL 13
#define HC_ACTION 0
#define LLKHF_ALTDOWN 0x00000020
typedef struct tagKBDLLHOOKSTRUCT
{
    DWORD vkCode;
    DWORD scanCode;
    DWORD flags;
    DWORD time;
    ULONG_PTR dwExtraInfo;
} KBDLLHOOKSTRUCT, *LPKBDLLHOOKSTRUCT, *PKBDLLHOOKSTRUCT;
HHOOK WINAPI SetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
#define SetWindowsHookEx SetWindowsHookExA
BOOL WINAPI UnhookWindowsHookEx(HHOOK hhk);
LRESULT WINAPI CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam);
DWORD WINAPI MsgWaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles, BOOL fWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask);
#define QS_ALLINPUT 0x04FF

HANDLE WINAPI OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId);
BOOL WINAPI QueryThreadCycleTime(HANDLE ThreadHandle, PULONG64 CycleTime);
BOOL WINAPI QueryProcessCycleTime(HANDLE ProcessHandle, PULONG64 CycleTime);
BOOL WINAPI GetSystemTimes(LPFILETIME lpIdleTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
BOOL WINAPI GetProcessHandleCount(HANDLE hProcess, PDWORD pdwHandleCount);
DWORD WINAPI GetCurrentProcessorNumber(void);
BOOL WINAPI IsProcessDPIAware(void);
BOOL WINAPI SetProcessDPIAware(void);
BOOL WINAPI EqualRect(const RECT *lprc1, const RECT *lprc2);
BOOL WINAPI EnumThreadWindows(DWORD dwThreadId, WNDENUMPROC lpfn, LPARAM lParam);
BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION Information, ULONG BufferLength, PULONG ReturnedLength, HANDLE Process, ULONG Flags);
BOOL WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
BOOL WINAPI LookupPrivilegeValueA(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid);
BOOL WINAPI AdjustTokenPrivileges(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState, DWORD BufferLength,
    PTOKEN_PRIVILEGES PreviousState, PDWORD ReturnLength);
SIZE_T WINAPI GetLargePageMinimum(void);
BOOL WINAPI EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam);
DWORD WINAPI GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId);

// ----- advapi32 (no registry in the browser) -----
LONG WINAPI RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LONG WINAPI RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult);
LONG WINAPI RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions, REGSAM samDesired,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition);
LONG WINAPI RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LONG WINAPI RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE *lpData, DWORD cbData);
LONG WINAPI RegDeleteValueA(HKEY hKey, LPCSTR lpValueName);
LONG WINAPI RegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey);
LONG WINAPI RegCloseKey(HKEY hKey);
LONG WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved, LPSTR lpClass, LPDWORD lpcchClass,
    PFILETIME lpftLastWriteTime);

// ----- winmm -----
DWORD WINAPI timeGetTime(void);
MMRESULT WINAPI timeBeginPeriod(UINT uPeriod);
MMRESULT WINAPI timeEndPeriod(UINT uPeriod);
MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc);

BO1_EXTERN_C_END

// A/W-less names (this is an ANSI build)
#define CreateEvent CreateEventA
#define OpenEvent OpenEventA
#define CreateMutex CreateMutexA
#define OpenMutex OpenMutexA
#define CreateSemaphore CreateSemaphoreA
#define CreateFile CreateFileA
#define DeleteFile DeleteFileA
#define MoveFile MoveFileA
#define MoveFileEx MoveFileExA
#define CopyFile CopyFileA
#define CreateDirectory CreateDirectoryA
#define RemoveDirectory RemoveDirectoryA
#define GetFileAttributes GetFileAttributesA
#define GetFileAttributesEx GetFileAttributesExA
#define SetFileAttributes SetFileAttributesA
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
#define GetModuleFileName GetModuleFileNameA
#define GetModuleHandle GetModuleHandleA
#define GetFullPathName GetFullPathNameA
#define GetTempPath GetTempPathA
#define GetDiskFreeSpaceEx GetDiskFreeSpaceExA
#define GetDriveType GetDriveTypeA
#define LoadLibrary LoadLibraryA
#define OutputDebugString OutputDebugStringA
#define GetCommandLine GetCommandLineA
#define GetEnvironmentVariable GetEnvironmentVariableA
#define SetEnvironmentVariable SetEnvironmentVariableA
#define GetComputerName GetComputerNameA
#define GetUserName GetUserNameA
#define GetVersionEx GetVersionExA
#define FormatMessage FormatMessageA
#define GetLocaleInfo GetLocaleInfoA
#define MessageBox MessageBoxA
#define CreateWindowEx CreateWindowExA
#define CreateWindow CreateWindowA
#define DefWindowProc DefWindowProcA
#define CallWindowProc CallWindowProcA
#define PeekMessage PeekMessageA
#define GetMessage GetMessageA
#define DispatchMessage DispatchMessageA
#define PostMessage PostMessageA
#define SendMessage SendMessageA
#define RegisterClass RegisterClassA
#define RegisterClassEx RegisterClassExA
#define UnregisterClass UnregisterClassA
#define FindWindow FindWindowA
#define LoadCursor LoadCursorA
#define LoadIcon LoadIconA
#define LoadImage LoadImageA
#define SetWindowText SetWindowTextA
#define GetWindowText GetWindowTextA
#define GetWindowLong GetWindowLongA
#define SetWindowLong SetWindowLongA
#define GetWindowLongPtr GetWindowLongA
#define SetWindowLongPtr SetWindowLongA
#define EnumDisplaySettings EnumDisplaySettingsA
#define ChangeDisplaySettings ChangeDisplaySettingsA
#define GetMonitorInfo GetMonitorInfoA
#define SystemParametersInfo SystemParametersInfoA
#define MapVirtualKey MapVirtualKeyA
#define MapVirtualKeyEx MapVirtualKeyExA
#define ShellExecute ShellExecuteA
#define RegOpenKeyEx RegOpenKeyExA
#define RegOpenKey RegOpenKeyA
#define RegCreateKeyEx RegCreateKeyExA
#define RegQueryValueEx RegQueryValueExA
#define RegSetValueEx RegSetValueExA
#define RegDeleteValue RegDeleteValueA
#define RegDeleteKey RegDeleteKeyA
#define RegEnumKeyEx RegEnumKeyExA
#define LookupPrivilegeValue LookupPrivilegeValueA
#define wsprintf wsprintfA
#define wvsprintf wvsprintfA
#define lstrcpy lstrcpyA
#define lstrcpyn lstrcpynA
#define lstrcat lstrcatA
#define lstrlen lstrlenA
#define lstrcmp lstrcmpA
#define lstrcmpi lstrcmpiA
#define WIN32_FIND_DATA WIN32_FIND_DATAA
#define MONITORINFOEX MONITORINFOEXA

#endif // BO1_WEB_WINDOWS_H

// MSVC's full windows.h also brings these in
#include "wincrypt.h"
#include "winsock2.h"
