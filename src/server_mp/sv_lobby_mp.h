#pragma once

#include <clientscript/cscr_variable.h>
#include <qcommon/net_chan_mp.h>

struct client_t;
struct dvar_s;

// zombies: a room's game, which the lobby runs (zombies-hub's server/rooms.go, +set bo1_lobby 1): sv_lobby_mp.cpp
extern const dvar_s *bo1_lobby;
extern const dvar_s *bo1_players;

void SV_Lobby_RegisterDvars();
int SV_Lobby_ExpectedPlayers();
void SV_Lobby_ScriptFields(scriptInstance_t inst);
void SV_Lobby_LevelNotify(unsigned int stringValue);
void SV_Lobby_MapLoaded();
void SV_Lobby_ShuttingDown();
void SV_Lobby_Frame();
void SV_Lobby_ClientEntered(const client_t *cl);
void SV_Lobby_ClientLeft(const client_t *cl);
void SV_Lobby_ClientQuit(const client_t *cl);
bool SV_Lobby_GameOver();
bool SV_Lobby_Alone();
const char *SV_Lobby_FinalMessage(const char *message);
int SV_Lobby_MinNameLength();
bool SV_Lobby_KeepsNames();
bool SV_Lobby_TakesPlayersBack();
bool SV_Lobby_KeepsCharacters();
bool SV_Lobby_TakesNewPlayers();
bool SV_Lobby_KicksIdlePlayers();
const char *SV_Lobby_ClientAddress(int clientNum);
int SV_Lobby_FreeSlot(const netadr_t &from, int start, int end);
void SV_Lobby_StatChanged(const char *address, const char *name, int value);
void SV_Lobby_CollectiblesChanged(const char *address, const char *collectibles);
void SV_Lobby_BlackBox(const char *format, const char *playerName);
