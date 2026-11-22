#pragma once
#include "windows.h"
#define CSIDL_PERSONAL 0x0005
#define CSIDL_APPDATA 0x001a
#define CSIDL_LOCAL_APPDATA 0x001c
#define CSIDL_COMMON_APPDATA 0x0023
#define CSIDL_MYDOCUMENTS CSIDL_PERSONAL
#define CSIDL_FLAG_CREATE 0x8000
#define SHGFP_TYPE_CURRENT 0
BO1_EXTERN_C HRESULT WINAPI SHGetFolderPathA(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPSTR pszPath);
BO1_EXTERN_C BOOL WINAPI SHGetSpecialFolderPathA(HWND hwnd, LPSTR pszPath, int csidl, BOOL fCreate);
BO1_EXTERN_C int WINAPI SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES *psa);
#define SHGetFolderPath SHGetFolderPathA
#define SHGetSpecialFolderPath SHGetSpecialFolderPathA
