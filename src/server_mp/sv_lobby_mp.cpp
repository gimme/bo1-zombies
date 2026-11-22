// zombies: what the lobby (zombies-hub's server/t5.go) needs of a room's game, which it runs with +set bo1_lobby 1. The
// game waits for the room's players before round 1 (bo1_players), says on its console who is in it and when round 1
// begins, keeps the character of a player who drops, or quits, for sv_timeout (the lobby's 5 minutes) and gives it back
// when they come back (SV_Lobby_TakesPlayersBack), takes no new player once round 1 has begun, kicks no one for
// standing idle, takes the names the lobby gives (as short as one letter) and keeps them (SV_Lobby_KeepsNames), and
// ends at game over rather than start the map again: the lobby takes its players back to their room.
// The lobby reads the lines it says:
//
//   bo1_lobby: in <address>    a client entered the game (SV_ClientEnterWorld)
//   bo1_lobby: out <address>   a client left it, and its character with it (SV_DropClient): they left the room
//                              (bo1_lobby_drop), were kicked, or were away for sv_timeout; not at game over, nor as the
//                              server shuts down
//   bo1_lobby: quit <address>  a client quit the game (its disconnect, SV_Disconnect_f: from its menu, or as its window
//                              closed), which keeps its character for it; one with none yet is out next
//   bo1_lobby: start           everyone expected is in: the level's scripts start round 1
//   bo1_lobby: stats <address> <score> <cash> <kills> <headshots> <downs> <revives> <doors>
//                              before each "round" and "over": a player's line of the report: what they earned, what's
//                              left of it to spend (the points the game shows, until its end scoreboard's total), the
//                              rest of that scoreboard then, and the doors they bought (SV_Lobby_Stats), for each
//                              player whose character is in the game; before "round", with the final down of a game
//                              that ends there
//   bo1_lobby: round <round>   a round begins (SV_Lobby_NewRound): a game that never reaches game over counts up to it
//   bo1_lobby: over <round>    game over, on the level's round (level.round_number; none when it keeps no round), as
//                              its end scoreboard begins (the level's intermission), or as the level exits without one:
//                              once the level exits, or every player has left the end scoreboard (client_mp/
//                              cl_main_mp.cpp: CL_LeaveGameOver), the game tells those still in it (BO1_GAMEOVER, which
//                              ends their game), and quits once they have heard it (SV_Lobby_TellOver)
//   bo1_lobby: dstat <address> <name> <value>
//                              the level's scripts changed a player's stat (setdstat: src/game_sp/g_sp_stats.cpp)
//   bo1_lobby: collectibles <address> <collectibles>
//                              a player's client set a collectible (setcollectible): all of theirs, as bg_collectibles
//                              has them ('1' at n - 1 for collectible n)
//
// An address is the client's as the game sees it, the relay's for the player (127.1.x.y:port). The lobby lets a
// player's character go when they leave the room (bo1_lobby_drop <address>): quitting the game only drops them. It gives
// the game, as it starts, before its map, in a config the game's command line execs, each player's place in the room
// (bo1_lobby_seat <address> <place>), and what it kept of each player's stats and collectibles (bo1_lobby_dstat
// <address> <name> <value>, bo1_lobby_collectibles <address> <collectibles>).
#include "sv_lobby_mp.h"

#include "server/server.h"
#include <server/sv_game.h>
#include "sv_init_mp.h"
#include "sv_main_mp.h"
#include <cgame_mp/cg_scoreboard_mp.h>
#include <client_mp/sv_client_mp.h>
#include <clientscript/cscr_compiler.h>
#include <clientscript/cscr_stringlist.h>
#include <game_mp/g_cmds_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_sp/g_sp_ext.h>
#include <game_sp/g_sp_stats.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>

const dvar_s *bo1_lobby;
const dvar_s *bo1_players;
static const dvar_s *bo1_started; // in the serverinfo: round 1 has begun (SV_Lobby_Frame)

static bool s_started; // the map's round 1 has begun: "bo1_lobby: start" said
static int s_roundSaid; // the last round "bo1_lobby: round" said
static bool s_shuttingDown; // the server shuts down (SV_Shutdown), and its clients go with it
static const char *s_finalMessage; // what the game tells its players as it quits; null: the usual
static bool s_quit[64]; // the client quit the game (SV_Disconnect_f), its character waiting for it: no one is there
static bool s_endScoreboard; // game over's end scoreboard has begun (SV_Lobby_LevelNotify: the level's intermission)
static bool s_overSaid; // "bo1_lobby: over" said (SV_Lobby_SayOver)
static bool s_over; // game over, on a dedicated server: it tells its players until they have heard (SV_Lobby_TellOver)
static bool s_overQuits; // and then quits
static int s_overAt, s_overToldAt; // when (svs.time), and when it last told those yet to hear it
static bool s_toHear[64]; // the players it waits for to hear it
static unsigned int s_roundNumberKey; // the level's round_number, as its scripts name the field (SV_Lobby_ScriptFields)
static unsigned int s_scoreTotalKey; // a player's score_total, likewise
static unsigned int s_scoreUnspentKey; // and their score_unspent (s_scrSourceFixes: the level's intermission keeps it)
static unsigned int s_intermission; // the string its game over's intermission begins with (SV_Lobby_LevelNotify)
static unsigned int s_stopIntermission; // and the one it stops with
static char s_seat[64][32]; // by client slot: the address of the player whose place in the room it is (bo1_lobby_seat)
static int s_doors[64]; // by client slot: the doors the player bought (SV_Lobby_BlackBox)

static const char *SV_Lobby_Address(const netadr_t &a);

// bo1_lobby_seat <address> <place>: the player at the address is at that place in the room (zombies-hub's server/t5.go:
// placesOf), 0 its host's, and takes the client slot of it (SV_Lobby_FreeSlot)
static void SV_Lobby_Seat_f()
{
    const int place = Cmd_Argc() == 3 ? atoi(Cmd_Argv(2)) : -1;
    if (place < 0 || place >= 64 || strlen(Cmd_Argv(1)) >= sizeof(s_seat[0]))
    {
        Com_Printf(0, "bo1_lobby_seat <address> <place>\n");
        return;
    }
    I_strncpyz(s_seat[place], Cmd_Argv(1), sizeof(s_seat[place]));
}

// The commands below change the game (its clients, the level's scripts, the players' stats), which the server thread
// runs: its frames, and the client packets it reads between them (SV_ServerThread). So they are server commands, as the
// engine's own kick, clientkick and fast_restart are: the console runs one only once the server thread has stopped
// (Cmd_ExecuteSingleCommandInternal: SV_WaitServer, which waits for its frame and stops its packet reads until the next
// frame), and it reads its arguments with SV_Cmd_Argv. A plain command runs on the main thread whenever the console
// has it, and bo1_lobby_drop, run as the server thread read the packets of a player still in the game, ran the level's
// scripts on both threads at once (ClientDisconnect's on one, the player's on the other): the script VM's stack, which
// they share, was overwritten under the notify one of them ran, which then read past its bottom, and the game ended
// at a script allocation of some 93 KB (MT_GetSize: max allocation exceeded).

// bo1_lobby_drop <address>: the player at the address left their room (zombies-hub's server/t5.go: drop), and the
// character the game keeps for them (SV_Lobby_KeepsCharacters) leaves it: a player's own disconnect (they quit the
// game) keeps it (SV_Disconnect_f)
static void SV_Lobby_Drop_f()
{
    if (SV_Cmd_Argc() != 2)
    {
        Com_Printf(0, "bo1_lobby_drop <address>\n");
        return;
    }
    const char *address = SV_Cmd_Argv(1);
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
    {
        client_t *cl = &svs.clients[i];
        if (cl->header.state > CS_ZOMBIE && !strcmp(SV_Lobby_Address(cl->header.netchan.remoteAddress), address))
            SV_DropClient(cl, "EXE_DISCONNECTED", true, true);
    }
}

// bo1_lobby_dstat <address> <name> <value>: a stat the lobby kept of the player at the address
static void SV_Lobby_DStat_f()
{
    if (SV_Cmd_Argc() != 4)
    {
        Com_Printf(0, "bo1_lobby_dstat <address> <name> <value>\n");
        return;
    }
    G_SP_SetPlayerStat(SV_Cmd_Argv(1), SV_Cmd_Argv(2), atoi(SV_Cmd_Argv(3)));
}

// bo1_lobby_collectibles <address> <collectibles>: the collectibles the lobby kept of the player at the address
static void SV_Lobby_Collectibles_f()
{
    if (SV_Cmd_Argc() != 3)
    {
        Com_Printf(0, "bo1_lobby_collectibles <address> <collectibles>\n");
        return;
    }
    G_SP_SetPlayerCollectibles(SV_Cmd_Argv(1), SV_Cmd_Argv(2));
}

static cmd_function_s SV_Lobby_Seat_f_VAR;
static cmd_function_s SV_Lobby_Drop_f_VAR, SV_Lobby_Drop_f_VAR_SERVER;
static cmd_function_s SV_Lobby_DStat_f_VAR, SV_Lobby_DStat_f_VAR_SERVER;
static cmd_function_s SV_Lobby_Collectibles_f_VAR, SV_Lobby_Collectibles_f_VAR_SERVER;

void SV_Lobby_RegisterDvars()
{
    Cmd_AddCommandInternal("bo1_lobby_seat", SV_Lobby_Seat_f, &SV_Lobby_Seat_f_VAR);
    // server commands (above)
    Cmd_AddCommandInternal("bo1_lobby_drop", Cbuf_AddServerText_f, &SV_Lobby_Drop_f_VAR);
    Cmd_AddServerCommandInternal("bo1_lobby_drop", SV_Lobby_Drop_f, &SV_Lobby_Drop_f_VAR_SERVER);
    Cmd_AddCommandInternal("bo1_lobby_dstat", Cbuf_AddServerText_f, &SV_Lobby_DStat_f_VAR);
    Cmd_AddServerCommandInternal("bo1_lobby_dstat", SV_Lobby_DStat_f, &SV_Lobby_DStat_f_VAR_SERVER);
    Cmd_AddCommandInternal("bo1_lobby_collectibles", Cbuf_AddServerText_f, &SV_Lobby_Collectibles_f_VAR);
    Cmd_AddServerCommandInternal("bo1_lobby_collectibles", SV_Lobby_Collectibles_f, &SV_Lobby_Collectibles_f_VAR_SERVER);
    bo1_lobby = _Dvar_RegisterBool("bo1_lobby", false, 0,
        "A lobby runs this game for a room (zombies-hub's server/t5.go): it says who is in it, and ends at game over");
    bo1_players = _Dvar_RegisterInt("bo1_players", 0, 0, 64, 0,
        "The players round 1 waits for (getnumexpectedplayers): the room's, whom the lobby expects; 0 = whoever is connecting");
    bo1_started = _Dvar_RegisterBool("bo1_started", false, 4,
        "The room's game has begun round 1, which takes only players coming back to their character: they go straight in (src/client/cl_cin.cpp: CL_LoadingMovieOverGame)");
}

static bool SV_Lobby_Says()
{
    return bo1_lobby && bo1_lobby->current.enabled;
}

// the address as the lobby knows it: its port unsigned, unlike NET_AdrToString's
static const char *SV_Lobby_Address(const netadr_t &a)
{
    return va("%i.%i.%i.%i:%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3], (unsigned short)BigShort(a.port));
}

// the client's address, as the lobby knows it (src/game_sp/g_sp_stats.cpp keeps each player's stats by it)
const char *SV_Lobby_ClientAddress(int clientNum)
{
    return SV_Lobby_Address(svs.clients[clientNum].header.netchan.remoteAddress);
}

// The client slot a new player at the address takes, of the free ones from start to end (SV_DirectConnect), or -1 when
// none is free. In a lobby's game, it is the slot of their place in the room (bo1_lobby_seat): its host's is the first,
// and the others' follow in the order they joined, as an SP game's host was its client 0 and its other players came
// after them. The level's scripts go by it: a player's spawn point (_zombiemode.gsc: coop_player_spawn_placement, the
// n-th of the map's initial_spawn_points) and their character in co-op (_loadout.gsc: give_model; Kino's Dempsey,
// Nikolai, Takeo and Richtofen). The first free slot, which each took, made whoever's game connected first Dempsey. A
// player with no place takes the first free slot that no place holds, else the first free one.
int SV_Lobby_FreeSlot(const netadr_t &from, int start, int end)
{
    const char *address = SV_Lobby_Address(from);
    int unheld = -1, any = -1;
    for (int i = start; i < end; ++i)
    {
        if (svs.clients[i].header.state)
            continue;
        if (s_seat[i][0] && !strcmp(s_seat[i], address))
            return i;
        if (unheld < 0 && !s_seat[i][0])
            unheld = i;
        if (any < 0)
            any = i;
    }
    return unheld >= 0 ? unheld : any;
}

void SV_Lobby_StatChanged(const char *address, const char *name, int value)
{
    if (SV_Lobby_Says())
        Com_Printf(15, "bo1_lobby: dstat %s %s %i\n", address, name, value);
}

void SV_Lobby_CollectiblesChanged(const char *address, const char *collectibles)
{
    if (SV_Lobby_Says())
        Com_Printf(15, "bo1_lobby: collectibles %s %s\n", address, collectibles);
}

// The players a level waits for (getnumexpectedplayers, src/game_sp/g_scr_sp_players.cpp): those connected or
// connecting, and at least bo1_players. SP counts its party's members; a dedicated server has none, so without
// bo1_players round 1 starts as soon as the first player is in.
int SV_Lobby_ExpectedPlayers()
{
    int count = 0;
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
    {
        if (svs.clients[i].header.state > CS_ZOMBIE)
            ++count;
    }
    if (bo1_players && count < bo1_players->current.integer)
        count = bo1_players->current.integer;
    return count;
}

// The level's scripts are loaded: the fields that hold its round (level.round_number), which game over says, and what
// each player earned and has left (score_total, score_unspent), which the report does. Compiler identifiers are
// canonical IDs, whose mapping Scr_EndLoadScripts releases after this.
void SV_Lobby_ScriptFields(scriptInstance_t inst)
{
    if (inst != SCRIPTINSTANCE_SERVER)
        return; // a client's scripts: the server's key stays
    const auto key = [inst](const char *name) {
        const unsigned int string = SL_FindString(name, inst);
        return string ? gScrCompilePub[inst].canonicalStrings[string] : 0u;
    };
    s_roundNumberKey = key("round_number");
    s_scoreTotalKey = key("score_total");
    s_scoreUnspentKey = key("score_unspent");
    s_intermission = SL_FindString("intermission", inst);
    s_stopIntermission = SL_FindString("stop_intermission", inst);
}

// The level notifies itself (VM_Notify). At game over its end scoreboard begins 3 s after "Game Over" shows
// (_zombiemode.gsc end_game: intermission(), whose "intermission" puts each player in it, as Moon's own does): the next
// server frame says it is over (SV_Lobby_Frame). 15 s on, its intermission stops ("stop_intermission"), and the level
// fades its players' screens to black, holds it, and exits 2.5 s on (ExitLevel): their sound fades out with the
// picture, where the world around the intermission's camera, then around the player as the level gave them the view
// back, played on until their game ended. The SP game's level exit faded its clients' sound with the same command, as
// missionfailed does ('q': cgame/cg_sp_servercmds.cpp, SND_FadeOut, 2 s), but only as it exited.
void SV_Lobby_LevelNotify(unsigned int stringValue)
{
    if (!SV_Lobby_Says())
        return;
    if (s_intermission && stringValue == s_intermission)
        s_endScoreboard = true;
    if (s_stopIntermission && stringValue == s_stopIntermission)
        SV_GameSendServerCommand(-1, SV_CMD_RELIABLE, "q 0 0");
}

void SV_Lobby_MapLoaded()
{
    s_started = false;
    s_roundSaid = 0;
    Dvar_SetBool((dvar_s *)bo1_started, false);
    s_shuttingDown = false;
    s_endScoreboard = s_overSaid = false;
    s_over = s_overQuits = false;
    memset(s_quit, 0, sizeof(s_quit));
    memset(s_doors, 0, sizeof(s_doors));
}

// The server shuts down (SV_Shutdown: it quits, or an error unloaded the map), and drops its clients on the way: the
// lobby hears no "out" for them, which would say each player's character is gone, as if they had all left or timed
// out. The game is ending anyway, which the lobby hears as it quits.
void SV_Lobby_ShuttingDown()
{
    s_shuttingDown = true;
}

// Whether the player in the client slot is in the game, for game over to wait for: connected, not having quit it
// (SV_Disconnect_f: they left, the end scoreboard among the rest), and heard from in the last 30 s. No one is at the
// others.
static bool SV_Lobby_InGame(int i)
{
    const client_t *cl = &svs.clients[i];
    return cl->header.state >= CS_CONNECTED && !s_quit[i] && svs.time - cl->lastPacketTime < 30000;
}

// Game over, on a dedicated server (SV_Lobby_GameOver): each second, the game tells its players again, until those it
// waits for have heard (their game ends, and says disconnect, which drops them: SV_Lobby_KeepsCharacters), or for 30 s,
// and then quits. Told once, as the server's own quit tells them (SV_FinalMessage), a player whose game was busy for a
// few seconds (a Windows game compiling shaders) missed it: what came meanwhile filled its socket's buffer, which drops
// the newest, and with the server gone, its game ended a minute later as one that had lost the connection. It waits
// for those it heard from in the 30 s before who had not quit the game: no one is at the others.
static void SV_Lobby_TellOver()
{
    const bool tell = svs.time - s_overToldAt >= 1000;
    int waiting = 0;
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
    {
        client_t *cl = &svs.clients[i];
        if (cl->header.state < CS_CONNECTED)
            continue;
        if (tell)
            SV_FinalMessageTo(cl, s_finalMessage);
        if (s_toHear[i])
            ++waiting;
    }
    if (tell)
        s_overToldAt = svs.time;
    if (s_overQuits || (waiting && svs.time - s_overAt < 30000))
        return;
    if (waiting)
        Com_Printf(15, "Game over: quitting, %i of the players not having heard it in 30 s\n", waiting);
    s_overQuits = true;
    Cbuf_AddText(0, "quit\n");
}

// A script object's integer field, or fallback when it keeps none.
static int SV_Lobby_IntField(unsigned int object, unsigned int key, int fallback = 0)
{
    const unsigned int field = object && key ? FindVariable(SCRIPTINSTANCE_SERVER, object, key) : 0;
    return field && GetValueType(SCRIPTINSTANCE_SERVER, field) == VAR_INTEGER
        ? GetVariableValueAddress(SCRIPTINSTANCE_SERVER, field)->u.intValue : fallback;
}

// The level's round (level.round_number), or 0 when it keeps none.
static int SV_Lobby_LevelRound()
{
    return SV_Lobby_IntField(gScrVarPub[SCRIPTINSTANCE_SERVER].levelId, s_roundNumberKey);
}

static void SV_Lobby_Stats(bool roundBegins);

// Game over, said once: each player's line of the scoreboard, and the round, for the room's report (zombies-hub's
// server/t5.go: gameLine), which shows from then on to those back in the room. As the end scoreboard begins, its
// players' points are their total, and what they had left the level keeps apart (SV_Lobby_Stats).
static void SV_Lobby_SayOver()
{
    if (s_overSaid)
        return;
    s_overSaid = true;
    SV_Lobby_Stats(false);
    if (const int round = SV_Lobby_LevelRound())
        Com_Printf(15, "bo1_lobby: over %i\n", round);
    else
        Com_Printf(15, "bo1_lobby: over\n");
}

// A dedicated server's level has gone on to a new round (_zombiemode.gsc round_think: level.round_number, up by one as
// the last one ends): the lobby hears it, with each player's line of the scoreboard, for a game that ends some other
// way than at game over (its players quit, a crash, the server stopping), which says neither. A map alone has no lobby
// to tell.
static void SV_Lobby_NewRound()
{
    if (!IsDedicatedServer() || s_shuttingDown)
        return;
    const int round = SV_Lobby_LevelRound();
    if (round <= s_roundSaid)
        return;
    s_roundSaid = round;
    SV_Lobby_Stats(true);
    Com_Printf(15, "bo1_lobby: round %i\n", round);
}

// Each server frame: once every player expected is in, as getnumconnectedplayers counts them (in the game, and having
// had a snapshot), the level's scripts start round 1 (_load_common's all_players_connected), and the lobby is told, and
// the clients, by the serverinfo's bo1_started: a player coming now, back to their character, has no briefing to wait
// in for the others, and their client takes them straight in (src/client/cl_cin.cpp: CL_LoadingMovieOverGame).
// From then on, it says each round as it begins (SV_Lobby_NewRound). As game over's end scoreboard begins, it says
// it is over (SV_Lobby_SayOver), and a dedicated server's game ends as soon as every player has left it (client_mp/
// cl_main_mp.cpp: CL_LeaveGameOver), rather than run on the 18 s to the level's exit, so that the room's next game can
// start. At game over, it tells its players (above).
void SV_Lobby_Frame()
{
    if (!SV_Lobby_Says() || !svs.clients)
        return;
    if (s_over)
    {
        SV_Lobby_TellOver();
        return;
    }
    if (s_endScoreboard)
    {
        SV_Lobby_SayOver();
        if (!IsDedicatedServer())
            return; // a map alone: its player's engine ends with the game
        for (int i = 0; i < sv_maxclients->current.integer; ++i)
        {
            if (SV_Lobby_InGame(i))
                return;
        }
        Com_Printf(15, "Game over: everyone has left the end scoreboard\n");
        SV_Lobby_GameOver();
        return;
    }
    if (s_started)
    {
        SV_Lobby_NewRound();
        return;
    }
    int connected = 0;
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
    {
        if (svs.clients[i].header.state == CS_ACTIVE && svs.clients[i].connectState == 10 /*CA_ACTIVE*/)
            ++connected;
    }
    if (connected == 0 || connected < SV_Lobby_ExpectedPlayers())
        return;
    s_started = true;
    Dvar_SetBool((dvar_s *)bo1_started, true);
    Com_Printf(15, "bo1_lobby: start\n");
}

void SV_Lobby_ClientEntered(const client_t *cl)
{
    s_quit[cl - svs.clients] = false;
    if (SV_Lobby_Says())
        Com_Printf(15, "bo1_lobby: in %s\n", SV_Lobby_Address(cl->header.netchan.remoteAddress));
}

void SV_Lobby_ClientLeft(const client_t *cl)
{
    s_doors[cl - svs.clients] = 0; // their character's, which goes with them
    if (SV_Lobby_Says() && !s_shuttingDown && !s_over)
        Com_Printf(15, "bo1_lobby: out %s\n", SV_Lobby_Address(cl->header.netchan.remoteAddress));
}

void SV_Lobby_ClientQuit(const client_t *cl)
{
    if (s_over)
        return; // it heard it was game over (SV_Lobby_TellOver)
    s_quit[cl - svs.clients] = true;
    if (SV_Lobby_Says() && !s_shuttingDown)
        Com_Printf(15, "bo1_lobby: quit %s\n", SV_Lobby_Address(cl->header.netchan.remoteAddress));
}

// Each player's line of the scoreboard as a round begins and at game over, for the report the lobby shows
// (zombies-hub's server/rooms.go: gameResult): the fields the level's scripts keep it in
// (src/game/g_client_fields.cpp): the points they earned (score_total, a field of the scripts' own), what's left of
// them to spend (score, or score_unspent once the level's intermission has put the total in score for its end
// scoreboard), kills, headshots (a scoreboard column's, which the game keeps whether or not it shows the column), downs
// and revives (SP's, which MP lacks: gclientSpExt), and the doors they bought, which no scoreboard has
// (SV_Lobby_BlackBox). A player whose character waits for them, as they dropped, has one too.
// As a round begins, the game may yet end before game over: that is everyone going down at once, whatever they were
// doing, so each player has that final down in this line, which the next line, or game over's, replaces.
static void SV_Lobby_Stats(bool roundBegins)
{
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
    {
        const client_t *cl = &svs.clients[i];
        if (cl->header.state <= CS_ZOMBIE || !cl->gentity || !cl->gentity->client)
            continue;
        const gclient_s *client = cl->gentity->client;
        const gclientSpExt &sp = G_ClientSpExt(client);
        const unsigned int player = FindEntityId(SCRIPTINSTANCE_SERVER, i, 0, 0);
        Com_Printf(15, "bo1_lobby: stats %s %i %i %i %i %i %i %i\n", SV_Lobby_Address(cl->header.netchan.remoteAddress),
            SV_Lobby_IntField(player, s_scoreTotalKey),
            SV_Lobby_IntField(player, s_scoreUnspentKey, client->sess.cs.score.score), client->sess.cs.score.kills,
            client->sess.scoreboardColumnCache[SB_TYPE_HEADSHOTS], sp.downs + (roundBegins ? 1 : 0), sp.revives,
            s_doors[i]);
    }
}

// The level's scripts tell their black box what a player bought (bbprint, src/game_mp/g_scr_main_mp.cpp: a format and
// what fills it, the buyer's name first), and this game has no black box. Of that, a lobby's game counts the doors, for
// its report (SV_Lobby_Stats), which nothing else keeps: a "zombie_uses" of type door (_zombiemode_blockers.gsc: a
// door, debris, a shutter; Moon's airlocks) or debris (Call of the Dead's stairs), said once its buyer has paid. A door
// that opens some other way says nothing there: the power's, a QED's, the zombie_unlock_all dvar's. The buyer is the
// player of that name (playername: ClientScr_GetName), which the lobby gives one player of the room alone
// (zombies-hub's server/rooms.go: uniqueName).
void SV_Lobby_BlackBox(const char *format, const char *playerName)
{
    static const char uses[] = "zombie_uses: playername %s ";
    if (!SV_Lobby_Says() || !svs.clients || strncmp(format, uses, sizeof(uses) - 1))
        return;
    const char *type = strrchr(format, ' ');
    if (strcmp(type, " door") && strcmp(type, " debris"))
        return;
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
    {
        const client_t *cl = &svs.clients[i];
        if (cl->header.state <= CS_ZOMBIE || !cl->gentity || !cl->gentity->client)
            continue;
        if (!strcmp(CS_DisplayName(&cl->gentity->client->sess.cs, 3), playerName))
        {
            ++s_doors[i];
            return;
        }
    }
}

// Game over: the level's end_game starts the level again, which comes to SP_LoadGameContinue_f (missionfailed's
// loadgame_continue, map_restart's fast_restart), and a lobby's game quits instead, and its players' games end, saying
// it was game over. A map alone in a browser, whose server is in the player's own engine, ends that engine's game as
// a room's game's players' end, at the error their server's disconnect is (CL_GameEnded: why "over", after Call of the
// Dead's outro, client/cl_cin.cpp). The console's loadgame_continue ends it the same way, and so does its fast_restart
// in a room's game; alone, a fast_restart before game over starts the map again (SV_Lobby_Alone). True when it does.
bool SV_Lobby_GameOver()
{
    if (!SV_Lobby_Says())
        return false;
    if (s_over)
        return true; // said, and the players are being told
    SV_Lobby_SayOver(); // unless its end scoreboard did
    s_finalMessage = "BO1_GAMEOVER";
    if ( !IsDedicatedServer() )
        Com_Error(ERR_DISCONNECT, s_finalMessage);
    // the server's frames tell the players, from the next (SV_Lobby_TellOver)
    s_over = true;
    s_overAt = svs.time;
    s_overToldAt = svs.time - 1000;
    for (int i = 0; i < sv_maxclients->current.integer; ++i)
        s_toHear[i] = SV_Lobby_InGame(i);
    return true;
}

// A map alone in a browser (Play offline): a lobby's game whose server is the player's own engine. Its pause menu's
// Restart Level starts the map again, as SP's did (src/game_sp/g_sp_loadgame.cpp: SP_FastRestart); a room's players
// have it greyed out, their games online ones (src/client_mp/cl_parse_mp.cpp: CL_ParseGamestate).
bool SV_Lobby_Alone()
{
    return SV_Lobby_Says() && !IsDedicatedServer();
}

// The letters and digits a player's name needs at least (SV_SanitizeName, SV_ValidateName): 4, as in BO1, but 1 in a
// lobby's game, where the lobby names the players (zombies-hub's server/rooms.go: cleanName), each its own in the room.
int SV_Lobby_MinNameLength()
{
    return SV_Lobby_Says() ? 1 : 4;
}

// A player keeps the name they connected with (SV_UpdateUserinfo_f): in a lobby's game, the lobby's, which the relay
// puts in their connect (zombies-hub's server/relay.go: named) and their room shows. Their client's later userinfo
// names whom it likes (its name dvar, which its player can set).
bool SV_Lobby_KeepsNames()
{
    return SV_Lobby_Says();
}

// A player back at the address they had in the game takes their character back (src/client_mp/sv_client_mp.cpp:
// SV_ResumeClient): in a lobby's game, where the relay gives each player in a room the same address and port at every
// visit (zombies-hub's server/relay.go: a seat), so an address is a player.
bool SV_Lobby_TakesPlayersBack()
{
    return SV_Lobby_Says();
}

// A lobby's game keeps the character of a player who has stopped answering (they dropped, or quit the game:
// SV_Disconnect_f) for sv_timeout, which the lobby sets (zombies-hub's server/rooms.go: keepCharacter), for them to
// take back; and as long while they take it back, though they are not yet in the world (SV_CheckTimeouts). It lets it
// go after that, or when they leave the room (bo1_lobby_drop). Once it is game over, none: a player's disconnect then
// says they heard it.
bool SV_Lobby_KeepsCharacters()
{
    return SV_Lobby_Says() && !s_over;
}

// A lobby's game takes a new player until round 1 begins, which waits for the room's players: after that, only one
// coming back to their character (SV_DirectConnect).
bool SV_Lobby_TakesNewPlayers()
{
    return !SV_Lobby_Says() || !s_started;
}

// Whether the game kicks a player who stands idle for g_inactivity (ClientInactivityTimer, src/game_mp/g_active_mp.cpp;
// the game's config sets 2 minutes): not a lobby's, where the kick would take their character for good. A player who is
// away has sv_timeout to come back (SV_Lobby_KeepsCharacters); one idle in the game is left to the zombies.
bool SV_Lobby_KicksIdlePlayers()
{
    return !SV_Lobby_Says();
}

// what the game tells its players as it quits (Com_Quit_f): BO1_GAMEOVER at game over, else message
const char *SV_Lobby_FinalMessage(const char *message)
{
    return s_finalMessage ? s_finalMessage : message;
}
