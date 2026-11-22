// zombies: the players' zombie stats, which the level's scripts keep with getdstat / setdstat "PlayerStatsList" <name>
// (_zombiemode.gsc: zombieStatGet / zombieStatSet): each map's best wave, its time and best points in a game, and
// career totals (kills, points, rounds, downs, revives, perks, headshots, gibs), which the game over of a game of two
// or more players adds to, and side quests done. No menu shows them. SP's getdstat and setdstat (SP 0x00805B50 /
// 0x00805D80) read and wrote the server's copy of each player's stats (ddl/stats.ddl), which their profile kept
// between games.
// And their collectibles: SP kept them in each player's profile (bg_collectibles), which their client's scripts set
// (setcollectible: Call of the Dead's side quest) and the host's server read (hascollectible: whether the one playing
// alone has done it before). A dedicated server has no profile of its own: a client of one tells it of each it sets
// (src/cgame/cg_scr_sp_client.cpp: CScr_SetCollectible), and it reads its players'.
// The game keeps them by the player's address, which is the player in a lobby's game (SV_Lobby_TakesPlayersBack). A
// room's game has each player's from the lobby as it starts, and tells it each change (src/server_mp/sv_lobby_mp.cpp):
// the lobby keeps them while it knows the player (zombies-hub's server/rooms.go). Elsewhere a player's start at 0.
#include "g_sp_stats.h"
#include <clientscript/cscr_vm.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_scr_main_mp.h>
#include <server_mp/sv_lobby_mp.h>
#include <universal/q_shared.h>
#include <cstring>

struct SPPlayerStat
{
    char name[64];
    int value;
};

struct SPPlayer
{
    char address[32];
    char collectibles[50]; // as bg_collectibles: '1' at n - 1 for collectible n, of 49
    unsigned int statCount;
    SPPlayerStat stats[96]; // the level's scripts name about 45
};

static SPPlayer s_players[64]; // a server that has seen more addresses keeps nothing for the rest
static unsigned int s_playerCount;

// The player at the address, or null; with create, a new one, with nothing kept.
static SPPlayer *G_SP_Player(const char *address, bool create)
{
    for ( unsigned int i = 0; i < s_playerCount; ++i )
    {
        if ( !strcmp(s_players[i].address, address) )
            return &s_players[i];
    }
    if ( !create || s_playerCount >= ARRAY_COUNT(s_players) )
        return nullptr;
    SPPlayer *player = &s_players[s_playerCount++];
    I_strncpyz(player->address, address, sizeof(player->address));
    memset(player->collectibles, '0', sizeof(player->collectibles) - 1);
    player->collectibles[sizeof(player->collectibles) - 1] = 0;
    player->statCount = 0;
    return player;
}

// The player's stat, or null; with create, a new one at 0, unless they have as many as are kept.
static SPPlayerStat *G_SP_Stat(SPPlayer *player, const char *name, bool create)
{
    for ( unsigned int i = 0; i < player->statCount; ++i )
    {
        if ( !I_stricmp(player->stats[i].name, name) )
            return &player->stats[i];
    }
    if ( !create || player->statCount >= ARRAY_COUNT(player->stats) )
        return nullptr;
    SPPlayerStat *stat = &player->stats[player->statCount++];
    I_strncpyz(stat->name, name, sizeof(stat->name));
    stat->value = 0;
    return stat;
}

// The player a call is on (self, "PlayerStatsList", <name>), and the stat's name, or null when it names another.
static SPPlayer *G_SP_ScriptPlayer(scr_entref_t entref, const char *function, int numPath, bool create,
    const char **name)
{
    gentity_s *ent = GetEntity(entref);
    if ( !ent->client )
    {
        Scr_Error(va("%s: entity must be a player entity", function), 0);
        return nullptr;
    }
    if ( Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != (unsigned int)numPath
        || Scr_GetType(0, SCRIPTINSTANCE_SERVER) != VAR_STRING || Scr_GetType(1, SCRIPTINSTANCE_SERVER) != VAR_STRING
        || I_stricmp(Scr_GetString(0, SCRIPTINSTANCE_SERVER), "PlayerStatsList") )
    {
        Scr_Error(va("%s: only PlayerStatsList's stats are kept", function), 0);
        return nullptr;
    }
    *name = Scr_GetString(1, SCRIPTINSTANCE_SERVER);
    return G_SP_Player(SV_Lobby_ClientAddress(ent->s.number), create);
}

void G_SP_GetDStat(scr_entref_t entref)
{
    const char *name = nullptr;
    SPPlayer *player = G_SP_ScriptPlayer(entref, "getdstat", 2, false, &name);
    const SPPlayerStat *stat = player ? G_SP_Stat(player, name, false) : nullptr;
    Scr_AddInt(stat ? stat->value : 0, SCRIPTINSTANCE_SERVER);
}

void G_SP_SetDStat(scr_entref_t entref)
{
    const int value = Scr_GetInt(2, SCRIPTINSTANCE_SERVER);
    const char *name = nullptr;
    SPPlayer *player = G_SP_ScriptPlayer(entref, "setdstat", 3, true, &name);
    if ( !player )
        return;
    SPPlayerStat *stat = G_SP_Stat(player, name, true);
    if ( !stat )
    {
        Scr_Error("setdstat: too many stats", 0);
        return;
    }
    if ( stat->value == value )
        return;
    stat->value = value;
    SV_Lobby_StatChanged(player->address, stat->name, value);
}

void G_SP_SetPlayerStat(const char *address, const char *name, int value)
{
    SPPlayer *player = G_SP_Player(address, true);
    if ( SPPlayerStat *stat = player ? G_SP_Stat(player, name, true) : nullptr )
        stat->value = value;
}

void G_SP_SetPlayerCollectibles(const char *address, const char *collectibles)
{
    SPPlayer *player = G_SP_Player(address, true);
    for ( unsigned int i = 0; player && i < sizeof(player->collectibles) - 1 && collectibles[i]; ++i )
        player->collectibles[i] = collectibles[i] == '1' ? '1' : '0';
}

void G_SP_SetCollectible(unsigned int clientNum, int n)
{
    SPPlayer *player = G_SP_Player(SV_Lobby_ClientAddress(clientNum), true);
    if ( !player || n < 1 || n > (int)sizeof(player->collectibles) - 1 || player->collectibles[n - 1] == '1' )
        return;
    player->collectibles[n - 1] = '1';
    SV_Lobby_CollectiblesChanged(player->address, player->collectibles);
}

bool G_SP_PlayersHaveCollectible(int n)
{
    for ( int i = 0; i < level.maxclients; ++i )
    {
        if ( level.clients[i].sess.connected != CON_CONNECTED )
            continue;
        const SPPlayer *player = G_SP_Player(SV_Lobby_ClientAddress(i), false);
        if ( player && n >= 1 && n <= (int)sizeof(player->collectibles) - 1 && player->collectibles[n - 1] == '1' )
            return true;
    }
    return false;
}
