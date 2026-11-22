// Libraries and Windows-only features the WebAssembly build goes without. Each stub answers the way the engine
// already handles a missing feature on Windows: Bink cannot open a movie (cinematics are skipped), no movie capture,
// no voice chat, no D3DX shader compiler (fastfiles carry compiled shaders). Steam's stand-ins are
// the Windows build's too (src/win32/win_steam_none.cpp).
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <binklib/bink.h>
#include <physics/phys_broad_phase.h>
#include <gfx_d3d/r_model_skin.h>
#include <win32/win_voice.h>

#include "web_platform.h"

// ----- Bink (src/binklib is the closed RAD library) -----
// With FFmpeg's Bink decoders (BO1_WEB_BINK: web/CMakeLists.txt, docker/Dockerfile's web target), bink_web.cpp plays
// the movies instead, and these stubs are left out.

#ifndef BO1_WEB_BINK
extern "C" {
RADEXPFUNC HBINK RADEXPLINK BinkOpen(const char *name, U32)
{
    Bo1_Log("BinkOpen %s: no Bink in the browser, cinematic skipped", name ? name : "");
    return 0;
}
RADEXPFUNC void RADEXPLINK BinkClose(HBINK) {}
RADEXPFUNC S32 RADEXPLINK BinkControlBackgroundIO(HBINK, U32) { return 0; }
RADEXPFUNC S32 RADEXPLINK BinkDoFrame(HBINK) { return 0; }
RADEXPFUNC char *RADEXPLINK BinkGetError(void) { return (char *)"Bink is not available in the browser"; }
RADEXPFUNC void RADEXPLINK BinkGetFrameBuffersInfo(HBINK, BINKFRAMEBUFFERS *fbset) { memset(fbset, 0, sizeof(*fbset)); }
RADEXPFUNC void RADEXPLINK BinkGetRealtime(HBINK, BINKREALTIME *run, U32) { memset(run, 0, sizeof(*run)); }
RADEXPFUNC void RADEXPLINK BinkNextFrame(HBINK) {}
RADEXPFUNC BINKSNDOPEN RADEXPLINK BinkOpenDirectSound(UINTa) { return 0; }
RADEXPFUNC S32 RADEXPLINK BinkPause(HBINK, S32) { return 0; }
RADEXPFUNC void RADEXPLINK BinkRegisterFrameBuffers(HBINK, BINKFRAMEBUFFERS *) {}
RADEXPFUNC void RADEXPLINK BinkSetIOSize(U32) {}
RADEXPFUNC void RADEXPLINK BinkSetMemory(BINKMEMALLOC, BINKMEMFREE) {}
RADEXPFUNC S32 RADEXPLINK BinkSetSoundSystem(BINKSNDSYSOPEN, UINTa) { return 0; }
RADEXPFUNC void RADEXPLINK BinkSetSoundTrack(U32, U32 *) {}
RADEXPFUNC void RADEXPLINK BinkSetVolume(HBINK, U32, S32) {}
RADEXPFUNC S32 RADEXPLINK BinkShouldSkip(HBINK) { return 0; }
RADEXPFUNC S32 RADEXPLINK BinkWait(HBINK) { return 0; }
}
bool Bo1_MovieStart(const char *, float) { return false; }
int Bo1_MovieState() { return BO1_MOVIE_NONE; }
void Bo1_MovieStop() {}
bool Bo1_MovieUnder() { return false; }
#endif // !BO1_WEB_BINK

// ----- movie capture (src/mjpeg) -----

bool __cdecl mjpeg_is_encoding() { return false; }
void __cdecl mjpeg_close() {}
void __cdecl mjpeg_draw() {}
void __cdecl yuv_lost_device() {}
void __cdecl yuv_recover_device() {}

// ----- voice chat (win_voice.cpp: DirectSound capture and the winmm mixer) -----

float voice_current_scaler = 1.0f;
bool __cdecl Voice_Init() { return false; }
void __cdecl Voice_StopClientSamples() {}
void __cdecl Voice_Shutdown() {}
double __cdecl Voice_GetVoiceLevel() { return 0.0; }
void __cdecl Voice_Playback() {}
int __cdecl Voice_GetLocalVoiceData() { return 0; }
void __cdecl Voice_IncomingVoiceData(unsigned __int8, unsigned __int8 *, int) {}
bool __cdecl Voice_IsClientTalking(unsigned int) { return false; }
char __cdecl Voice_StartRecording() { return 0; }
char __cdecl Voice_StopRecording() { return 0; }
unsigned int __cdecl mixerGetRecordLevel(char *) { return 0; }
int __cdecl mixerSetRecordLevel(char *, unsigned __int16) { return 0; }
int __cdecl mixerGetRecordSource(char *) { return 0; }
int __cdecl mixerSetRecordSource(char *) { return 0; }
int __cdecl mixerSetMicrophoneMute(unsigned __int8) { return 0; }
int __cdecl Live_GetClientNumForXuid(const SessionData_s *, unsigned __int64) { return -1; }

// ----- engine functions whose only definition is in a file the browser build leaves out -----

// r_model_skin_sse.cpp (MMX): never called, sys_SSE is 0 here
void __cdecl R_SkinXSurfaceSkinnedSse(const XSurface *xsurf, const DObjSkelMat *boneMatrix, GfxPackedVertexNormal *,
    GfxPackedVertexNormal *, GfxPackedVertex *skinVerticesOut)
{
    R_SkinXSurfaceSkinned(xsurf, boneMatrix, skinVerticesOut);
}

// declared virtual and never defined (MSVC emits no vtable for it); the derived queries override it
void broad_phase_terrain_query_callback::query(const broad_phase_environment_query_input *, broad_phase_environement_query_results *) {}

// ----- D3DX / DxErr: no runtime shader compiler; fastfile shaders are already compiled -----

HRESULT WINAPI D3DXCompileShader(LPCSTR, UINT, CONST D3DXMACRO *, LPD3DXINCLUDE, LPCSTR, LPCSTR, DWORD, LPD3DXBUFFER *ppShader,
    LPD3DXBUFFER *ppErrorMsgs, LPD3DXCONSTANTTABLE *ppConstantTable)
{
    if (ppShader)
        *ppShader = NULL;
    if (ppErrorMsgs)
        *ppErrorMsgs = NULL;
    if (ppConstantTable)
        *ppConstantTable = NULL;
    return E_NOTIMPL;
}

struct Bo1D3DXBuffer : ID3DXBuffer
{
    ULONG refs = 1;
    DWORD size;
    void *data;
    explicit Bo1D3DXBuffer(DWORD n) : size(n), data(calloc(1, n ? n : 1)) {}
    STDMETHOD(QueryInterface)(REFIID, LPVOID *ppv) override
    {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG r = --refs;
        if (!r)
        {
            free(data);
            delete this;
        }
        return r;
    }
    STDMETHOD_(LPVOID, GetBufferPointer)() override { return data; }
    STDMETHOD_(DWORD, GetBufferSize)() override { return size; }
};

HRESULT WINAPI D3DXCreateBuffer(DWORD NumBytes, LPD3DXBUFFER *ppBuffer)
{
    *ppBuffer = new Bo1D3DXBuffer(NumBytes);
    return S_OK;
}

HRESULT WINAPI D3DXGetShaderConstantTable(CONST DWORD *, LPD3DXCONSTANTTABLE *ppConstantTable)
{
    *ppConstantTable = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXGetShaderInputSemantics(CONST DWORD *, D3DXSEMANTIC *, UINT *pCount)
{
    if (pCount)
        *pCount = 0;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXGetShaderOutputSemantics(CONST DWORD *, D3DXSEMANTIC *, UINT *pCount)
{
    if (pCount)
        *pCount = 0;
    return E_NOTIMPL;
}

extern "C" const char *WINAPI DXGetErrorDescriptionA(HRESULT hr)
{
    static thread_local char text[32];
    snprintf(text, sizeof(text), "HRESULT 0x%08x", (unsigned)hr);
    return text;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
int WINAPI D3DPERF_EndEvent(void) { return 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }

// ----- CubeMapGen (reflection probe generation, a developer tool; wide-char Win32 UI) -----

#include <CubeMapGenLib/CCubeMapProcessor.h>

CCubeMapProcessor::CCubeMapProcessor() {}
CCubeMapProcessor::~CCubeMapProcessor() {}
void CCubeMapProcessor::Init(int32, int32, int32, int32) {}
void CCubeMapProcessor::SetInputFaceData(int32, int32, int32, int32, void *, float32, float32, float32) {}
void CCubeMapProcessor::GetOutputFaceData(int32, int32, int32, int32, int32, void *, float32, float32) {}
void CCubeMapProcessor::InitiateFiltering(float32, float32, float32, int32, int32, int32, bool8) {}
int32 CCubeMapProcessor::GetStatus() { return 0; }

CImageSurface::CImageSurface() {}
CImageSurface::~CImageSurface() {}

// ----- more of the above -----

// DemonWare (not linked): the interceptors' base classes
#include <demo/demo_common.h>
#include <live/live_storage.h>
unsigned int bdDownloadInterceptor::handleDownload(void *, unsigned int) { return 0; }
unsigned int bdUploadInterceptor::handleUpload(void *, unsigned int, unsigned int) { return 0; }

void mjpeg_initonce() {}
void __cdecl mjpeg_add_samples(__int16 *, int) {} // the movie recorder's sound track (src/mjpeg is not built)

HRESULT WINAPI D3DXSaveSurfaceToFileA(LPCSTR, D3DXIMAGE_FILEFORMAT, LPDIRECT3DSURFACE9, CONST PALETTEENTRY *, CONST RECT *) { return E_NOTIMPL; }


