// zombies: the gamepad's rumble, which the PC exe left out. Its script functions do nothing (playrumbleonentity,
// playrumbleonposition: game_mp/g_scr_main_mp.cpp, cgame/cg_scr_main.cpp), and nothing ran the motors but to stop them
// at the start (win32/win_gamepad.cpp: GPad_InitAll). The rumbles are in the game files, as the SP game played them:
// rumble/<name>, rawfiles of code_post_gfx.ff (damage_heavy, explosion_generic, ...) and common_zombie.ff (the zombie
// weapons' pistol_fire, assault_fire, shotgun_fire, ...), each an info string
//
//   RUMBLE\lowRumbleFile\pistol_fire_l.rmb\highRumbleFile\pistol_fire_h.rmb\duration\0.5\range\1\fadeWithDistance\1\...
//
// naming two graphs of its strength over its duration: rumble/pistol_fire_l.rmb, "RUMBLEGRAPHFILE", the number of
// knots, then each knot's time (0 to 1, of the duration) and strength (0 to 1). The low one runs the left motor (the
// heavy, slow one), the high one the right. What plays them, for the player's own:
//   - each shot of their weapon: its fireRumble (CG_FireWeapon)
//   - their melee hitting a zombie: its meleeImpactRumble (the knife's defaultweapon_melee; cg_event.cpp:
//     EV_MELEE_BLOOD, the player's own event for it, as EV_MELEE_HIT comes on a temporary entity)
//   - damage: damage_light, or damage_heavy from 40 (a zombie's swipe does 50; CG_DamageFeedback)
//   - an explosion: explosion_generic, fading out to its range (1000) from where it went off (CG_ExplosionEvent)
//   - the camera shaking where they are (earthquakes; CG_ShakeCamera): both motors, as strong as the shake.
// Each frame the client sums them, each motor at most full (CG_UpdateRumble), and the gamepad runs at that while the
// client goes on drawing frames and the gamepad is on (win32/win_input.cpp: GPad_UpdateRumble). While the game is paused
// (played alone, under its menu) none runs: its clock stops, which would hold each at the strength it had.
// What plays goes on as the game does.
#include "cg_rumble.h"

#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_main_mp.h>
#include <database/db_registry.h>
#include <qcommon/common.h>
#include <universal/com_math.h>
#include <universal/q_shared.h>
#include <win32/win_gamepad.h>

#include <cstdlib>
#include <cstring>

namespace
{
constexpr int RUMBLE_INFO_MAX = 32;
constexpr int RUMBLE_KNOT_MAX = 16;
constexpr int RUMBLE_ACTIVE_MAX = 8;

struct RumbleGraph
{
    int knotCount;
    float time[RUMBLE_KNOT_MAX];
    float strength[RUMBLE_KNOT_MAX];
};

struct RumbleInfo
{
    char name[64];
    bool found; // the game files have it, and both its graphs
    int duration; // ms
    float range;
    bool fadeWithDistance;
    RumbleGraph low;
    RumbleGraph high;
};

struct ActiveRumble
{
    const RumbleInfo *info;
    int startTime; // the client's time
    float scale;
};

RumbleInfo s_rumbleInfos[RUMBLE_INFO_MAX];
int s_rumbleInfoCount;
ActiveRumble s_activeRumbles[RUMBLE_ACTIVE_MAX];
float s_shakeRumble;

const char *RawFileText(const char *name)
{
    const RawFile *rawfile = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char *)name, false, 0).rawfile;
    return rawfile ? rawfile->buffer : nullptr;
}

// an info string's value for the key (\key\value\key\value), or ""
void InfoValue(const char *info, const char *key, char *value, int size)
{
    *value = 0;
    const size_t keyLen = strlen(key);
    for (const char *p = info; (p = strchr(p, '\\')) != nullptr;)
    {
        ++p;
        const char *end = strchr(p, '\\');
        if (!end)
            return;
        const char *valueEnd = strchr(end + 1, '\\');
        if ((size_t)(end - p) == keyLen && !I_strnicmp(p, key, (int)keyLen))
        {
            const int len = valueEnd ? (int)(valueEnd - end - 1) : (int)strlen(end + 1);
            I_strncpyz(value, end + 1, len + 1 < size ? len + 1 : size);
            return;
        }
        p = valueEnd ? valueEnd : end + 1;
    }
}

bool LoadGraph(const char *file, RumbleGraph *graph)
{
    graph->knotCount = 0;
    const char *text = RawFileText(va("rumble/%s", file));
    if (!text || strncmp(text, "RUMBLEGRAPHFILE", 15))
        return false;
    char *p = (char *)text + 15;
    const int count = (int)strtol(p, &p, 10);
    for (int i = 0; i < count && i < RUMBLE_KNOT_MAX; ++i)
    {
        graph->time[i] = (float)strtod(p, &p);
        graph->strength[i] = (float)strtod(p, &p);
        graph->knotCount = i + 1;
    }
    return graph->knotCount > 0;
}

const RumbleInfo *FindRumble(const char *name)
{
    for (int i = 0; i < s_rumbleInfoCount; ++i)
        if (!I_stricmp(s_rumbleInfos[i].name, name))
            return s_rumbleInfos[i].found ? &s_rumbleInfos[i] : nullptr;
    if (s_rumbleInfoCount == RUMBLE_INFO_MAX)
        return nullptr;
    RumbleInfo *info = &s_rumbleInfos[s_rumbleInfoCount++];
    memset(info, 0, sizeof(*info));
    I_strncpyz(info->name, name, sizeof(info->name));
    const char *text = RawFileText(va("rumble/%s", name));
    char low[64], high[64], value[64];
    if (text && !strncmp(text, "RUMBLE\\", 7))
    {
        InfoValue(text + 6, "lowRumbleFile", low, sizeof(low));
        InfoValue(text + 6, "highRumbleFile", high, sizeof(high));
        InfoValue(text + 6, "duration", value, sizeof(value));
        info->duration = (int)(atof(value) * 1000.0);
        InfoValue(text + 6, "range", value, sizeof(value));
        info->range = (float)atof(value);
        InfoValue(text + 6, "fadeWithDistance", value, sizeof(value));
        info->fadeWithDistance = atoi(value) != 0;
        info->found = info->duration > 0 && LoadGraph(low, &info->low) && LoadGraph(high, &info->high);
    }
    Com_Printf(14, "rumble: %s %s\n", name, info->found ? va("for %d ms", info->duration) : "is not in the game files");
    return info->found ? info : nullptr;
}

float GraphStrength(const RumbleGraph *graph, float t)
{
    if (t <= graph->time[0])
        return graph->strength[0];
    for (int i = 1; i < graph->knotCount; ++i)
    {
        if (t <= graph->time[i])
        {
            const float span = graph->time[i] - graph->time[i - 1];
            const float f = span > 0.0f ? (t - graph->time[i - 1]) / span : 1.0f;
            return graph->strength[i - 1] + (graph->strength[i] - graph->strength[i - 1]) * f;
        }
    }
    return graph->strength[graph->knotCount - 1];
}
} // namespace

// Plays the rumble from the start, at the player, or where origin is (its strength fading out to its range, if it
// fades with distance); the oldest gives way when all are playing.
void CG_PlayRumble(int localClientNum, const char *name, const float *origin)
{
    if (!name || !*name)
        return;
    const RumbleInfo *info = FindRumble(name);
    if (!info)
        return;
    const cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    float scale = 1.0f;
    if (origin && info->fadeWithDistance && info->range > 0.0f)
    {
        scale = 1.0f - Vec3Distance(origin, cgameGlob->refdef.vieworg) / info->range;
        if (scale <= 0.0f)
            return;
    }
    ActiveRumble *slot = &s_activeRumbles[0];
    for (int i = 0; i < RUMBLE_ACTIVE_MAX; ++i)
    {
        if (!s_activeRumbles[i].info)
        {
            slot = &s_activeRumbles[i];
            break;
        }
        if (s_activeRumbles[i].startTime < slot->startTime)
            slot = &s_activeRumbles[i];
    }
    slot->info = info;
    slot->startTime = cgameGlob->time;
    slot->scale = scale;
}

// The camera's shake where the player is this frame (CG_ShakeCamera), 0 to 1.
void CG_RumbleShake(int localClientNum, float size)
{
    s_shakeRumble = size > 1.0f ? 1.0f : size;
}

// Each frame the client draws: the motors' strength, what plays summed (CG_PlayRumble, CG_RumbleShake).
void CG_UpdateRumble(int localClientNum)
{
    if (cg_paused && cg_paused->current.integer)
    {
        s_shakeRumble = 0.0f;
        GPad_SetRumble(0, 0.0f, 0.0f);
        return;
    }
    const cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    float low = s_shakeRumble;
    float high = s_shakeRumble * 0.5f;
    s_shakeRumble = 0.0f;
    for (int i = 0; i < RUMBLE_ACTIVE_MAX; ++i)
    {
        ActiveRumble *rumble = &s_activeRumbles[i];
        if (!rumble->info)
            continue;
        const int elapsed = cgameGlob->time - rumble->startTime;
        if (elapsed < 0 || elapsed >= rumble->info->duration)
        {
            rumble->info = nullptr;
            continue;
        }
        const float t = (float)elapsed / (float)rumble->info->duration;
        low += GraphStrength(&rumble->info->low, t) * rumble->scale;
        high += GraphStrength(&rumble->info->high, t) * rumble->scale;
    }
    GPad_SetRumble(0, low > 1.0f ? 1.0f : low, high > 1.0f ? 1.0f : high);
}
