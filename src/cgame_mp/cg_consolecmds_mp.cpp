#include "cg_consolecmds_mp.h"
#include "cg_local_mp.h"
#include "cg_scoreboard_mp.h"
#include <ui_mp/ui_main_mp.h>
#include <bgame/bg_misc.h>
#include "cg_main_mp.h"
#include <gfx_d3d/r_dvars.h>
#include <client/cl_main.h>
#include <cgame/cg_sp_hud.h>
#include <bgame/bg_mantle.h>
#include <bgame/bg_weapons_ammo.h>
#include <bgame/bg_weapons_def.h>
#include <client_mp/cl_cgame_mp.h> // zombies: bo1_player
#include <client_mp/cl_input_mp.h>
#include <aim_assist/aim_assist.h> // zombies: bo1_player
#include "cg_anim_commands.h" // zombies: bo1_player

void __cdecl CG_ScoresUp(int localClientNum)
{
    cg_s *cgameGlob; // eax

    if ( CG_IsScoreboardDisplayed(localClientNum) )
    {
        cgameGlob = CG_GetLocalClientGlobals(localClientNum);
        cgameGlob->showScores = 0;
        cgameGlob->scoresTop = -1;
        cgameGlob->scoreFadeTime = cgameGlob->time;
    }
}

cmd_function_s CG_Viewpos_f_VAR;
cmd_function_s CG_ScoresDown_f_VAR;
cmd_function_s CG_ScoresUp_f_VAR;
cmd_function_s CG_NextWeapon_f_VAR;
cmd_function_s CG_PrevWeapon_f_VAR;
cmd_function_s CG_ActionSlotDown_f_VAR;
cmd_function_s CG_ActionSlotUp_f_VAR;
cmd_function_s CG_ShellShock_f_VAR;
cmd_function_s CG_ShellShock_Load_f_VAR;
cmd_function_s CG_ShellShock_Save_f_VAR;
cmd_function_s CG_MovieCamera_f_VAR;
cmd_function_s CG_FollowCamera_f_VAR;
cmd_function_s CG_QuickMessage_f_VAR;
cmd_function_s CG_VoiceChat_f_VAR;
cmd_function_s CG_TeamVoiceChat_f_VAR;
cmd_function_s CG_UpdateVehicleBindings_f_VAR;
cmd_function_s CG_FxSetTestPosition_VAR;
cmd_function_s CG_FxTest_VAR;
cmd_function_s CG_RestartSmokeGrenades_f_VAR;
cmd_function_s UpdateFilmTweaks_f_VAR;
cmd_function_s UpdateCharPrimaryTweaks_f_VAR;
cmd_function_s UpdateBloomTweaks_f_VAR;
cmd_function_s CG_ToggleBandwidthProfiling_f_VAR;
cmd_function_s mr_VAR;
cmd_function_s kill_VAR;
cmd_function_s give_VAR;
cmd_function_s bo1_models_VAR, bo1_viewmodels_VAR, bo1_viewmodel_VAR; // mod: the server's (g_cmds_mp.cpp)
cmd_function_s take_VAR;
cmd_function_s god_VAR;
cmd_function_s demigod_VAR;
cmd_function_s notarget_VAR;
cmd_function_s noclip_VAR;
cmd_function_s ufo_VAR;
cmd_function_s levelshot_VAR;
cmd_function_s setviewpos_VAR;
cmd_function_s jumptonode_VAR;
cmd_function_s stats_VAR;
cmd_function_s say_VAR;
cmd_function_s say_team_VAR;
cmd_function_s team_VAR;
cmd_function_s follow_VAR;
cmd_function_s callvote_VAR;
cmd_function_s vote_VAR;
cmd_function_s follownext_VAR;
cmd_function_s followprev_VAR;
cmd_function_s printentities_VAR;
cmd_function_s muteplayer_VAR;
cmd_function_s unmuteplayer_VAR;
cmd_function_s VisionSetNaked_VAR;
cmd_function_s VisionSetNight_VAR;
cmd_function_s CG_SP_Fade_f_VAR; // zombies (L4)
cmd_function_s CG_SP_Silence_f_VAR; // zombies (L4)
cmd_function_s CG_Bo1Player_f_VAR; // zombies
cmd_function_s CG_Bo1Weapons_f_VAR; // mod
cmd_function_s CG_Bo1FxAll_f_VAR; // mod

void __cdecl CG_InitConsoleCommands()
{
    Cmd_AddCommandInternal("viewpos", CG_Viewpos_f, &CG_Viewpos_f_VAR);
    Cmd_AddCommandInternal("bo1_player", CG_Bo1Player_f, &CG_Bo1Player_f_VAR); // zombies
    Cmd_AddCommandInternal("bo1_weapons", CG_Bo1Weapons_f, &CG_Bo1Weapons_f_VAR); // mod
    Cmd_AddCommandInternal("bo1_fxall", CG_Bo1FxAll_f, &CG_Bo1FxAll_f_VAR); // mod
    Cmd_AddCommandInternal("+scores", CG_ScoresDown_f, &CG_ScoresDown_f_VAR);
    Cmd_AddCommandInternal("-scores", CG_ScoresUp_f, &CG_ScoresUp_f_VAR);
    Cmd_AddCommandInternal("weapnext", CG_NextWeapon_f, &CG_NextWeapon_f_VAR);
    Cmd_AddCommandInternal("weapprev", CG_PrevWeapon_f, &CG_PrevWeapon_f_VAR);
    Cmd_AddCommandInternal("+actionslot", CG_ActionSlotDown_f, &CG_ActionSlotDown_f_VAR);
    Cmd_AddCommandInternal("-actionslot", CG_ActionSlotUp_f, &CG_ActionSlotUp_f_VAR);
    Cmd_AddCommandInternal("cg_shellshock", CG_ShellShock_f, &CG_ShellShock_f_VAR);
    Cmd_AddCommandInternal("cg_shellshock_load", CG_ShellShock_Load_f, &CG_ShellShock_Load_f_VAR);
    Cmd_AddCommandInternal("cg_shellshock_save", CG_ShellShock_Save_f, &CG_ShellShock_Save_f_VAR);
    Cmd_AddCommandInternal("cg_moviecamera", CG_MovieCamera_f, &CG_MovieCamera_f_VAR);
    Cmd_AddCommandInternal("cg_followcamera", CG_FollowCamera_f, &CG_FollowCamera_f_VAR);
    Cmd_AddCommandInternal("mp_QuickMessage", CG_QuickMessage_f, &CG_QuickMessage_f_VAR);
    Cmd_AddCommandInternal("VoiceChat", CG_VoiceChat_f, &CG_VoiceChat_f_VAR);
    Cmd_AddCommandInternal("VoiceTeamChat", CG_TeamVoiceChat_f, &CG_TeamVoiceChat_f_VAR);
    Cmd_AddCommandInternal("updatevehiclebindings", CG_UpdateVehicleBindings_f, &CG_UpdateVehicleBindings_f_VAR);
    Cmd_AddCommandInternal("fxSetTestPosition", CG_FxSetTestPosition, &CG_FxSetTestPosition_VAR);
    Cmd_AddCommandInternal("fxTest", CG_FxTest, &CG_FxTest_VAR);
    Cmd_AddCommandInternal("restartsmokegrenades", CG_RestartSmokeGrenades_f, &CG_RestartSmokeGrenades_f_VAR);
    Cmd_AddCommandInternal("updateFilmTweaks", UpdateFilmTweaks_f, &UpdateFilmTweaks_f_VAR);
    Cmd_AddCommandInternal("updateCharPrimaryTweaks", UpdateCharPrimaryTweaks_f, &UpdateCharPrimaryTweaks_f_VAR);
    Cmd_AddCommandInternal("updateBloomTweaks", UpdateBloomTweaks_f, &UpdateBloomTweaks_f_VAR);
    Cmd_AddCommandInternal("togglebandwidthprofile", BLOPS_NULLSUB, &CG_ToggleBandwidthProfiling_f_VAR);
    Cmd_AddCommandInternal("mr", 0, &mr_VAR);
    Cmd_AddCommandInternal("kill", 0, &kill_VAR);
    Cmd_AddCommandInternal("give", 0, &give_VAR);
    Cmd_AddCommandInternal("bo1_models", 0, &bo1_models_VAR); // mod
    Cmd_AddCommandInternal("bo1_viewmodels", 0, &bo1_viewmodels_VAR); // mod
    Cmd_AddCommandInternal("bo1_viewmodel", 0, &bo1_viewmodel_VAR); // mod
    Cmd_AddCommandInternal("take", 0, &take_VAR);
    Cmd_AddCommandInternal("god", 0, &god_VAR);
    Cmd_AddCommandInternal("demigod", 0, &demigod_VAR);
    Cmd_AddCommandInternal("notarget", 0, &notarget_VAR);
    Cmd_AddCommandInternal("noclip", 0, &noclip_VAR);
    Cmd_AddCommandInternal("ufo", 0, &ufo_VAR);
    Cmd_AddCommandInternal("levelshot", 0, &levelshot_VAR);
    Cmd_AddCommandInternal("setviewpos", 0, &setviewpos_VAR);
    Cmd_AddCommandInternal("jumptonode", 0, &jumptonode_VAR);
    Cmd_AddCommandInternal("stats", 0, &stats_VAR);
    Cmd_AddCommandInternal("say", 0, &say_VAR);
    Cmd_AddCommandInternal("say_team", 0, &say_team_VAR);
    Cmd_AddCommandInternal("team", 0, &team_VAR);
    Cmd_AddCommandInternal("follow", 0, &follow_VAR);
    Cmd_AddCommandInternal("callvote", 0, &callvote_VAR);
    Cmd_AddCommandInternal("vote", 0, &vote_VAR);
    Cmd_AddCommandInternal("follownext", 0, &follownext_VAR);
    Cmd_AddCommandInternal("followprev", 0, &followprev_VAR);
    Cmd_AddCommandInternal("printentities", 0, &printentities_VAR);
    Cmd_AddCommandInternal("muteplayer", 0, &muteplayer_VAR);
    Cmd_AddCommandInternal("unmuteplayer", 0, &unmuteplayer_VAR);
    Cmd_AddCommandInternal("VisionSetNaked", 0, &VisionSetNaked_VAR);
    Cmd_AddCommandInternal("VisionSetNight", 0, &VisionSetNight_VAR);
    // zombies (L4): SP cgame commands the pause menu's Restart Level uses (SP 0x00563C5E / 0x00563C75); only with
    // the SP code zones, the MP exe has neither.
    if ( bo1_zombies && bo1_zombies->current.enabled )
    {
        Cmd_AddCommandInternal("fade", CG_SP_Fade_f, &CG_SP_Fade_f_VAR);
        Cmd_AddCommandInternal("silence", CG_SP_Silence_f, &CG_SP_Silence_f_VAR);
    }
}

void __cdecl CG_Viewpos_f()
{
    const cg_s *cgameGlob; // [esp+28h] [ebp-4h]

    cgameGlob = CG_GetLocalClientGlobals(0);
    if ( cgameGlob->nextSnap )
        Com_Printf(
            0,
            "(%.0f %.0f %.0f) : %.0f %.0f\n",
            cgameGlob->refdef.vieworg[0],
            cgameGlob->refdef.vieworg[1],
            cgameGlob->refdef.vieworg[2],
            cgameGlob->refdefViewAngles[1],
            cgameGlob->refdefViewAngles[0]);
}

// zombies: the local player as the server last sent them (the newest snapshot), on one line for tests to read
// (zombies-hub's e2e/game/player.ts: ask): movement type, where they stand, where they look (pitch, yaw), and the
// weapon in hand with its ammo; then whether the client holds forward (+forward, W) now, and the aim assist's state
// (AimAssist_Describe), the commands that asked for a knife's lunge with the last one's distance, those that pressed
// melee (CL_Lunges), the arms of the player's character (the level's SetViewModel), whether the scoreboard shows
// or fades, were the game not paused (CG_SP_ScoreboardShows), and the anim commands the client lost
// (CG_SP_AnimCommandsLost), e.g. "bo1_player: pm_type 0
// origin 1 2 3 angles 0 90 weapon m1911_zm clip 8 stock 32 forward 1 aimtargets 0 nearest 0.00 0.00 0 aimrange 1200
// autoaimrange 1000 slowdown 0 autoaim -1 closest 0.00 0.00 0 lunges 1 lungedist 40 melees 2 viewmodel
// viewmodel_usa_pow_arms scores 0 animlost 0"
void __cdecl CG_Bo1Player_f()
{
    const cg_s *cgameGlob = CG_GetLocalClientGlobals(0);
    if ( !cgameGlob->nextSnap )
    {
        Com_Printf(0, "bo1_player: none\n");
        return;
    }
    const playerState_s *ps = &cgameGlob->nextSnap->ps;
    const unsigned int weapon = ps->weapon;
    char aim[256];
    AimAssist_Describe(0, aim, sizeof(aim));
    int lungeDist = 0;
    int melees = 0;
    const int lunges = CL_Lunges(0, &lungeDist, &melees);
    Com_Printf(
        0,
        "bo1_player: pm_type %d origin %.0f %.0f %.0f angles %.0f %.0f weapon %s clip %d stock %d forward %d %s lunges %d "
        "lungedist %d melees %d viewmodel %s scores %d animlost %d\n",
        ps->pm_type,
        ps->origin[0],
        ps->origin[1],
        ps->origin[2],
        ps->viewangles[0],
        ps->viewangles[1],
        weapon ? BG_GetWeaponVariantDef(weapon)->szInternalName : "none",
        weapon ? BG_GetAmmoInClip(ps, weapon) : 0,
        weapon ? BG_GetTotalAmmoReserve(ps, weapon) : 0,
        CL_KeyHeld(0, KB_FORWARD) ? 1 : 0,
        aim,
        lunges,
        lungeDist,
        melees,
        // the model's name, as CG_UpdateHandViewmodels reads it
        ps->viewmodelIndex > 0 ? CL_GetConfigString(ps->viewmodelIndex + 1568) : "none",
        CG_SP_ScoreboardShows(0) ? 1 : 0,
        CG_SP_AnimCommandsLost(0));
}

// mod: the weapons a player of this map can hold, a line each ("bo1_weapons: weapon ray_gun_zm"), then how many
// ("bo1_weapons: 61 weapons"), for zombies-hub's docker/pipelines.sh to give each (give <name>) and draw its view
// model: those the level has registered with a view model, as give all gives them, less what a give does not put in the
// hand (an alternate mode, drawn with its weapon's model; a grenade or a mine, thrown from the offhand)
void __cdecl CG_Bo1Weapons_f()
{
    unsigned int weapIndex;
    int count = 0;

    for ( weapIndex = 1; weapIndex < BG_GetNumWeapons(); ++weapIndex )
    {
        const WeaponVariantDef *weapVariantDef = BG_GetWeaponVariantDef(weapIndex);
        if ( !weapVariantDef || !weapVariantDef->weapDef || !weapVariantDef->szInternalName )
            continue;
        const WeaponDef *weapDef = weapVariantDef->weapDef;
        if ( !weapDef->gunXModel || weapDef->inventoryType == WEAPINVENTORY_ALTMODE )
            continue;
        if ( (weapDef->weapType == WEAPTYPE_GRENADE || weapDef->weapType == WEAPTYPE_MINE)
            && weapDef->offhandClass != OFFHAND_CLASS_NONE )
        {
            continue;
        }
        Com_Printf(0, "bo1_weapons: weapon %s\n", weapVariantDef->szInternalName);
        ++count;
    }
    Com_Printf(0, "bo1_weapons: %d weapons\n", count);
}

void __cdecl CG_ScoresUp_f()
{
    if ( CG_GetLocalClientGlobals(0)->nextSnap )
    {
        CG_ScoresUp(0);
        if ( UI_GetActiveMenu(0) == UIMENU_SCOREBOARD )
            UI_SetActiveMenu(0, UIMENU_NONE);
    }
}

void __cdecl CG_ScoresDown_f()
{
    if ( CG_GetLocalClientGlobals(0)->nextSnap )
    {
        CG_ScoresDown(0);
        if ( UI_GetActiveMenu(0) != UIMENU_SCOREBOARD )
            UI_SetActiveMenu(0, UIMENU_SCOREBOARD);
    }
}

void __cdecl CG_ScoresDown(int localClientNum)
{
    CG_GetLocalClientGlobals(localClientNum)->showScores = 1;
}

void __cdecl CG_ShellShock_f()
{
    const char *v0; // eax
    const char *v1; // eax
    shellshock_parms_t *ShellshockParms; // eax
    float v3; // [esp+0h] [ebp-40h]
    int v4; // [esp+4h] [ebp-3Ch]
    float v5; // [esp+8h] [ebp-38h]
    cg_s *cgameGlob; // [esp+34h] [ebp-Ch]

    if ( CG_GetLocalClientGlobals(0)->nextSnap )
    {
        v4 = Cmd_Argc();
        if ( v4 == 2 )
        {
LABEL_5:
            v1 = Cmd_Argv(1);
            v3 = atof(v1);
            v5 = v3 * 1000.0;
            cgameGlob = CG_GetLocalClientGlobals(0);
            CG_GetLocalClientStaticGlobals(0);
            ShellshockParms = BG_GetShellshockParms(0);
            BG_SetShellShockParmsFromDvars(ShellshockParms);
            cgameGlob->testShock.time = cgameGlob->time;
            cgameGlob->testShock.duration = (int)(v5 + 9.313225746154785e-10);
            return;
        }
        if ( v4 == 3 )
        {
            v0 = Cmd_Argv(2);
            if ( !BG_LoadShellShockDvars(v0) )
                return;
            goto LABEL_5;
        }
        Com_Printf(0, "USAGE: cg_shellshock <duration> <filename?>\n");
    }
}

void __cdecl CG_ShellShock_Load_f()
{
    const char *name; // [esp+10h] [ebp-4h]

    if ( CG_GetLocalClientGlobals(0)->nextSnap )
    {
        if ( Cmd_Argc() == 2 )
        {
            name = Cmd_Argv(1);
            BG_LoadShellShockDvars(name);
        }
        else
        {
            Com_Printf(0, "USAGE: cg_shellshock_load <name>\n");
        }
    }
}

void __cdecl CG_ShellShock_Save_f()
{
    const char *name; // [esp+10h] [ebp-4h]

    if ( CG_GetLocalClientGlobals(0)->nextSnap )
    {
        if ( Cmd_Argc() == 2 )
        {
            name = Cmd_Argv(1);
            BG_SaveShellShockDvars(name);
        }
        else
        {
            Com_Printf(0, "USAGE: cg_shellshock_save <name>\n");
        }
    }
}

void __cdecl CG_MovieCamera_f()
{
    cg_s *cgameGlob; // [esp+18h] [ebp-18h]
    int value; // [esp+24h] [ebp-Ch]
    bool hud_state; // [esp+2Bh] [ebp-5h]

    hud_state = 1;
    if ( CG_CheatsOK("cg_moviecamera") )
    {
        cgameGlob = CG_GetLocalClientGlobals(0);
        if ( CG_GetLocalClientGlobals(0)->nextSnap )
        {
            if ( cgameGlob->cameraMode == 1 )
            {
                value = 0;
                if ( cg_paused->current.integer == 2 )
                    Dvar_SetInt((dvar_s *)cg_paused, 1);
            }
            else
            {
                value = 1;
                hud_state = 0;
                if ( ((*((unsigned int *)CG_GetEntity(0, cgameGlob->clientNum) + 201) >> 1) & 1) != 0 )
                {
                    cgameGlob->movieCameraOrigin[0] = cgameGlob->refdef.vieworg[0];
                    cgameGlob->movieCameraOrigin[1] = cgameGlob->refdef.vieworg[1];
                    cgameGlob->movieCameraOrigin[2] = cgameGlob->refdef.vieworg[2];
                    cgameGlob->movieCameraOrigin[2] = cgameGlob->movieCameraOrigin[2] + 10.0;
                }
                else
                {
                    cgameGlob->movieCameraOrigin[0] = cgameGlob->refdef.vieworg[0];
                    cgameGlob->movieCameraOrigin[1] = cgameGlob->refdef.vieworg[1];
                    cgameGlob->movieCameraOrigin[2] = cgameGlob->refdef.vieworg[2];
                }
            }
            cgameGlob->cameraMode = value;
            Dvar_SetBool((dvar_s *)cg_draw2D, hud_state);
        }
    }
}

char __cdecl CG_CheatsOK(const char *cmdName)
{
    if ( sv_cheats->current.enabled )
        return 1;
    Com_Printf(16, "%s is cheat protected.\n", cmdName);
    return 0;
}

void __cdecl CG_FollowCamera_f()
{
    const char *v0; // eax
    cg_s *cgameGlob; // [esp+20h] [ebp-10h]
    int clientNum; // [esp+28h] [ebp-8h]
    centity_s *centTarget; // [esp+2Ch] [ebp-4h]

    if ( CG_CheatsOK("cg_followcamera") )
    {
        cgameGlob = CG_GetLocalClientGlobals(0);
        if ( CG_GetLocalClientGlobals(0)->nextSnap )
        {
            if ( Cmd_Argc() >= 2 )
            {
                v0 = Cmd_Argv(1);
                clientNum = atoi(v0);
                if ( clientNum >= 0 )
                {
                    cgameGlob->followCameraClient = clientNum;
                    centTarget = CG_GetEntity(0, cgameGlob->followCameraClient);
                    if ( ((*((unsigned int *)centTarget + 201) >> 1) & 1) != 0 )
                    {
                        cgameGlob->movieCameraOrigin[0] = centTarget->pose.origin[0];
                        cgameGlob->movieCameraOrigin[1] = centTarget->pose.origin[1];
                        cgameGlob->movieCameraOrigin[2] = centTarget->pose.origin[2];
                        cgameGlob->cameraMode = 2;
                        Dvar_SetBool((dvar_s *)cg_draw2D, 0);
                    }
                    else
                    {
                        Com_PrintWarning(14, "Cannot find client %d to follow.\n", clientNum);
                    }
                }
                else if ( cgameGlob->cameraMode == 2 )
                {
                    cgameGlob->cameraMode = 0;
                    Dvar_SetBool((dvar_s *)cg_draw2D, 1);
                }
            }
            else
            {
                Com_Printf(14, "Usage: cg_followcamera <clientnum>\n");
            }
        }
    }
}

void __cdecl CG_QuickMessage_f()
{
    const cg_s *cgameGlob; // [esp+0h] [ebp-8h]

    cgameGlob = CG_GetLocalClientGlobals(0);
    if ( cgameGlob->nextSnap )
    {
        if ( (cgameGlob->nextSnap->ps.otherFlags & 4) != 0 )
            UI_Popup(0, "UIMENU_WM_QUICKMESSAGE");
    }
}

void __cdecl CG_VoiceChat_f()
{
    char *v0; // eax
    const char *v1; // eax
    const cg_s *cgameGlob; // [esp+10h] [ebp-Ch]
    const char *chatCmd; // [esp+18h] [ebp-4h]

    cgameGlob = CG_GetLocalClientGlobals(0);
    if ( cgameGlob->nextSnap && Cmd_Argc() == 2 )
    {
        if ( cgameGlob->nextSnap->ps.pm_type == 5 || (cgameGlob->nextSnap->ps.otherFlags & 4) != 0 )
        {
            chatCmd = Cmd_Argv(1);
            v1 = va("cmd vsay %s\n", chatCmd);
            Cbuf_AddText(0, v1);
        }
        else
        {
            v0 = UI_SafeTranslateString("CGAME_NOSPECTATORVOICECHAT");
            Com_Printf(0, "%s\n", v0);
        }
    }
}

void __cdecl CG_TeamVoiceChat_f()
{
    char *v0; // eax
    const char *v1; // eax
    const cg_s *cgameGlob; // [esp+10h] [ebp-Ch]
    const char *chatCmd; // [esp+18h] [ebp-4h]

    cgameGlob = CG_GetLocalClientGlobals(0);
    if ( cgameGlob->nextSnap && Cmd_Argc() == 2 )
    {
        if ( cgameGlob->nextSnap->ps.pm_type == 5 || (cgameGlob->nextSnap->ps.otherFlags & 4) != 0 )
        {
            chatCmd = Cmd_Argv(1);
            v1 = va("cmd vsay_team %s\n", chatCmd);
            Cbuf_AddText(0, v1);
        }
        else
        {
            v0 = UI_SafeTranslateString("CGAME_NOSPECTATORVOICECHAT");
            Com_Printf(0, "%s\n", v0);
        }
    }
}

void __cdecl CG_UpdateVehicleBindings_f()
{
    if ( CL_LocalClient_IsCUIFlagSet(0, 32) )
        CG_UpdateVehicleBindings(0);
}

bool __cdecl CL_LocalClient_IsCUIFlagSet(int localClientNum, int flag)
{
    if ( localClientNum
        && !Assert_MyHandler(
                    "c:\\projects_pc\\cod\\codsrc\\src\\live\\../client/client.h",
                    288,
                    0,
                    "%s\n\t(localClientNum) = %i",
                    "(localClientNum >= 0 && localClientNum < 1)",
                    localClientNum) )
    {
        __debugbreak();
    }
    return (flag & clientUIActives[localClientNum].flags) != 0;
}

void __cdecl CG_RestartSmokeGrenades_f()
{
    CG_RestartSmokeGrenades(0);
}

void __cdecl UpdateFilmTweaks_f()
{
    CG_VisionSetUpdateTweaksFromFile_Film();
}

void __cdecl UpdateCharPrimaryTweaks_f()
{
    CG_VisionSetUpdateTweaksFromFile_CharPrimary();
}

void __cdecl UpdateBloomTweaks_f()
{
    CG_VisionSetUpdateTweaksFromFile_Bloom();
}

void __cdecl CG_ShutdownConsoleCommands()
{
    Cmd_RemoveCommand("viewpos");
    Cmd_RemoveCommand("bo1_player"); // zombies
    Cmd_RemoveCommand("bo1_weapons"); // mod
    Cmd_RemoveCommand("bo1_fxall"); // mod
    Cmd_RemoveCommand("bo1_models"); // mod
    Cmd_RemoveCommand("bo1_viewmodels"); // mod
    Cmd_RemoveCommand("bo1_viewmodel"); // mod
    CG_Bo1FxAll_Stop(); // mod
    Cmd_RemoveCommand("fade"); // zombies (L4)
    Cmd_RemoveCommand("silence"); // zombies (L4)
    Cmd_RemoveCommand("+scores");
    Cmd_RemoveCommand("-scores");
    Cmd_RemoveCommand("weapnext");
    Cmd_RemoveCommand("weapprev");
    Cmd_RemoveCommand("+actionslot");
    Cmd_RemoveCommand("-actionslot");
    Cmd_RemoveCommand("cg_shellshock");
    Cmd_RemoveCommand("cg_shellshock_load");
    Cmd_RemoveCommand("cg_shellshock_save");
    Cmd_RemoveCommand("mp_QuickMessage");
    Cmd_RemoveCommand("VoiceChat");
    Cmd_RemoveCommand("VoiceTeamChat");
    Cmd_RemoveCommand("fxSetTestPosition");
    Cmd_RemoveCommand("fxTest");
    Cmd_RemoveCommand("restartsmokegrenades");
    Cmd_RemoveCommand("updateGlowTweaks");
    Cmd_RemoveCommand("updateFilmTweaks");
    Cmd_RemoveCommand("updateBloomTweaks");
}

