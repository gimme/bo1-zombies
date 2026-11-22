#pragma once

struct gclient_s;

// zombies: the bits of SP's ps->pm_flags that KB's MP pm_flags has other meanings for (0x400000 is MP's dive, say),
// kept beside the playerState: SPPlayerBuiltinState::spPmFlags on the server, CgSPClientExt::spPmFlags on the
// player's own client, pmove_t::spPmFlags in both pmoves.
enum SPPmFlags
{
    SP_LOWREADY = 0x200000, // SetLowReady( true ) (SP 0x007DA1E0)
    SP_DISABLE_STAND = 0x400000,
    SP_DISABLE_CROUCH = 0x800000,
    SP_DISABLE_PRONE = 0x1000000,
    SP_DISABLE_LEAN = 0x2000000,
    SP_DISABLE_MELEE = 0x8000000,
    // what the player's pmove reads of them, on the server and in the client's prediction: the lowered weapon
    // (SP 0x00767CD0, 0x00769CD6, 0x004CB47C), the stances (SP 0x00761D60), the lean (SP 0x005F9DBD) and melee
    // (SP 0x0076A2F5)
    SP_PMF_PMOVE = SP_LOWREADY | SP_DISABLE_STAND | SP_DISABLE_CROUCH | SP_DISABLE_PRONE | SP_DISABLE_LEAN
        | SP_DISABLE_MELEE
};

struct SPPlayerBuiltinState
{
    unsigned int spPmFlags; // SPPmFlags
    bool healthShield;
    unsigned int perks; // SP playerState +0x4fc, two bits per perk.
    bool weaponChangeRaising; // SP gclient +0x1d0e: a raise was seen, weapon_change_complete pending.
};

SPPlayerBuiltinState &G_SP_PlayerBuiltinState(unsigned int clientNum);
// zombies: the player's SPPmFlags. SP's clients have them in the playerState's pm_flags; here the server tells
// the player's own client as they change, and as it takes their character back (server command 'p' <bits>:
// cgame/cg_sp_servercmds.cpp), so that it predicts as the server moves them.
void G_SP_SetPmFlags(unsigned int clientNum, unsigned int spPmFlags);
void G_SP_SendPmFlags(unsigned int clientNum);
void G_SP_ClearPlayerBuiltinState(gclient_s *client);
void G_SP_ClearLevelBuiltinState();
void G_SP_ResetMissionFailed();
struct gentity_s;
enum hitLocation_t : __int32;
bool G_SP_ShouldEnterLastStand();
void G_SP_PlayerEnterLastStand(struct gentity_s *ent, struct gentity_s *inflictor, struct gentity_s *attacker, int damage,
    unsigned int meansOfDeath, unsigned int weapon, float *dir, hitLocation_t hitLoc, int timeOffset);
bool G_SP_GrenadeSuicideDisabled();
void G_SP_SetGrenadeSuicideDisabled(bool disabled);
void G_SP_DisableGrenadeSuicide();
bool G_SP_HasPerk(gclient_s *client, const char *name);
void G_SP_SetPerk(gclient_s *client, const char *name, bool enabled);
void G_SP_GetPerks(gclient_s *client);
