// The client's cinematics: SP's SCR_DrawCinematic and SCR_StopCinematic, and (zombies) the maps' loading movies and the
// fullscreen cinematics the MP exe never drew.
//
// Each Zombies map's loading screen in the SP game is a Bink movie, main/video/<map>_load.bik (1280x720, with sound):
// the game's video/cin_levels.txt (in patch.ff) plays <map>_load as a map loads, and zombie_coast_outro once a solo game
// of Call of the Dead ends and its menus come back. The MP client drew a still, loadscreen_<map>, over which the loading
// bar and "Compiling shaders" go. Now:
//
//  - CL_LoadingMovieStart plays <map>_load as the client loads a Zombies map (CL_InitCGame; in a game alone, as its
//    server loads it, server_mp/sv_init_mp.cpp: SV_SpawnServer, where the SP game started it), with sound, once: a load
//    that outlasts the movie goes on over the still. While it plays the loading screen is the movie alone, in its
//    proportions, with the menus over it (CL_DrawLoadingMovie, from SCR_DrawScreenField), as the SP game's was:
//    offline, its connect screen drew nothing over the menus (SP 0x0043D590, from SCR_DrawScreenField 0x007A1590),
//    where the MP client's put SP's connect menu, the still and the loading bar. The menus draw no cursor over it, or
//    over the still after it, until the game has loaded, where the SP game's did, the system's showing instead
//    (CL_LoadingMovieHidesCursor). A map with no movie gets the engine's default (R_Cinematic_StartPlayback_Now:
//    default.bik), else the still.
//  - In the browser the page plays it, apart from the engine (Sys_LoadingMoviePlay; web/src/bink_web.cpp), from the
//    engine's start, as the player clicks Play, the page giving the map: the client plays on with it as it loads the
//    map, and its loading screen draws nothing where it shows, only the menus over it (CL_DrawLoadingMovie), from its
//    first connect screen on. So it starts at once, and the load holds up neither its pictures nor its sound.
//  - A game loaded before the movie ends waits for it, as the SP game did (CL_FirstSnapshot: CL_LoadingMovieHolds): the
//    client stays in CA_PRIMED, which the server does not count among the players in (getnumconnectedplayers counts
//    CA_ACTIVE: round 1 waits for them, src/server_mp/sv_lobby_mp.cpp). The SP game paused instead (cl_paused), alone.
//    The game, loaded, opens SP's pregame menu as the SP game's map command did, which has the keys and the mouse, its
//    line "LEFT MOUSE skip" fading after a few seconds (ui_mp/ui_main_mp.cpp: UI_SetActiveMenu, UIMENU_PREGAME), and
//    the left mouse button then takes the player in (CL_LoadingMovieSkip, from CL_KeyEvent). The movie plays on over
//    the game until the game is theirs (CL_LoadingMovieOverGame): alone, a moment later, as they spawn; in a room
//    whose others are still loading, as they all are in, the briefing's "Waiting for other players" over it, where the
//    SP game's online players waited on the map's still and a full loading bar; back in a game under way, at once. A
//    click during the load does nothing to the movie, as in the SP game, whose load threw its clicks away. Moon's movie is a minute on
//    one comic-book page, which looks like a still, Call of the Dead's its two-minute intro, "Five"'s a minute and a
//    half. cl_loadingMovie 0 plays none (a still, straight in), as a headless client has.
//  - CL_PlayCinematic plays a movie fullscreen in CA_CINEMATIC, as SP's `cinematic` command did: SCR_DrawCinematic draws
//    it in its proportions, and it ends with the movie or at a key (CL_StopLogoOrCinematic, SCR_StopCinematic). The
//    `cinematic <name>` command, out of a game. And Call of the Dead's outro, the film's credits, at game over
//    (CL_GameEnded: CL_PlayOutro), after which the engine ends as it would have, saying why: a room's game's players
//    hear of it from their server, and a game alone ends its client the same way (server_mp/sv_lobby_mp.cpp:
//    SV_Lobby_GameOver).
//
// The playback flags (gfx_d3d/r_cinematic.cpp: playbackFlags): 0x1 fullscreen (SP's named cinematics add it; no pause on
// a read underrun), 0x2 looping, 0x8 from a zone's rawfile (bik/<name>.bik), 0x40 timed by the clock rather than by Bink
// (SP's 3D cinematics: 0x42, fullscreen ones 0x43), 0x80 paused at the start.
#include "cl_cin.h"
#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>
#include <server_mp/sv_init_mp.h>
#include "client.h"
#include <client_mp/cl_cgame_mp.h>
#include <client_mp/cl_main_mp.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <win32/win_main.h>
#include <win32/win_shared.h>
#include <ui/ui_main.h>
#include <stdlib.h>
#include <string.h>

bool cin_skippable = true;

#define CINEMATIC_FLAGS_FULLSCREEN 0x41 // fullscreen, timed by the clock (above)

const dvar_t *cl_loadingMovie;

static char s_map[64];              // the map the client last loaded (CL_LoadingMovieStart), for its outro
static char s_loadingMovie[80];     // the loading movie asked for, "" when none is on
static bool s_loadingMovieBySys;    // the platform plays it (Sys_LoadingMoviePlay), not the engine's cinematics
static bool s_loadingMovieSkipped;  // a click took the player in (CL_LoadingMovieSkip), the movie playing on over the game
static unsigned int s_loadingMovieAsked; // when, for a movie that never comes
static int s_loadingMovieWentIn;    // when the game went in under it, 0 before (CL_LoadingMovieOverGame)
static bool s_loadingMovieSpectated; // the player was seen not yet spawned under it
static bool s_loadingMovieWaited;   // the player waited for the others under it, in the briefing
static bool s_loadingMovieOverGame; // it covers the game this frame
static bool s_cinematicAsked;       // CL_PlayCinematic has asked for a movie not yet seen playing
static bool s_quitAfterCinematic;   // the engine ends with the cinematic (CL_PlayOutro), saying:
static char s_endWhy[32], s_endText[1024];

static void CL_RegisterCinematicDvars()
{
    if ( !cl_loadingMovie )
        cl_loadingMovie = _Dvar_RegisterBool("cl_loadingMovie", 1, 0, "Play a map's loading movie (main/video/<map>_load.bik) as it loads, and Call of the Dead's outro at game over");
}

// the picture, in its proportions, as large as the screen takes it, over black
static void CL_DrawCinematicPicture()
{
    float sw = (float)cls.vidConfig.displayWidth, sh = (float)cls.vidConfig.displayHeight;
    float x = 0.0f, y = 0.0f, w = sw, h = sh;
    unsigned int vw, vh;
    if ( R_Cinematic_GetSize(&vw, &vh) )
    {
        if ( sw * (float)vh > sh * (float)vw )
        {
            w = sh * (float)vw / (float)vh; // a wider screen: bars at the sides
            x = (sw - w) * 0.5f;
        }
        else
        {
            h = sw * (float)vh / (float)vw;
            y = (sh - h) * 0.5f;
        }
    }
    float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdDrawStretchPic(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, white, rgp.cinematicMaterial);
}

void __cdecl SCR_DrawCinematic()
{
    if ( R_Cinematic_IsNextReady() )
        R_Cinematic_StartNextPlayback();
    // zombies: a movie that could not be opened (nothing pending, nothing playing) ends as a finished one does
    if ( R_Cinematic_IsFinished() || (s_cinematicAsked && !R_Cinematic_IsPending() && !R_Cinematic_IsStarted()) )
    {
        if ( Dvar_GetBool("com_movieIsPlaying") )
            Dvar_SetBool((dvar_s *)com_movieIsPlaying, 0);
        SCR_StopCinematic();
        return;
    }
    if ( R_Cinematic_IsStarted() && !Dvar_GetBool("com_movieIsPlaying") )
        Dvar_SetBool((dvar_s *)com_movieIsPlaying, 1);
    // zombies: SP drew the movie here; the MP exe had none to draw
    if ( R_Cinematic_IsStarted() )
        CL_DrawCinematicPicture();
}

void __cdecl SCR_StopCinematic()
{
    const char *v0; // eax
    int clientNum; // [esp+0h] [ebp-8h]
    bool oneClientWasChanged; // [esp+7h] [ebp-1h]

    if ( cin_skippable || R_Cinematic_IsFinished() )
    {
        R_Cinematic_StopPlayback();
        s_cinematicAsked = false;
        oneClientWasChanged = 0;
        for ( clientNum = 0; clientNum < 1; ++clientNum )
        {
            if ( CL_GetLocalClientConnectionState(clientNum) == 1 || CL_GetLocalClientConnectionState(clientNum) == 2 )
            {
                oneClientWasChanged = 1;
                CL_SetLocalClientConnectionState(clientNum, CA_DISCONNECTED);
            }
        }
        if ( oneClientWasChanged && *(_BYTE *)nextmap->current.integer )
        {
            v0 = va("%s\n", nextmap->current.string);
            Cbuf_AddText(0, v0);
            Dvar_SetString((dvar_s *)nextmap, "");
        }
        // zombies: the outro was the last of the game: the engine ends now, as it would have (CL_GameEnded)
        if ( s_quitAfterCinematic )
        {
            s_quitAfterCinematic = false;
            Sys_GameEnded(s_endWhy, s_endText);
            Cbuf_AddText(0, "quit\n");
        }
    }
}

// ---- zombies: the loading movie ----

// The client starts loading a map (CL_InitCGame), or a game alone's server starts loading it for the client
// (server_mp/sv_init_mp.cpp: SV_SpawnServer): its loading movie plays, when it has one, and it is a Zombies map's (the
// campaign's are the campaign's business), on a client that draws. The client, then starting on the map its server
// loaded, keeps the movie the load began with, playing, ended or skipped. The loading screen shows from here, the movie
// or the still: the web client's page, which kept the canvas out of sight over its movie while the engine started and
// joined its game, shows the canvas now (Sys_LoadingScreenShows). The platform plays the movie when it can (the web
// client's page, which started this one as the engine started), else the engine's cinematics.
void CL_LoadingMovieStart(const char *mapname)
{
    CL_RegisterCinematicDvars();
    Sys_LoadingScreenShows(); // the web client's page shows the canvas from now on (web/src/web_main.cpp)
    if ( s_loadingMovie[0] && mapname && !I_stricmp(s_map, mapname) )
        return;
    I_strncpyz(s_map, mapname ? mapname : "", sizeof(s_map));
    s_loadingMovie[0] = 0;
    s_loadingMovieSkipped = false;
    s_loadingMovieWentIn = 0;
    s_loadingMovieSpectated = false;
    s_loadingMovieWaited = false;
    if ( IsDedicatedServer() || Sys_IsHeadlessClient() || !s_map[0] || !Com_IsZombieMap(s_map) || !cl_loadingMovie->current.enabled )
    {
        Sys_LoadingMovieStop(); // one the page started for the map it gave (web), which plays none
        return;
    }
    Com_sprintf(s_loadingMovie, sizeof(s_loadingMovie), "%s_load", s_map);
    s_loadingMovieAsked = Sys_Milliseconds();
    Com_Printf(14, "Playing the loading movie %s\n", s_loadingMovie);
    s_loadingMovieBySys = Sys_LoadingMoviePlay(s_loadingMovie);
    if ( !s_loadingMovieBySys )
        R_Cinematic_StartPlayback_SP(s_loadingMovie, CINEMATIC_FLAGS_FULLSCREEN, 1.0f);
}

// The loading screen draws the movie in the still's place: it plays. The platform's may play before the client knows
// the map (web: from the engine's start), and its connect screen is the movie then too.
bool CL_LoadingMovieShows()
{
    if ( !s_loadingMovie[0] || s_loadingMovieBySys )
        return Sys_LoadingMovieState() == SYS_LOADING_MOVIE_SHOWS;
    return R_Cinematic_IsStarted();
}

// the movie plays on no more: the platform's, or the engine's
static void CL_LoadingMovieStopPlaying()
{
    if ( s_loadingMovieBySys )
        Sys_LoadingMovieStop();
    else
        R_Cinematic_StopPlayback();
}

// a loading movie is on: asked for, and not yet done with (it may be playing, or about to)
bool CL_LoadingMovieOn()
{
    return s_loadingMovie[0] && !s_loadingMovieSkipped;
}

// A loaded game waits for its loading movie to end, or for a click: true while it does (CL_FirstSnapshot). A movie
// that has ended, or never came (one that could not be opened fails at once; none in 10 s, nothing plays it), is done
// with, and the game goes on; after a click, the game goes on under it (CL_LoadingMovieOverGame).
bool CL_LoadingMovieHolds(int localClientNum)
{
    if ( !s_loadingMovie[0] )
        return false;
    if ( s_loadingMovieSkipped )
        return false;
    // pending first: the worker names the movie it plays before it is no longer pending (r_cinematic.cpp), so one that
    // is neither, asked in that order, has ended or failed (the platform's likewise)
    bool pending = s_loadingMovieBySys ? Sys_LoadingMovieState() == SYS_LOADING_MOVIE_PENDING : R_Cinematic_IsPending();
    if ( pending && Sys_Milliseconds() - s_loadingMovieAsked < 10000 )
        return true;
    if ( CL_LoadingMovieShows() )
        return true;
    if ( pending )
        Com_Printf(14, "The loading movie %s did not start: going in without it\n", s_loadingMovie);
    CL_LoadingMovieStop();
    return false;
}

// The left mouse button skips the rest of the movie for this player (CL_KeyEvent) once the loaded game waits for it,
// its pregame menu up (CL_FirstSnapshot): the game goes in at once (CL_LoadingMovieHolds), the movie playing on over it
// until the game is the player's (CL_LoadingMovieOverGame). True when it did. A click during the load does nothing to
// it.
bool CL_LoadingMovieSkip(int localClientNum)
{
    if ( !s_loadingMovie[0] || s_loadingMovieSkipped || !CL_LoadingMovieShows()
         || CL_GetLocalClientConnectionState(localClientNum) != CA_PRIMED
         || UI_GetActiveMenu(localClientNum) != UIMENU_PREGAME )
        return false;
    s_loadingMovieSkipped = true;
    Com_Printf(14, "The loading movie was skipped\n");
    return true;
}

// After the click (CL_LoadingMovieSkip) the game goes in under the movie, which plays on over it until the game is the
// player's: they spawn (alone, a moment after the click; pmType, the snapshot's pm_type, -1 before one, was 4 until
// then), the briefing closes as the others are all in, its "Waiting for other players" over the movie (ui/ui_main.cpp:
// UI_BriefingDrawsItem), or the movie ends, the briefing's still in its place. A room's game whose round 1 has begun
// takes only players coming back to their character, with no briefing to wait in, and says so in its serverinfo
// (server_mp/sv_lobby_mp.cpp: bo1_started): the movie leaves as they go in. Elsewhere, one who goes in with no briefing
// coming sees the game once it would have come: a second and their ping both ways after going in. Each frame of the
// game in, before the cgame's, which runs under it as under a full-screen menu, drawing nothing, the screen drawing the
// movie (client_mp/cl_scrn_mp.cpp: CL_CGameRendering, SCR_DrawScreenField). True when it covers the game.
bool CL_LoadingMovieOverGame(int localClientNum, int pmType)
{
    s_loadingMovieOverGame = false;
    if ( !s_loadingMovie[0] || !s_loadingMovieSkipped )
        return false;
    const int now = Sys_Milliseconds();
    bool underWay = false;
    if ( !s_loadingMovieWentIn )
    {
        s_loadingMovieWentIn = now ? now : 1;
        underWay = atoi(Info_ValueForKey(CL_GetConfigString(0), "bo1_started")) != 0;
    }
    const bool waits = UI_WaitsForOtherPlayers(localClientNum);
    if ( waits && !s_loadingMovieWaited )
    {
        s_loadingMovieWaited = true;
        Com_Printf(14, "The player waits for the others over the loading movie\n");
    }
    const char *why = nullptr;
    if ( pmType == 4 )
        s_loadingMovieSpectated = true;
    if ( underWay )
        why = "the game is under way";
    else if ( !CL_LoadingMovieShows() )
        why = "it ended";
    else if ( pmType >= 0 && pmType != 4 && s_loadingMovieSpectated )
        why = "the player spawned";
    else if ( s_loadingMovieWaited && !waits )
        why = "the others are in";
    else if ( !s_loadingMovieWaited && now - s_loadingMovieWentIn > 1000 + 2 * CL_GetLocalClientGlobals(localClientNum)->snap.ping )
        why = "no wait for the others came";
    if ( why )
    {
        Com_Printf(14, "The loading movie leaves the game: %s\n", why);
        CL_LoadingMovieStop();
        return false;
    }
    s_loadingMovieOverGame = true;
    return true;
}

// the movie covers the game this frame (CL_LoadingMovieOverGame)
bool CL_LoadingMovieCoversGame()
{
    return s_loadingMovieOverGame;
}

// The menus draw no cursor over the movie as the game loads (ui/ui_main.cpp: UI_Refresh), and the system's shows where
// they would have, the mouse theirs (client_mp/cl_input_mp.cpp: CL_MouseEvent, UI_MouseEvent; web: the browser's arrow,
// until a click captures the mouse), from the engine's start, which hid it (client_mp/cl_scrn_mp.cpp:
// SCR_DrawScreenField). The SP game's did, its waits for the level's assets giving the menus the mouse
// (DB_Sleep, SP 0x007A2830), but a click does nothing then, and the cursor moved only as the loading screen drew, 30
// times a second or fewer, and stood still while the client did more than wait for the database. Nor over the still
// where the load outlasts the movie: the connect screen draws the still and the loading bar over the menus, and so over
// their cursor, which showed only beside a still narrower than the screen, moving as the loading screen drew (the web
// client's wait for its pipelines, CL_InitCGame, a few times a second). Once loaded, the pregame menu's cursor shows
// (ui_mp/ui_main_mp.cpp: UI_SetActiveMenu), or the game's goes in.
bool CL_LoadingMovieHidesCursor(int localClientNum)
{
    if ( s_loadingMovieSkipped || UI_GetActiveMenu(localClientNum) == UIMENU_PREGAME )
        return false;
    if ( CL_LoadingMovieShows() )
        return true;
    const connstate_t state = CL_GetLocalClientConnectionState(localClientNum);
    return s_loadingMovie[0] && state >= CA_CONNECTING && state <= CA_PRIMED;
}

// The platform's movie shows under the screen (web: the page's canvas under the game's), which draws nothing over it:
// the screen is cleared to transparent black where it shows, and the menus draw over that.
static bool CL_LoadingMovieUnderScreen()
{
    return (!s_loadingMovie[0] || s_loadingMovieBySys) && Sys_LoadingMovieState() == SYS_LOADING_MOVIE_SHOWS;
}

static void CL_ClearScreenToLoadingMovie()
{
    static const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    R_AddCmdClearScreen(1, transparent, 1.0f, 0);
}

// The loading screen while the movie plays (SCR_DrawScreenField): the movie alone, as the SP game's loading screen was,
// under the menus (the game loaded, its pregame menu). True when it drew it; the MP client's loading screen, SP's
// connect menu with the map's still and the loading bar, comes off the menus' stack (UI_CloseLoadingScreen).
bool CL_DrawLoadingMovie(int localClientNum)
{
    if ( !CL_LoadingMovieShows() )
        return false;
    UI_CloseLoadingScreen(localClientNum);
    if ( CL_LoadingMovieUnderScreen() )
        CL_ClearScreenToLoadingMovie();
    else
        CL_DrawCinematicPicture();
    return true;
}

// A menu's picture of the cinematic (ui/ui_atoms.cpp: UI_DrawHandlePic): the pregame menu's, full screen over its own
// black, which shows the loading movie playing as SP's did. Under the screen, the platform's movie shows through it:
// the screen is cleared to transparent there instead. True when it was.
bool CL_DrawLoadingMoviePicture(Material *material)
{
    if ( material != rgp.cinematicMaterial || !CL_LoadingMovieUnderScreen() )
        return false;
    CL_ClearScreenToLoadingMovie();
    return true;
}

// The movie is done with: the game is in (CL_LoadingMovieHolds, CL_LoadingMovieOverGame), or the client disconnected.
// Not the platform's before the client plays on with it: the client disconnects from nothing as it starts to connect,
// and as a game alone's server starts the map.
void CL_LoadingMovieStop()
{
    s_loadingMovieOverGame = false;
    if ( !s_loadingMovie[0] )
        return;
    s_loadingMovie[0] = 0;
    CL_LoadingMovieStopPlaying();
}

// ---- zombies: fullscreen cinematics ----

// plays a movie fullscreen, in CA_CINEMATIC, until it ends or a key (above)
void CL_PlayCinematic(const char *name, unsigned int playbackFlags)
{
    CL_LoadingMovieStop();
    Com_Printf(14, "Playing cinematic %s\n", name);
    s_cinematicAsked = true;
    R_Cinematic_StartPlayback_SP(name, playbackFlags, 1.0f);
    CL_SetLocalClientConnectionState(0, CA_CINEMATIC);
}

// The game ended (CL_GameEnded: why, and the words for the player): at game over on Call of the Dead, its outro rolls
// first, the film's credits, as the SP game played them coming back to its menus after a solo game (here a co-op game's
// players see them too). True when it does: the engine then ends with it (SCR_StopCinematic), saying why as it would
// have. Nothing comes over it meanwhile: the client, starting its menus again, opens none in CA_CINEMATIC
// (ui/ui_shared.cpp: UI_GetForcedMenuScreen), nor gives them the keys (CL_Frame), nor loads the front end
// (qcommon/common.cpp: Com_AssetLoadUIWithMsg).
bool CL_PlayOutro(const char *why, const char *text)
{
    CL_RegisterCinematicDvars();
    if ( strcmp(why, "over") || IsDedicatedServer() || Sys_IsHeadlessClient() || !cl_loadingMovie->current.enabled ||
         I_stricmp(s_map, "zombie_coast") )
        return false;
    I_strncpyz(s_endWhy, why, sizeof(s_endWhy));
    I_strncpyz(s_endText, text, sizeof(s_endText));
    s_quitAfterCinematic = true;
    Sys_GameEndsAfterOutro(); // the web client's page waits for it
    CL_PlayCinematic("zombie_coast_outro", CINEMATIC_FLAGS_FULLSCREEN);
    return true;
}

// cinematic <name> [flags]: plays a movie of main/video/ fullscreen, out of a game (SP's command, for a look at one:
// cinematic zombie_theater_load)
cmd_function_s CL_PlayCinematic_f_VAR;
void __cdecl CL_PlayCinematic_f()
{
    if ( Cmd_Argc() < 2 )
    {
        Com_Printf(0, "USAGE: cinematic <name> [flags]\n");
        return;
    }
    if ( CL_GetLocalClientConnectionState(0) != CA_DISCONNECTED && CL_GetLocalClientConnectionState(0) != CA_CINEMATIC )
    {
        Com_Printf(0, "cinematic: not while in a game\n");
        return;
    }
    unsigned int flags = CINEMATIC_FLAGS_FULLSCREEN;
    if ( Cmd_Argc() > 2 )
        flags = (unsigned int)strtoul(Cmd_Argv(2), 0, 0);
    CL_PlayCinematic(Cmd_Argv(1), flags);
}
