#include "actor_events.h"
#include "actor_zombie_exposed.h"
#include <game_mp/g_main_mp.h>
#include "actor_senses.h"
#include "bullet.h"
#include <game_mp/actor_mp.h>
#include "actor_event_listeners.h"
#include <game_mp/g_spawn_mp.h>
#include <clientscript/cscr_vm.h>
#include <game_mp/g_misc_mp.h>
#include <clientscript/scr_const.h>
#include <game_sp/actor_sp_ext.h>
#include <game_sp/g_sp_levelstart.h>
#include "sentient.h"

const struct $97E41B13BD0B431546CEBD7708497565 // sizeof=0xC
{                                       // XREF: .rdata:g_ai_event_info/r
    const char *name;                   // XREF: Actor_DumpEvents+6E/r
    const dvar_s **defaultDistDvar;     // XREF: Actor_EventDefaultRadiusSqrd(ai_event_t)+F/r
                                        // Actor_EventDefaultRadiusSqrd(ai_event_t)+41/r
    float defaultHeight;
};

const $97E41B13BD0B431546CEBD7708497565 g_ai_event_info[28] =
{
  { NULL, NULL, 0.0 },
  { NULL, NULL, 0.0 },
  { "footstep", &ai_eventDistFootstep, 0.0 },
  { "footsteplite", &ai_eventDistFootstepLite, 0.0 },
  { "new_enemy", &ai_eventDistNewEnemy, 0.0 },
  { "pain", &ai_eventDistPain, 0.0 },
  { "react", &ai_eventDistReact, 0.0 },
  { "death", &ai_eventDistDeath, 0.0 },
  { "explosion", &ai_eventDistExplosion, 0.0 },
  { "grenade_ping", &ai_eventDistGrenadePing, 0.0 },
  { "projectile_ping", &ai_eventDistProjPing, 0.0 },
  { "gunshot", &ai_eventDistGunShot, 0.0 },
  { "silenced_shot", &ai_eventDistSilencedShot, 0.0 },
  { NULL, NULL, 0.0 },
  { NULL, NULL, 0.0 },
  { "bullet", &ai_eventDistBullet, 45.0 },
  { "bullet", &ai_eventDistBullet, 45.0 },
  { "bullet_react", &ai_eventDistBulletRunning, 45.0 },
  { "projectile_impact", &ai_eventDistProjImpact, 45.0 },
  { NULL, NULL, 0.0 },
  { NULL, NULL, 0.0 },
  { "badplacearc", &ai_eventDistBadPlace, 0.0 },
  { "badplacelimitedarc", &ai_eventDistBadPlace, 0.0 },
  { "reallybadplacearc", &ai_eventDistBadPlace, 0.0 },
  { NULL, NULL, 0.0 },
  { NULL, NULL, 0.0 },
  { "badplacevolume", &ai_eventDistBadPlace, 0.0 },
  { NULL, NULL, 0.0 }
};



double __fastcall Actor_EventDefaultRadiusSqrd(ai_event_t eType)
{
    const dvar_s *dvar; // [esp+4h] [ebp-4h]

    if ( !g_ai_event_info[eType].defaultDistDvar
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    78,
                    0,
                    "%s",
                    "g_ai_event_info[eType].defaultDistDvar") )
    {
        __debugbreak();
    }
    dvar = *g_ai_event_info[eType].defaultDistDvar;
    if ( !dvar && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 81, 0, "%s", "dvar") )
        __debugbreak();
    return dvar->current.value * dvar->current.value;
}

double __fastcall Actor_EventDefaultHeightDiff(ai_event_t eType)
{
    return g_ai_event_info[eType].defaultHeight;
}

void __fastcall Actor_GetPerfectInfo(actor_s *self, sentient_s *pOther)
{
    Actor_UpdateLastKnownPos(self, pOther);
}

void __fastcall Actor_WasAttackedBy(actor_s *self, sentient_s *pOther)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 162, 0, "%s", "self") )
        __debugbreak();
    if ( !pOther && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 163, 0, "%s", "pOther") )
        __debugbreak();
    if ( (pOther < level.sentients || pOther >= &level.sentients[MAX_SENTIENTS])
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    164,
                    0,
                    "%s",
                    "pOther >= level.sentients && pOther < level.sentients + MAX_SENTIENTS") )
    {
        __debugbreak();
    }
    if ( pOther != &level.sentients[pOther - level.sentients]
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    165,
                    0,
                    "%s",
                    "pOther == level.sentients + (pOther - level.sentients)") )
    {
        __debugbreak();
    }
    self->sentientInfo[pOther - level.sentients].iLastAttackMeTime = level.time;
}

void __fastcall Actor_BroadcastTeamEvent(sentient_s *sentient, ai_event_t eType)
{
    if ( !sentient
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 181, 0, "%s", "sentient") )
    {
        __debugbreak();
    }
    if ( !sentient->ent
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 182, 0, "%s", "sentient->ent") )
    {
        __debugbreak();
    }
    Actor_BroadcastPointEvent(sentient->ent, eType, 1 << sentient->eTeam, sentient->ent->r.currentOrigin, 0.0);
}

void __fastcall Actor_BroadcastPointEvent(
                gentity_s *originator,
                ai_event_t eType,
                int teamFlags,
                float *vOrigin,
                float fRadiusSqrd)
{
    gentity_s *ent; // [esp+38h] [ebp-14h]
    team_t eEnemyTeam; // [esp+3Ch] [ebp-10h]
    actor_s *pActor; // [esp+40h] [ebp-Ch]
    int listenerIndex; // [esp+44h] [ebp-8h]
    float fDistSqrd; // [esp+48h] [ebp-4h]
    float fDistSqrda; // [esp+48h] [ebp-4h]

    if ( teamFlags > 15
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    223,
                    0,
                    "%s",
                    "teamFlags <= (1 << TEAM_NUM_TEAMS) - 1") )
    {
        __debugbreak();
    }
    if ( (eType <= AI_EV_FIRST_POINT_EVENT || eType >= AI_EV_LAST_POINT_EVENT)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    224,
                    0,
                    "%s",
                    "eType > AI_EV_FIRST_POINT_EVENT && eType < AI_EV_LAST_POINT_EVENT") )
    {
        __debugbreak();
    }
    if ( teamFlags )
        goto LABEL_15;
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 231, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( originator->sentient )
    {
        eEnemyTeam = Sentient_EnemyTeam(originator->sentient->eTeam);
        if ( eEnemyTeam )
        {
            teamFlags = 1 << eEnemyTeam;
LABEL_15:
            if ( fRadiusSqrd == 0.0 )
                fRadiusSqrd = Actor_EventDefaultRadiusSqrd(eType);
            for ( pActor = Actor_FirstActor(teamFlags); pActor; pActor = Actor_NextActor(pActor, teamFlags) )
            {
                if ( !Actor_IsOnSameTeam(originator, pActor, teamFlags) )
                {
                    fDistSqrd = Vec3DistanceSq(pActor->ent->r.currentOrigin, vOrigin);
                    if ( fRadiusSqrd >= fDistSqrd )
                        Actor_ReceivePointEvent(pActor, originator, eType, vOrigin, fDistSqrd, fRadiusSqrd);
                }
            }
            for ( listenerIndex = Actor_EventListener_First(eType, teamFlags);
                        listenerIndex >= 0;
                        listenerIndex = Actor_EventListener_Next(listenerIndex, eType, teamFlags) )
            {
                ent = Actor_EventListener_GetEntity(listenerIndex);
                fDistSqrda = Vec3DistanceSq(ent->r.currentOrigin, vOrigin);
                if ( fRadiusSqrd >= fDistSqrda )
                    Actor_EventListener_NotifyToListener(ent, originator, eType, vOrigin);
            }
        }
    }
}

char __cdecl Actor_IsOnSameTeam(gentity_s *originator, actor_s *pActor, int teamFlags)
{
    gentity_s *v4; // esi

    if ( pActor == originator->actor )
        return 1;
    //if ( !EntHandle::isDefined(&pActor->sentient->scriptOwner) )
    if ( !pActor->sentient->scriptOwner.isDefined() )
        return 0;
    //if ( EntHandle::ent(&pActor->sentient->scriptOwner) == originator )
    if ( pActor->sentient->scriptOwner.ent() == originator)
        return 1;
    if ( !teamFlags
        && originator->sentient
        //&& EntHandle::isDefined(&originator->sentient->scriptOwner)
        && originator->sentient->scriptOwner.isDefined()
        //&& (v4 = EntHandle::ent(&pActor->sentient->scriptOwner), v4 == EntHandle::ent(&originator->sentient->scriptOwner)) )
        && (v4 = pActor->sentient->scriptOwner.ent(), v4 == originator->sentient->scriptOwner.ent()))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void __fastcall Actor_BroadcastLineEvent(
                gentity_s *originator,
                gentity_s *hitEnt,
                ai_event_t eType,
                int teamFlags,
                const float *vStart,
                const float *vEnd,
                float fRadiusSqrd)
{
    float *v9; // [esp+2Ch] [ebp-60h]
    float *currentOrigin; // [esp+34h] [ebp-58h]
    gentity_s *ent; // [esp+48h] [ebp-44h]
    team_t eEnemyTeam; // [esp+4Ch] [ebp-40h]
    actor_s *pActor; // [esp+50h] [ebp-3Ch]
    float fScaledDist; // [esp+54h] [ebp-38h]
    float fScaledDista; // [esp+54h] [ebp-38h]
    float fMaxHeightDiff; // [esp+58h] [ebp-34h]
    int listenerIndex; // [esp+5Ch] [ebp-30h]
    float fDistSqrd; // [esp+60h] [ebp-2Ch]
    float fLineLenSqrd; // [esp+64h] [ebp-28h]
    float vClosest[3]; // [esp+68h] [ebp-24h] BYREF
    float vLine[3]; // [esp+74h] [ebp-18h]
    float vDelta[3]; // [esp+80h] [ebp-Ch]

    if ( teamFlags > 15
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    296,
                    0,
                    "%s",
                    "teamFlags <= (1 << TEAM_NUM_TEAMS) - 1") )
    {
        __debugbreak();
    }
    if ( (eType <= AI_EV_FIRST_LINE_EVENT || eType >= AI_EV_LAST_LINE_EVENT)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    297,
                    0,
                    "%s",
                    "eType > AI_EV_FIRST_LINE_EVENT && eType < AI_EV_LAST_LINE_EVENT") )
    {
        __debugbreak();
    }
    if ( !teamFlags )
    {
        if ( !originator->sentient )
            return;
        eEnemyTeam = Sentient_EnemyTeam(originator->sentient->eTeam);
        if ( eEnemyTeam == TEAM_FREE )
            return;
        teamFlags = 1 << eEnemyTeam;
    }
    if ( fRadiusSqrd == 0.0 )
        fRadiusSqrd = Actor_EventDefaultRadiusSqrd(eType);
    fMaxHeightDiff = Actor_EventDefaultHeightDiff(eType);
    vLine[0] = *vEnd - *vStart;
    vLine[1] = vEnd[1] - vStart[1];
    vLine[2] = vEnd[2] - vStart[2];
    fLineLenSqrd = (float)((float)(vLine[0] * vLine[0]) + (float)(vLine[1] * vLine[1])) + (float)(vLine[2] * vLine[2]);
    for ( pActor = Actor_FirstActor(teamFlags); pActor; pActor = Actor_NextActor(pActor, teamFlags) )
    {
        if ( !Actor_IsOnSameTeam(originator, pActor, teamFlags) )
        {
            currentOrigin = pActor->ent->r.currentOrigin;
            vDelta[0] = *currentOrigin - *vStart;
            vDelta[1] = currentOrigin[1] - vStart[1];
            vDelta[2] = currentOrigin[2] - vStart[2];
            fScaledDist = (float)((float)(vDelta[0] * vLine[0]) + (float)(vDelta[1] * vLine[1]))
                                    + (float)(vDelta[2] * vLine[2]);
            if ( fScaledDist >= 0.0 )
            {
                if ( fScaledDist < fLineLenSqrd )
                {
                    if ( fLineLenSqrd == 0.0
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                                    365,
                                    0,
                                    "%s",
                                    "fLineLenSqrd") )
                    {
                        __debugbreak();
                    }
                    vClosest[0] = (float)((float)(fScaledDist / fLineLenSqrd) * vLine[0]) + *vStart;
                    vClosest[1] = (float)((float)(fScaledDist / fLineLenSqrd) * vLine[1]) + vStart[1];
                    vClosest[2] = (float)((float)(fScaledDist / fLineLenSqrd) * vLine[2]) + vStart[2];
                }
                else
                {
                    vClosest[0] = *vEnd;
                    vClosest[1] = vEnd[1];
                    vClosest[2] = vEnd[2];
                }
                v9 = pActor->ent->r.currentOrigin;
                vDelta[0] = *v9 - vClosest[0];
                vDelta[1] = v9[1] - vClosest[1];
                vDelta[2] = v9[2] - vClosest[2];
                vDelta[2] = (float)((float)(48.0 + 0.0) * 0.5) + vDelta[2];
                fDistSqrd = (float)(vDelta[0] * vDelta[0]) + (float)(vDelta[1] * vDelta[1]);
                if ( fRadiusSqrd >= fDistSqrd
                    && fabs(fMaxHeightDiff) > fabs(vDelta[2]) )
                {
                    Actor_ReceiveLineEvent(pActor, originator, hitEnt, eType, vStart, vEnd, vClosest, fDistSqrd, fRadiusSqrd);
                }
            }
        }
    }
    for ( listenerIndex = Actor_EventListener_First(eType, teamFlags);
                listenerIndex >= 0;
                listenerIndex = Actor_EventListener_Next(listenerIndex, eType, teamFlags) )
    {
        ent = Actor_EventListener_GetEntity(listenerIndex);
        vDelta[0] = ent->r.currentOrigin[0] - *vStart;
        vDelta[1] = ent->r.currentOrigin[1] - vStart[1];
        vDelta[2] = ent->r.currentOrigin[2] - vStart[2];
        fScaledDista = (float)((float)(vDelta[0] * vLine[0]) + (float)(vDelta[1] * vLine[1]))
                                 + (float)(vDelta[2] * vLine[2]);
        if ( fScaledDista >= 0.0 )
        {
            if ( fScaledDista < fLineLenSqrd )
            {
                if ( fLineLenSqrd == 0.0
                    && !Assert_MyHandler(
                                "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                                410,
                                0,
                                "%s",
                                "fLineLenSqrd") )
                {
                    __debugbreak();
                }
                vClosest[0] = (float)((float)(fScaledDista / fLineLenSqrd) * vLine[0]) + *vStart;
                vClosest[1] = (float)((float)(fScaledDista / fLineLenSqrd) * vLine[1]) + vStart[1];
                vClosest[2] = (float)((float)(fScaledDista / fLineLenSqrd) * vLine[2]) + vStart[2];
            }
            else
            {
                vClosest[0] = *vEnd;
                vClosest[1] = vEnd[1];
                vClosest[2] = vEnd[2];
            }
            vDelta[0] = ent->r.currentOrigin[0] - vClosest[0];
            vDelta[1] = ent->r.currentOrigin[1] - vClosest[1];
            vDelta[2] = ent->r.currentOrigin[2] - vClosest[2];
            if ( fRadiusSqrd >= (float)((float)(vDelta[0] * vDelta[0]) + (float)(vDelta[1] * vDelta[1]))
                && fabs(fMaxHeightDiff) > fabs(vDelta[2]) )
            {
                Actor_EventListener_NotifyToListener(ent, originator, eType, vClosest);
            }
        }
    }
}

void __fastcall Actor_ReceivePointEvent(
                actor_s *self,
                gentity_s *originator,
                ai_event_t eType,
                float *vOrigin,
                float fDistSqrd,
                float fRadiusSqrd)
{
    gentity_s *v6; // eax
    gentity_s *v7; // eax
    gentity_s *v8; // eax
    gentity_s *v9; // eax

    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 784, 0, "%s", "self") )
        __debugbreak();
    if ( !vOrigin
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 785, 0, "%s", "vOrigin") )
    {
        __debugbreak();
    }
    if ( (fDistSqrd < 0.0 || fRadiusSqrd < fDistSqrd)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    786,
                    0,
                    "%s",
                    "fDistSqrd >= 0 && fDistSqrd <= fRadiusSqrd") )
    {
        __debugbreak();
    }
    if ( (eType <= AI_EV_FIRST_POINT_EVENT || eType >= AI_EV_LAST_POINT_EVENT)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    787,
                    0,
                    "%s",
                    "eType > AI_EV_FIRST_POINT_EVENT && eType < AI_EV_LAST_POINT_EVENT") )
    {
        __debugbreak();
    }
    switch ( eType )
    {
        case AI_EV_FOOTSTEP:
        case AI_EV_FOOTSTEP_LITE:
            if ( !originator
                && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 793, 0, "%s", "originator") )
            {
                __debugbreak();
            }
            if ( !originator->sentient
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            794,
                            0,
                            "%s",
                            "originator->sentient") )
            {
                __debugbreak();
            }
            if ( Actor_CaresAboutInfo(self, originator->sentient) )
            {
                Actor_EventFootstep(self, originator->sentient, vOrigin);
                break;
            }
            return;
        case AI_EV_NEW_ENEMY:
            if ( !originator
                && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 809, 0, "%s", "originator") )
            {
                __debugbreak();
            }
            if ( !originator->sentient
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            810,
                            0,
                            "%s",
                            "originator->sentient") )
            {
                __debugbreak();
            }
            Actor_EventNewEnemy(self, originator->sentient);
            break;
        case AI_EV_PAIN:
            if ( !originator
                && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 816, 0, "%s", "originator") )
            {
                __debugbreak();
            }
            if ( !originator->sentient
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            817,
                            0,
                            "%s",
                            "originator->sentient") )
            {
                __debugbreak();
            }
            //if ( !EntHandle::isDefined(&originator->sentient->lastAttacker)
            if ( !originator->sentient->lastAttacker.isDefined()
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            818,
                            0,
                            "%s",
                            "originator->sentient->lastAttacker.isDefined()") )
            {
                __debugbreak();
            }
            //if ( !EntHandle::ent(&originator->sentient->lastAttacker)->sentient )
            if ( !originator->sentient->lastAttacker.ent()->sentient)
                return;
            //v6 = EntHandle::ent(&originator->sentient->lastAttacker);
            v6 = originator->sentient->lastAttacker.ent();
            if ( !Actor_CaresAboutInfo(self, v6->sentient) )
                return;
            //v7 = EntHandle::ent(&originator->sentient->lastAttacker);
            v7 = originator->sentient->lastAttacker.ent();
            Actor_EventPain(self, originator->sentient, v7->sentient);
            break;
        case AI_EV_REACT:
            break;
        case AI_EV_DEATH:
            if ( !originator
                && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 845, 0, "%s", "originator") )
            {
                __debugbreak();
            }
            if ( !originator->sentient
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            846,
                            0,
                            "%s",
                            "originator->sentient") )
            {
                __debugbreak();
            }
            //if ( !EntHandle::isDefined(&originator->sentient->lastAttacker) )
            if ( !originator->sentient->lastAttacker.isDefined() )
                return;
            //if ( !EntHandle::ent(&originator->sentient->lastAttacker)->sentient )
            if ( !originator->sentient->lastAttacker.ent()->sentient)
                return;
            //v8 = EntHandle::ent(&originator->sentient->lastAttacker);
            v8 = originator->sentient->lastAttacker.ent();
            if ( !Actor_CaresAboutInfo(self, v8->sentient) )
                return;
            //v9 = EntHandle::ent(&originator->sentient->lastAttacker);
            v9 = originator->sentient->lastAttacker.ent();
            Actor_EventDeath(self, originator->sentient, v9->sentient);
            break;
        case AI_EV_EXPLOSION:
            Actor_EventExplosion(self, originator, vOrigin);
            break;
        case AI_EV_GRENADE_PING:
            goto LABEL_60;
        case AI_EV_PROJECTILE_PING:
            if ( !originator
                && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 865, 0, "%s", "originator") )
            {
                __debugbreak();
            }
            //if ( EntHandle::isDefined(&originator->parent) && EntHandle::ent(&originator->parent) == self->ent )
            if ( originator->parent.isDefined() && originator->parent.ent() == self->ent)
                return;
LABEL_60:
            Actor_EventGrenadePing(self, originator, vOrigin);
            break;
        case AI_EV_GUNSHOT:
        case AI_EV_SILENCED_SHOT:
            if ( !originator
                && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 874, 0, "%s", "originator") )
            {
                __debugbreak();
            }
            if ( !originator->sentient
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            875,
                            0,
                            "%s",
                            "originator->sentient") )
            {
                __debugbreak();
            }
            if ( originator == self->ent || !Actor_CaresAboutInfo(self, originator->sentient) )
                return;
            Actor_EventGunshot(self, originator->sentient, vOrigin);
            break;
        default:
            if ( !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            887,
                            0,
                            "Actor_ReceivePointEvent: unhandled case\n") )
                __debugbreak();
            return;
    }
    Actor_DumpEvents(self, eType, originator);
}

void __cdecl Actor_DumpEvents(actor_s *self, ai_event_t event, gentity_s *originator)
{
    int number; // [esp+0h] [ebp-8h]

    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 103, 0, "%s", "self") )
        __debugbreak();
    if ( g_dumpAIEvents->current.integer == self->ent->s.number )
    {
        if ( originator )
            number = originator->s.number;
        else
            number = -1;
        Com_Printf(
            18,
            "%d ^3 %s^7:    from entity^5 %d ^7at time^5 %d\n",
            self->ent->s.number,
            g_ai_event_info[event].name,
            number,
            level.time);
    }
}

void __fastcall Actor_EventFootstep(actor_s *self, sentient_s *originator, const float *vOrigin)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 586, 0, "%s", "self") )
        __debugbreak();
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 587, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( !vOrigin
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 588, 0, "%s", "vOrigin") )
    {
        __debugbreak();
    }
    Actor_UpdateLastKnownPos(self, originator);
}

void __fastcall Actor_EventNewEnemy(actor_s *self, sentient_s *originator)
{
    sentient_s *enemy; // [esp+8h] [ebp-4h]

    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 604, 0, "%s", "self") )
        __debugbreak();
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 605, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( !originator->ent
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 606, 0, "%s", "originator->ent") )
    {
        __debugbreak();
    }
    //if ( !EntHandle::isDefined(&originator->targetEnt)
    if ( !originator->targetEnt.isDefined()
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    607,
                    0,
                    "%s",
                    "originator->targetEnt.isDefined()") )
    {
        __debugbreak();
    }
    //if ( !EntHandle::ent(&originator->targetEnt)->sentient
    if ( !originator->targetEnt.ent()->sentient
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    608,
                    0,
                    "%s",
                    "originator->targetEnt.ent()->sentient") )
    {
        __debugbreak();
    }
    //enemy = EntHandle::ent(&originator->targetEnt)->sentient;
    enemy = originator->targetEnt.ent()->sentient;
    if ( originator->ent->actor )
        SentientInfo_Copy(self, originator->ent->actor, enemy - level.sentients);
    else
        Actor_GetPerfectInfo(self, enemy);
}

void __fastcall Actor_EventPain(actor_s *self, sentient_s *pCasualty, sentient_s *pAttacker)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 634, 0, "%s", "self") )
        __debugbreak();
    if ( !pCasualty
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 635, 0, "%s", "pCasualty") )
    {
        __debugbreak();
    }
    if ( !pAttacker
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 636, 0, "%s", "pAttacker") )
    {
        __debugbreak();
    }
    Actor_WasAttackedBy(self, pAttacker);
    Actor_GetPerfectInfo(self, pAttacker);
}

void __fastcall Actor_EventDeath(actor_s *self, sentient_s *pCasualty, sentient_s *pAttacker)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 675, 0, "%s", "self") )
        __debugbreak();
    if ( !pCasualty
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 676, 0, "%s", "pCasualty") )
    {
        __debugbreak();
    }
    if ( !pAttacker
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 677, 0, "%s", "pAttacker") )
    {
        __debugbreak();
    }
    Actor_GetPerfectInfo(self, pAttacker);
}

void __cdecl Actor_EventExplosion(actor_s *self, gentity_s *originator, float *vOrigin)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 692, 0, "%s", "self") )
        __debugbreak();
    if ( !vOrigin
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 693, 0, "%s", "vOrigin") )
    {
        __debugbreak();
    }
    if ( originator )
        Scr_AddEntity(originator, SCRIPTINSTANCE_SERVER);
    else
        Scr_AddUndefined(SCRIPTINSTANCE_SERVER);
    Scr_AddVector(vOrigin, SCRIPTINSTANCE_SERVER);
    Scr_Notify(self->ent, scr_const.explode, 2u);
    ScrNotify_FaceEvent(self->ent, scr_const.explode);
}

void __cdecl Actor_EventGrenadePing(actor_s *self, gentity_s *originator, const float *vOrigin)
{
    if ( zombiemode->current.enabled ) // zombies: native grenade ping (SP 0x00441310)
        return Actor_EventGrenadePing_SP(self, originator);
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 717, 0, "%s", "self") )
        __debugbreak();
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 718, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( !vOrigin
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 719, 0, "%s", "vOrigin") )
    {
        __debugbreak();
    }
}

void __fastcall Actor_EventGunshot(actor_s *self, sentient_s *originator, const float *vOrigin)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 737, 0, "%s", "self") )
        __debugbreak();
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 738, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( !vOrigin
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 739, 0, "%s", "vOrigin") )
    {
        __debugbreak();
    }
    Actor_GetPerfectInfo(self, originator);
}

void __fastcall Actor_ReceiveLineEvent(
                actor_s *self,
                gentity_s *originator,
                gentity_s *hitEnt,
                ai_event_t eType,
                const float *vStart,
                const float *vEnd,
                const float *vClosest,
                float fDistSqrd,
                float fRadiusSqrd)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 904, 0, "%s", "self") )
        __debugbreak();
    if ( !vStart && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 905, 0, "%s", "vStart") )
        __debugbreak();
    if ( !vEnd && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 906, 0, "%s", "vEnd") )
        __debugbreak();
    if ( !vClosest
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 907, 0, "%s", "vClosest") )
    {
        __debugbreak();
    }
    if ( (fDistSqrd < 0.0 || fRadiusSqrd < fDistSqrd)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    908,
                    0,
                    "%s",
                    "fDistSqrd >= 0 && fDistSqrd <= fRadiusSqrd") )
    {
        __debugbreak();
    }
    if ( (eType <= AI_EV_FIRST_LINE_EVENT || eType >= AI_EV_LAST_LINE_EVENT)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    909,
                    0,
                    "%s",
                    "eType > AI_EV_FIRST_LINE_EVENT && eType < AI_EV_LAST_LINE_EVENT") )
    {
        __debugbreak();
    }
    if ( eType < AI_EV_BULLET )
        goto LABEL_31;
    if ( eType > AI_EV_BLOCK_FRIENDLIES )
    {
        if ( eType != AI_EV_PROJECTILE_IMPACT )
        {
LABEL_31:
            if ( !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                            959,
                            0,
                            "Actor_ReceiveLineEvent: unhandled case\n") )
                __debugbreak();
            return;
        }
        Actor_EventBullet(self, originator, vStart, vEnd, vClosest, fDistSqrd, fRadiusSqrd, DONT_SUPPRESS);
LABEL_34:
        Actor_DumpEvents(self, eType, originator);
        return;
    }
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 915, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( originator != self->ent )
    {
        Actor_EventBullet(self, originator, vStart, vEnd, vClosest, fDistSqrd, fRadiusSqrd, DO_SUPPRESSION);
        goto LABEL_34;
    }
}

void __fastcall Actor_EventBullet(
                actor_s *self,
                gentity_s *originator,
                const float *vStart,
                const float *vEnd,
                const float *vClosest,
                float fDistSqrd,
                float fRadiusSqrd,
                PARM_SUPPRESSION suppression)
{
    if ( !self && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 755, 0, "%s", "self") )
        __debugbreak();
    if ( !originator
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 756, 0, "%s", "originator") )
    {
        __debugbreak();
    }
    if ( !vStart && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 757, 0, "%s", "vStart") )
        __debugbreak();
    if ( !vEnd && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 758, 0, "%s", "vEnd") )
        __debugbreak();
    if ( !vClosest
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp", 759, 0, "%s", "vClosest") )
    {
        __debugbreak();
    }
    if ( (fDistSqrd < 0.0 || fRadiusSqrd < fDistSqrd)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\game\\actor_events.cpp",
                    760,
                    0,
                    "%s",
                    "fDistSqrd >= 0 && fDistSqrd <= fRadiusSqrd") )
    {
        __debugbreak();
    }
    if ( originator->sentient )
    {
        Actor_WasAttackedBy(self, originator->sentient);
        Actor_GetPerfectInfo(self, originator->sentient);
        // zombies: SP 0x007C0E05: a bullet that isn't a projectile's impact suppresses. MP has no suppression.
        if ( suppression == DO_SUPPRESSION && G_SP_IsSPLevel() )
            Actor_SP_BulletSuppression(self, originator->sentient, vStart, vEnd, vClosest);
    }
}

// zombies: SP 0x007CF870: the actor stands within 32 units across (80 up or down) of the node it claims
// (SP 0x0051AAC0), or of its nearest node when that is a cover node (SP 0x00579730).
static bool Actor_SP_AtCoverForSuppression(actor_s *self)
{
    const float *origin = self->ent->r.currentOrigin;
    auto within = [origin](const pathnode_t *node)
    {
        const float dz = origin[2] - node->constant.vOrigin[2];
        if ( dz * dz > 6400.0f )
            return false;
        const float dx = node->constant.vOrigin[0] - origin[0];
        const float dy = node->constant.vOrigin[1] - origin[1];
        return dx * dx + dy * dy <= 32.0f * 32.0f;
    };
    const pathnode_t *claimed = self->sentient->pClaimedNode;
    if ( claimed && within(claimed) )
        return true;
    const pathnode_t *nearest = Sentient_NearestNode(self->sentient);
    return nearest && ((1 << nearest->constant.type) & 0x83ffc) != 0 && within(nearest);
}

// zombies: SP 0x007CF8D0: "bulletwhizby" with the shooter, the bullet's start and end and its closest point.
static void Actor_SP_NotifyBulletWhizby(actor_s *self, sentient_s *shooter, const float *vStart, const float *vEnd,
                                        const float *vClosest)
{
    Scr_AddVector(const_cast<float *>(vClosest), SCRIPTINSTANCE_SERVER);
    Scr_AddVector(const_cast<float *>(vEnd), SCRIPTINSTANCE_SERVER);
    Scr_AddVector(const_cast<float *>(vStart), SCRIPTINSTANCE_SERVER);
    Scr_AddEntity(shooter->ent, SCRIPTINSTANCE_SERVER);
    Scr_Notify(self->ent, scr_const.bulletwhizby, 4u);
}

// zombies: SP 0x00407870, the enemy's half (from 0x00407A6A). The suppression meter rises by 0.15 to 1 at most.
// An actor at its claimed node or at cover records a suppression plane: the vertical plane along the bullet,
// its normal turned away from the actor, in the shooter's slot or the oldest (SP 0x007CF830); the reacquire
// paths then keep off its far side. It is then suppressed ("suppression", suppressionStartTime) unless it
// ignores suppression, which hears the bullet instead, as an actor away from any such node does.
// The same-team half (0x0040788D, ai_friendlySuppressionDist) is not here: no actor of a zombie map shares a
// team with anything that fires bullets.
void Actor_SP_BulletSuppression(actor_s *self, sentient_s *shooter, const float *vStart, const float *vEnd,
                                const float *vClosest)
{
    if ( self->sentient->eTeam == shooter->eTeam )
        return;
    self->suppressionMeter += 0.15f;
    if ( self->suppressionMeter > 1.0f )
        self->suppressionMeter = 1.0f;
    if ( !Actor_SP_AtCoverForSuppression(self) )
    {
        Actor_SP_NotifyBulletWhizby(self, shooter, vStart, vEnd, vClosest);
        return;
    }
    actor_sp_ext_t *ext = Actor_SP_Ext(self);
    int slot = 0;
    for ( int i = 0; i < 4; ++i )
    {
        if ( ext->suppression[i].shooter == shooter )
        {
            slot = i;
            break;
        }
        if ( ext->suppression[i].time < ext->suppression[slot].time )
            slot = i;
    }
    auto &rec = ext->suppression[slot];
    rec.time = level.time;
    rec.shooter = shooter;
    float nx = vStart[1] - vEnd[1];
    float ny = vEnd[0] - vStart[0];
    float dist = vStart[1] * ny + vStart[0] * nx;
    const float *origin = self->ent->r.currentOrigin;
    if ( origin[1] * ny + origin[0] * nx > dist )
    {
        nx = -nx;
        ny = -ny;
        dist = -dist;
    }
    rec.normal[0] = nx;
    rec.normal[1] = ny;
    rec.dist = dist;
    if ( self->ignoreSuppression )
    {
        Actor_SP_NotifyBulletWhizby(self, shooter, vStart, vEnd, vClosest);
        return;
    }
    rec.friendly = 0;
    if ( !self->suppressionStartTime )
        self->suppressionStartTime = level.time;
    Scr_AddEntity(shooter->ent, SCRIPTINSTANCE_SERVER);
    Scr_Notify(self->ent, scr_const.suppression, 1u);
}

// zombies: SP 0x005623D0, at the start of an actor's think (SP 0x0049E2E6). The meter falls by 0.01 a think
// (0 at once for an actor that ignores suppression); an enemy's plane lapses after suppressionDuration (at once
// when ignored); and with no enemy's plane left the actor is no longer suppressed ("suppression_end").
void Actor_SP_UpdateSuppression(actor_s *self)
{
    const bool wasSuppressed = self->suppressionStartTime > 0;
    if ( self->ignoreSuppression )
    {
        self->suppressionMeter = 0.0f;
    }
    else
    {
        self->suppressionMeter -= 0.01f;
        if ( self->suppressionMeter <= 0.0f )
            self->suppressionMeter = 0.0f;
    }
    actor_sp_ext_t *ext = Actor_SP_Ext(self);
    bool enemyPlane = false;
    for ( auto &rec : ext->suppression )
    {
        if ( rec.time <= 0 )
            continue;
        // a same-team plane would last ai_friendlyFireBlockDuration; none is recorded here
        const int duration = self->ignoreSuppression ? 0 : self->suppressionDuration;
        if ( level.time - rec.time >= duration )
        {
            rec.time = 0;
            rec.shooter = NULL;
            rec.friendly = 0;
        }
        else if ( !rec.friendly )
        {
            enemyPlane = true;
        }
    }
    if ( enemyPlane )
        return;
    if ( wasSuppressed )
        Scr_Notify(self->ent, scr_const.suppression_end, 0);
    self->suppressionStartTime = 0;
}

