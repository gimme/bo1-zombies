// Stand-ins for steam_api.dll: the engine is built without Steam.
//
// These are the functions of the Steamworks API that the engine and the inline functions in the Steamworks headers
// call. SteamAPI_Init reports that no Steam client is running and every interface is null, so the engine takes its
// no-Steam paths (win_steam.cpp). The Windows build compiles the headers with STEAM_API_NODLL, which makes the calls
// link to these definitions instead of the DLL's.
#include <steam/steam_api.h>

#include <cstdio>

S_API ESteamAPIInitResult S_CALLTYPE SteamInternal_SteamAPI_Init(const char *, SteamErrMsg *pOutErrMsg)
{
    if (pOutErrMsg)
        snprintf(*pOutErrMsg, sizeof(SteamErrMsg), "this build has no Steam");
    return k_ESteamAPIInitResult_NoSteamClient;
}

S_API void S_CALLTYPE SteamAPI_Shutdown() {}

// The headers' interface accessors (SteamUser(), SteamFriends(), ...) call this with
// { void (*init)(void *ctx); uintptr_t counter; void *ptr; }. It runs init once and returns &ptr.
S_API void *S_CALLTYPE SteamInternal_ContextInit(void *pContextInitData)
{
    void **ctx = (void **)pContextInitData;
    if (!ctx[1])
    {
        ((void (*)(void *))ctx[0])(&ctx[2]);
        ctx[1] = (void *)1;
    }
    return &ctx[2];
}

S_API void *S_CALLTYPE SteamInternal_FindOrCreateUserInterface(HSteamUser, const char *) { return nullptr; }
S_API void *S_CALLTYPE SteamInternal_FindOrCreateGameServerInterface(HSteamUser, const char *) { return nullptr; }
S_API void *S_CALLTYPE SteamInternal_CreateInterface(const char *) { return nullptr; }

S_API HSteamUser SteamAPI_GetHSteamUser() { return 0; }
S_API HSteamPipe SteamAPI_GetHSteamPipe() { return 0; }
S_API HSteamUser SteamGameServer_GetHSteamUser() { return 0; }

S_API void S_CALLTYPE SteamAPI_RunCallbacks() {}
S_API void S_CALLTYPE SteamGameServer_RunCallbacks() {}
S_API void S_CALLTYPE SteamAPI_RegisterCallback(class CCallbackBase *, int) {}
S_API void S_CALLTYPE SteamAPI_UnregisterCallback(class CCallbackBase *) {}
S_API void S_CALLTYPE SteamAPI_RegisterCallResult(class CCallbackBase *, SteamAPICall_t) {}
S_API void S_CALLTYPE SteamAPI_UnregisterCallResult(class CCallbackBase *, SteamAPICall_t) {}
