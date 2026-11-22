#include "r_draw_method.h"
#include "r_dvars.h"
#include "r_init.h"
#include "r_scene.h"
#include "r_primarylights.h"
#include "r_draw_shadowablelight.h"
#include <cgame_mp/cg_ents_mp.h>
#include <qcommon/cm_mapkit.h>
#include <qcommon/common.h>

GfxDrawMethod gfxDrawMethod;

void __cdecl R_InitDrawMethod()
{
    if ( r_fullbright->current.enabled )
    {
        gfxDrawMethod.drawScene = GFX_DRAW_SCENE_FULLBRIGHT;
        gfxDrawMethod.baseTechType = 4;
        gfxDrawMethod.emissiveTechType = 4;
        R_ForceLitTechType(4u);
    }
    else if ( r_debugShader->current.integer )
    {
        gfxDrawMethod.drawScene = GFX_DRAW_SCENE_DEBUGSHADER;
        gfxDrawMethod.baseTechType = 127;
        gfxDrawMethod.emissiveTechType = 127;
        R_ForceLitTechType(0x7Fu);
    }
    else
    {
        gfxDrawMethod.drawScene = GFX_DRAW_SCENE_STANDARD;
        gfxDrawMethod.baseTechType = 10;
        gfxDrawMethod.emissiveTechType = 5;
        R_SetDefaultLitTechTypes();
    }
}

void R_SetDefaultLitTechTypes()
{
    memset(gfxDrawMethod.litTechType[0][0][0], 0xFFu, 0x1C0u);
    gfxDrawMethod.litTechType[0][0][0][0] = 10;
    gfxDrawMethod.litTechType[0][1][0][0] = 11;
    gfxDrawMethod.litTechType[0][2][0][0] = 13;
    gfxDrawMethod.litTechType[0][3][0][0] = 15;
    gfxDrawMethod.litTechType[0][4][0][0] = 12;
    gfxDrawMethod.litTechType[0][5][0][0] = 14;
    gfxDrawMethod.litTechType[0][6][0][0] = 16;
    gfxDrawMethod.litTechType[0][0][1][0] = 52;
    gfxDrawMethod.litTechType[0][1][1][0] = 53;
    gfxDrawMethod.litTechType[0][2][1][0] = 55;
    gfxDrawMethod.litTechType[0][3][1][0] = 57;
    gfxDrawMethod.litTechType[0][4][1][0] = 54;
    gfxDrawMethod.litTechType[0][5][1][0] = 56;
    gfxDrawMethod.litTechType[0][6][1][0] = 58;
    gfxDrawMethod.litTechType[1][0][0][0] = 10;
    gfxDrawMethod.litTechType[1][1][0][0] = 11;
    gfxDrawMethod.litTechType[1][2][0][0] = 13;
    gfxDrawMethod.litTechType[1][3][0][0] = 15;
    gfxDrawMethod.litTechType[1][4][0][0] = 12;
    gfxDrawMethod.litTechType[1][5][0][0] = 14;
    gfxDrawMethod.litTechType[1][6][0][0] = 16;
    gfxDrawMethod.litTechType[1][0][1][0] = 52;
    gfxDrawMethod.litTechType[1][1][1][0] = 53;
    gfxDrawMethod.litTechType[1][2][1][0] = 55;
    gfxDrawMethod.litTechType[1][3][1][0] = 57;
    gfxDrawMethod.litTechType[1][4][1][0] = 54;
    gfxDrawMethod.litTechType[1][5][1][0] = 56;
    gfxDrawMethod.litTechType[1][6][1][0] = 58;
    *(_WORD *)&gfxDrawMethod.litTechType[2][0][0][0] = 15114;
    *(_WORD *)&gfxDrawMethod.litTechType[2][1][0][0] = 15627;
    *(_WORD *)&gfxDrawMethod.litTechType[2][2][0][0] = 16653;
    *(_WORD *)&gfxDrawMethod.litTechType[2][3][0][0] = 17679;
    *(_WORD *)&gfxDrawMethod.litTechType[2][4][0][0] = 16140;
    *(_WORD *)&gfxDrawMethod.litTechType[2][5][0][0] = 17166;
    *(_WORD *)&gfxDrawMethod.litTechType[2][6][0][0] = 18192;
    gfxDrawMethod.litTechType[3][0][0][0] = 73;
    gfxDrawMethod.litTechType[3][1][0][0] = 74;
    gfxDrawMethod.litTechType[3][2][0][0] = 76;
    gfxDrawMethod.litTechType[3][3][0][0] = 78;
    gfxDrawMethod.litTechType[3][4][0][0] = 75;
    gfxDrawMethod.litTechType[3][5][0][0] = 77;
    gfxDrawMethod.litTechType[3][6][0][0] = 79;
    gfxDrawMethod.litTechType[4][0][0][0] = 73;
    gfxDrawMethod.litTechType[4][1][0][0] = 74;
    gfxDrawMethod.litTechType[4][2][0][0] = 76;
    gfxDrawMethod.litTechType[4][3][0][0] = 78;
    gfxDrawMethod.litTechType[4][4][0][0] = 75;
    gfxDrawMethod.litTechType[4][5][0][0] = 77;
    gfxDrawMethod.litTechType[4][6][0][0] = 79;
    gfxDrawMethod.litTechType[5][0][0][0] = 10;
    gfxDrawMethod.litTechType[5][1][0][0] = 11;
    gfxDrawMethod.litTechType[5][2][0][0] = 13;
    gfxDrawMethod.litTechType[5][3][0][0] = 15;
    gfxDrawMethod.litTechType[5][4][0][0] = 12;
    gfxDrawMethod.litTechType[5][5][0][0] = 14;
    gfxDrawMethod.litTechType[5][6][0][0] = 16;
    gfxDrawMethod.litTechType[6][0][0][0] = 10;
    gfxDrawMethod.litTechType[6][1][0][0] = 11;
    gfxDrawMethod.litTechType[6][2][0][0] = 13;
    gfxDrawMethod.litTechType[6][3][0][0] = 15;
    gfxDrawMethod.litTechType[6][4][0][0] = 12;
    gfxDrawMethod.litTechType[6][5][0][0] = 14;
    gfxDrawMethod.litTechType[6][6][0][0] = 16;
    gfxDrawMethod.litTechType[6][0][1][0] = 52;
    gfxDrawMethod.litTechType[6][1][1][0] = 53;
    gfxDrawMethod.litTechType[6][2][1][0] = 55;
    gfxDrawMethod.litTechType[6][3][1][0] = 57;
    gfxDrawMethod.litTechType[6][4][1][0] = 54;
    gfxDrawMethod.litTechType[6][5][1][0] = 56;
    gfxDrawMethod.litTechType[6][6][1][0] = 58;
    *(_WORD *)&gfxDrawMethod.litTechType[7][0][0][0] = 15114;
    *(_WORD *)&gfxDrawMethod.litTechType[7][1][0][0] = 15627;
    *(_WORD *)&gfxDrawMethod.litTechType[7][2][0][0] = 16653;
    *(_WORD *)&gfxDrawMethod.litTechType[7][3][0][0] = 17679;
    *(_WORD *)&gfxDrawMethod.litTechType[7][4][0][0] = 16140;
    *(_WORD *)&gfxDrawMethod.litTechType[7][5][0][0] = 17166;
    *(_WORD *)&gfxDrawMethod.litTechType[7][6][0][0] = 18192;
    gfxDrawMethod.litTechType[8][0][0][0] = 10;
    gfxDrawMethod.litTechType[8][1][0][0] = 11;
    gfxDrawMethod.litTechType[8][2][0][0] = 13;
    gfxDrawMethod.litTechType[8][3][0][0] = 15;
    gfxDrawMethod.litTechType[8][4][0][0] = 12;
    gfxDrawMethod.litTechType[8][5][0][0] = 14;
    gfxDrawMethod.litTechType[8][6][0][0] = 16;
    *(_WORD *)&gfxDrawMethod.litTechType[9][0][0][0] = 15114;
    *(_WORD *)&gfxDrawMethod.litTechType[9][1][0][0] = 15627;
    *(_WORD *)&gfxDrawMethod.litTechType[9][2][0][0] = 16653;
    *(_WORD *)&gfxDrawMethod.litTechType[9][3][0][0] = 17679;
    *(_WORD *)&gfxDrawMethod.litTechType[9][4][0][0] = 16140;
    *(_WORD *)&gfxDrawMethod.litTechType[9][5][0][0] = 17166;
    *(_WORD *)&gfxDrawMethod.litTechType[9][6][0][0] = 18192;
    gfxDrawMethod.litTechType[12][0][0][0] = 10;
    gfxDrawMethod.litTechType[12][1][0][0] = 11;
    gfxDrawMethod.litTechType[12][2][0][0] = 13;
    gfxDrawMethod.litTechType[12][3][0][0] = 15;
    gfxDrawMethod.litTechType[12][4][0][0] = 12;
    gfxDrawMethod.litTechType[12][5][0][0] = 14;
    gfxDrawMethod.litTechType[12][6][0][0] = 16;
    gfxDrawMethod.litTechType[10][0][0][0] = 10;
    gfxDrawMethod.litTechType[10][1][0][0] = 11;
    gfxDrawMethod.litTechType[10][2][0][0] = 13;
    gfxDrawMethod.litTechType[10][3][0][0] = 15;
    gfxDrawMethod.litTechType[10][4][0][0] = 12;
    gfxDrawMethod.litTechType[10][5][0][0] = 14;
    gfxDrawMethod.litTechType[10][6][0][0] = 16;
    gfxDrawMethod.litTechType[14][0][0][0] = 10;
    gfxDrawMethod.litTechType[14][1][0][0] = 11;
    gfxDrawMethod.litTechType[14][2][0][0] = 13;
    gfxDrawMethod.litTechType[14][3][0][0] = 15;
    gfxDrawMethod.litTechType[14][4][0][0] = 12;
    gfxDrawMethod.litTechType[14][5][0][0] = 14;
    gfxDrawMethod.litTechType[14][6][0][0] = 16;
    gfxDrawMethod.litTechType[15][0][0][0] = 10;
    gfxDrawMethod.litTechType[15][1][0][0] = 11;
    gfxDrawMethod.litTechType[15][2][0][0] = 13;
    gfxDrawMethod.litTechType[15][3][0][0] = 15;
    gfxDrawMethod.litTechType[15][4][0][0] = 12;
    gfxDrawMethod.litTechType[15][5][0][0] = 14;
    gfxDrawMethod.litTechType[15][6][0][0] = 16;
}

void __cdecl R_ForceLitTechType(unsigned __int8 litTechType)
{
    memset(gfxDrawMethod.litTechType[0][0][0], litTechType, 0x1C0u);
}

void __cdecl R_UpdateDrawMethod(GfxBackEndData *data)
{
    bool v1; // [esp+7h] [ebp-21h]
    bool v2; // [esp+8h] [ebp-20h]
    GfxFadeType fadeType; // [esp+Ch] [ebp-1Ch]
    GfxTechCharredType charType; // [esp+14h] [ebp-14h]
    unsigned int primaryLightIndex; // [esp+1Ch] [ebp-Ch]
    unsigned int surfType; // [esp+20h] [ebp-8h]
    unsigned int lightTypea; // [esp+24h] [ebp-4h]
    unsigned int lightType; // [esp+24h] [ebp-4h]

    if ( data->shadowableLightCount > 0xFF
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_draw_method.cpp",
                    232,
                    0,
                    "%s\n\t(data->shadowableLightCount) = %i",
                    "(data->shadowableLightCount <= 255)",
                    data->shadowableLightCount) )
    {
        __debugbreak();
    }
    // mod: r_mod_forceLightKind: every light lights as that kind (1-3 sun, spot, omni; 4-6 the same with a shadow
    // map), its surfaces keeping their own type, fade and charring; the no-light entry (0) stays as it is
    // mod: r_mod_forceLightKind: 1 every primary light without its shadow map, 2 every primary light with one that may
    // have one in play (R_ModLightMayHaveShadowMap), each its own kind: with every cell drawn, each lit surface draws
    // with each technique it can have in play wherever the camera stands (zombies-hub's docker/pipelines.sh). The
    // emissive spot light after the primary lights (a muzzle flash's) keeps what it has this frame.
    const int forced = r_mod_forceLightKind ? r_mod_forceLightKind->current.integer : 0;
    const unsigned int primaryLights = forced ? Com_GetPrimaryLightCount() : 0;
    for ( primaryLightIndex = 0; primaryLightIndex < data->shadowableLightCount; ++primaryLightIndex )
    {
        lightTypea = data->shadowableLights[primaryLightIndex].type;
        lightType = lightTypea + (Com_BitCheckAssert(data->shadowableLightHasShadowMap, primaryLightIndex, 32) ? 3 : 0);
        if ( forced == 1 && primaryLightIndex < primaryLights )
            lightType = lightTypea; // mod
        else if ( forced == 2 && lightTypea && primaryLightIndex < primaryLights )
            lightType = lightTypea + (R_ModLightMayHaveShadowMap(primaryLightIndex, lightTypea) ? 3 : 0); // mod
        for ( surfType = 0; surfType < 0x10; ++surfType )
        {
            v2 = surfType < 2 || surfType == 6;
            fadeType = (GfxFadeType)v2;
            v1 = surfType == 9 || surfType == 7 || surfType == 2;
            charType = (GfxTechCharredType)v1;
            data->primaryLightTechType[surfType][0][0][primaryLightIndex] = gfxDrawMethod.litTechType[surfType][lightType][0][0];
            data->primaryLightTechType[surfType][0][1][primaryLightIndex] = gfxDrawMethod.litTechType[surfType][lightType][0][charType];
            data->primaryLightTechType[surfType][1][0][primaryLightIndex] = gfxDrawMethod.litTechType[surfType][lightType][fadeType][0];
            data->primaryLightTechType[surfType][1][1][primaryLightIndex] = gfxDrawMethod.litTechType[surfType][lightType][fadeType][charType];
        }
    }
}

// mod: whether a primary light may have a shadow map in play, so that a surface it lights draws with its kind's
// shadowed technique: the sun's where sun shadows are on (sm_sunEnable), a spot's or an omni's that may cast one where
// spot shadows are on (sm_spotEnable, sm_maxLights), as R_ChooseShadowedLights picks them. Which of them have one in a
// frame depends on where the camera is; r_mod_forceLightKind 2 gives them all one.
bool R_ModLightMayHaveShadowMap(unsigned int lightIndex, unsigned int lightType)
{
    if ( !rgp.world || !R_GetAllowShadowMaps() || lightIndex >= Com_GetPrimaryLightCount() )
        return false;
    if ( lightType == GFX_LIGHT_TYPE_DIR )
        return lightIndex == rgp.world->sunPrimaryLightIndex && sm_sunEnable->current.enabled && !CM_Mapkit_Empty();
    if ( lightType == GFX_LIGHT_TYPE_SPOT || lightType == GFX_LIGHT_TYPE_OMNI )
        return Com_GetPrimaryLight(lightIndex)->canUseShadowMap && sm_maxLights->current.integer > 0 && sm_spotEnable->current.enabled;
    return false;
}

// mod: the kinds of light the map's primary lights can light with, on one line for the log of zombies-hub's
// docker/pipelines.sh, e.g. "r_mod_lightkinds: 1 2 4 5": 1 the sun (or another directional light), 2 a spot, 3 an omni,
// and 4-6 the same with a shadow map, where the shadows this game draws are on for it (R_ModLightMayHaveShadowMap). The
// sun's kinds count without its shadow too: a surface out of its shadow's reach, or seen through a missile camera, has
// it so.
void __cdecl R_Cmd_ModLightKinds()
{
    bool kinds[GFX_LIGHT_TYPE_COUNT_WITH_SHADOWMAP_VERSIONS] = {};
    char line[64];
    unsigned int lightIndex;
    int kind;
    int len;

    if ( !rgp.world )
    {
        Com_Printf(8, "r_mod_lightkinds: no map\n");
        return;
    }
    for ( lightIndex = 1; lightIndex < rgp.world->primaryLightCount && lightIndex < Com_GetPrimaryLightCount(); ++lightIndex )
    {
        const ComPrimaryLight *light = Com_GetPrimaryLight(lightIndex);
        if ( light->type == GFX_LIGHT_TYPE_DIR || light->type == GFX_LIGHT_TYPE_SPOT || light->type == GFX_LIGHT_TYPE_OMNI )
        {
            kinds[light->type] = true;
            if ( R_ModLightMayHaveShadowMap(lightIndex, light->type) )
                kinds[light->type + 3] = true;
        }
    }
    len = Com_sprintf(line, sizeof(line), "r_mod_lightkinds:");
    for ( kind = 1; kind < GFX_LIGHT_TYPE_COUNT_WITH_SHADOWMAP_VERSIONS; ++kind )
    {
        if ( kinds[kind] && len > 0 && len < (int)sizeof(line) )
            len += Com_sprintf(line + len, sizeof(line) - len, " %i", kind);
    }
    Com_Printf(8, "%s\n", line);
}
