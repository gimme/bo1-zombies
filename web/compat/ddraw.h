// DirectDraw: r_texturemem.h names its function-pointer types; ddraw.dll never loads in the browser
#pragma once
#include "windows.h"
typedef struct _DDSCAPS2
{
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    union
    {
        DWORD dwCaps4;
        DWORD dwVolumeDepth;
    };
} DDSCAPS2, *LPDDSCAPS2;
#define DDSCAPS_VIDEOMEMORY 0x00004000l
#define DDSCAPS_LOCALVIDMEM 0x10000000l
#define DDSCAPS_TEXTURE 0x00001000l
EXTERN_C const GUID IID_IDirectDraw7;
#ifdef __cplusplus
struct IDirectDraw7 : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hWnd, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS2 lpDDSCaps2, LPDWORD lpdwTotal, LPDWORD lpdwFree) = 0;
};
#endif
