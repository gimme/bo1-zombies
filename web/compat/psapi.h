#pragma once
#include "windows.h"
typedef struct _PROCESS_MEMORY_COUNTERS
{
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX
{
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;
typedef struct _MODULEINFO
{
    LPVOID lpBaseOfDll;
    DWORD SizeOfImage;
    LPVOID EntryPoint;
} MODULEINFO, *LPMODULEINFO;
BO1_EXTERN_C BOOL WINAPI GetProcessMemoryInfo(HANDLE Process, PPROCESS_MEMORY_COUNTERS ppsmemCounters, DWORD cb);
BO1_EXTERN_C BOOL WINAPI GetModuleInformation(HANDLE hProcess, HMODULE hModule, LPMODULEINFO lpmodinfo, DWORD cb);
BO1_EXTERN_C BOOL WINAPI EnumProcessModules(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded);
BO1_EXTERN_C DWORD WINAPI GetModuleBaseNameA(HANDLE hProcess, HMODULE hModule, LPSTR lpBaseName, DWORD nSize);
BO1_EXTERN_C BOOL WINAPI EnumProcesses(DWORD *lpidProcess, DWORD cb, LPDWORD lpcbNeeded);
#define GetModuleBaseName GetModuleBaseNameA
#define K32GetProcessMemoryInfo GetProcessMemoryInfo
