#pragma once
#include "windows.h"
#define SYMOPT_UNDNAME 0x00000002
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMOPT_LOAD_LINES 0x00000010
#define MAX_SYM_NAME 2000
typedef struct _SYMBOL_INFO
{
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    CHAR Name[1];
} SYMBOL_INFO, *PSYMBOL_INFO;
typedef char *PCHAR;
typedef struct _IMAGEHLP_LINE64
{
    DWORD SizeOfStruct;
    PVOID Key;
    DWORD LineNumber;
    PCHAR FileName;
    DWORD64 Address;
} IMAGEHLP_LINE64, *PIMAGEHLP_LINE64;

BO1_EXTERN_C BOOL WINAPI SymInitialize(HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess);
BO1_EXTERN_C BOOL WINAPI SymCleanup(HANDLE hProcess);
BO1_EXTERN_C DWORD WINAPI SymSetOptions(DWORD SymOptions);
BO1_EXTERN_C DWORD WINAPI SymGetOptions(void);
BO1_EXTERN_C BOOL WINAPI SymFromAddr(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PSYMBOL_INFO Symbol);
BO1_EXTERN_C BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD pdwDisplacement, PIMAGEHLP_LINE64 Line64);
