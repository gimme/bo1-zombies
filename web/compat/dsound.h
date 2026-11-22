// DirectSound: cinematic audio and voice chat name these types; DirectSoundCreate8 fails in the browser
// (web/src/win32_compat.cpp), which both callers already handle.
#pragma once
#include "windows.h"

#define DS_OK S_OK
#define _FACDS 0x878
#define MAKE_DSHRESULT(code) MAKE_HRESULT(1, _FACDS, code)
#define DSERR_ALLOCATED MAKE_DSHRESULT(10)
#define DSERR_INVALIDPARAM E_INVALIDARG
#define DSERR_INVALIDCALL MAKE_DSHRESULT(50)
#define DSERR_GENERIC E_FAIL
#define DSERR_PRIOLEVELNEEDED MAKE_DSHRESULT(70)
#define DSERR_OUTOFMEMORY E_OUTOFMEMORY
#define DSERR_BUFFERLOST MAKE_DSHRESULT(150)
#define DSERR_NODRIVER MAKE_DSHRESULT(120)
#define DSSCL_NORMAL 0x00000001
#define DSSCL_PRIORITY 0x00000002
#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBCAPS_STATIC 0x00000002
#define DSBCAPS_LOCSOFTWARE 0x00000008
#define DSBCAPS_CTRLFREQUENCY 0x00000020
#define DSBCAPS_CTRLPAN 0x00000040
#define DSBCAPS_CTRLVOLUME 0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBCAPS_GLOBALFOCUS 0x00008000
#define DSBPLAY_LOOPING 0x00000001
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_BUFFERLOST 0x00000002
#define DSBSTATUS_LOOPING 0x00000004
#define DSCBSTART_LOOPING 0x00000001
#define DSBVOLUME_MIN -10000
#define DSBVOLUME_MAX 0

typedef struct _DSBUFFERDESC
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    LPWAVEFORMATEX lpwfxFormat;
    GUID guid3DAlgorithm;
} DSBUFFERDESC, *LPDSBUFFERDESC;
typedef const DSBUFFERDESC *LPCDSBUFFERDESC;

typedef struct _DSCBUFFERDESC
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    LPWAVEFORMATEX lpwfxFormat;
    DWORD dwFXCount;
    void *lpDSCFXDesc;
} DSCBUFFERDESC, *LPDSCBUFFERDESC;
typedef const DSCBUFFERDESC *LPCDSCBUFFERDESC;

typedef struct _DSCAPS
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD reserved[22];
} DSCAPS, *LPDSCAPS;

#ifdef __cplusplus
struct IDirectSound;
struct IDirectSoundCapture;
struct IDirectSoundBuffer : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetCaps(void *pDSBufferCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD pdwCurrentPlayCursor, LPDWORD pdwCurrentWriteCursor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX pwfxFormat, DWORD dwSizeAllocated, LPDWORD pdwSizeWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolume(LPLONG plVolume) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPan(LPLONG plPan) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD pdwFrequency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD pdwStatus) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(IDirectSound *pDirectSound, LPCDSBUFFERDESC pcDSBufferDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE Lock(DWORD dwOffset, DWORD dwBytes, LPVOID *ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID *ppvAudioPtr2,
        LPDWORD pdwAudioBytes2, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Play(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD dwNewPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX pcfxFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(LONG lVolume) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPan(LONG lPan) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFrequency(DWORD dwFrequency) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unlock(LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2) = 0;
    virtual HRESULT STDMETHODCALLTYPE Restore(void) = 0;
};
struct IDirectSound : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC pcDSBufferDesc, IDirectSoundBuffer **ppDSBuffer, LPUNKNOWN pUnkOuter) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS pDSCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(IDirectSoundBuffer *pDSBufferOriginal, IDirectSoundBuffer **ppDSBufferDuplicate) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD dwLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE Compact(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD pdwSpeakerConfig) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD dwSpeakerConfig) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCGUID pcGuidDevice) = 0;
};
struct IDirectSound8 : public IDirectSound
{
    virtual HRESULT STDMETHODCALLTYPE VerifyCertification(LPDWORD pdwCertified) = 0;
};
struct IDirectSoundCaptureBuffer : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetCaps(void *pDSCBCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD pdwCapturePosition, LPDWORD pdwReadPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX pwfxFormat, DWORD dwSizeAllocated, LPDWORD pdwSizeWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD pdwStatus) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(IDirectSoundCapture *pDirectSoundCapture, LPCDSCBUFFERDESC pcDSCBufferDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE Lock(DWORD dwOffset, DWORD dwBytes, LPVOID *ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID *ppvAudioPtr2,
        LPDWORD pdwAudioBytes2, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unlock(LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2) = 0;
};
struct IDirectSoundCapture : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureBuffer(LPCDSCBUFFERDESC pcDSCBufferDesc, IDirectSoundCaptureBuffer **ppDSCBuffer, LPUNKNOWN pUnkOuter) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCaps(void *pDSCCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCGUID pcGuidDevice) = 0;
};
typedef IDirectSound *LPDIRECTSOUND;
typedef IDirectSound8 *LPDIRECTSOUND8;
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER;
typedef IDirectSoundCapture *LPDIRECTSOUNDCAPTURE;
typedef IDirectSoundCaptureBuffer *LPDIRECTSOUNDCAPTUREBUFFER;
EXTERN_C HRESULT WINAPI DirectSoundCreate8(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8, LPUNKNOWN pUnkOuter);
EXTERN_C HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID pcGuidDevice, LPDIRECTSOUNDCAPTURE *ppDSC, LPUNKNOWN pUnkOuter);
#endif
