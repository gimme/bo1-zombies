#pragma once
// zombies: the players' zombie stats and collectibles (g_sp_stats.cpp), which an SP level's getdstat / setdstat and
// hascollectible read, kept by the player's address.
struct scr_entref_t;

void G_SP_GetDStat(scr_entref_t entref);
void G_SP_SetDStat(scr_entref_t entref);
// what the lobby kept of the player at the address, as the game starts (src/server_mp/sv_lobby_mp.cpp)
void G_SP_SetPlayerStat(const char *address, const char *name, int value);
void G_SP_SetPlayerCollectibles(const char *address, const char *collectibles);
// the client set collectible n, 1 to 49 (ClientCommand "setcollectible")
void G_SP_SetCollectible(unsigned int clientNum, int n);
// hascollectible on a dedicated server: whether a player in the game has collectible n
bool G_SP_PlayersHaveCollectible(int n);
