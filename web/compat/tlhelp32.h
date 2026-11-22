#pragma once
#include "windows.h"
#define TH32CS_SNAPPROCESS 0x00000002
#define TH32CS_SNAPTHREAD 0x00000004
#define TH32CS_SNAPMODULE 0x00000008
typedef struct tagTHREADENTRY32
{
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32, *PTHREADENTRY32, *LPTHREADENTRY32;
typedef struct tagPROCESSENTRY32
{
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    CHAR szExeFile[MAX_PATH];
} PROCESSENTRY32, *PPROCESSENTRY32, *LPPROCESSENTRY32;
BO1_EXTERN_C HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
BO1_EXTERN_C BOOL WINAPI Thread32First(HANDLE hSnapshot, LPTHREADENTRY32 lpte);
BO1_EXTERN_C BOOL WINAPI Thread32Next(HANDLE hSnapshot, LPTHREADENTRY32 lpte);
BO1_EXTERN_C BOOL WINAPI Process32First(HANDLE hSnapshot, LPPROCESSENTRY32 lppe);
BO1_EXTERN_C BOOL WINAPI Process32Next(HANDLE hSnapshot, LPPROCESSENTRY32 lppe);

