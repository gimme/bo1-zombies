#include "ui_atoms.h"
#include <universal/fncast.h>
#include <gfx_d3d/r_material.h>
#include <client_mp/cl_cgame_mp.h>
#include <client/cl_main.h>
#include <client/cl_cin.h>
#include <universal/com_memory.h>
#include <universal/q_shared.h>
#include <database/db_file_load.h>
#include "ui_shared.h"
#include <qcommon/common.h>

// zombies: the loading screen's picture. The loading screen is SP's connect menu (code_post_gfx's, loaded over MP's),
// which paints its level briefing with the $levelbriefing material, whose picture the SP exe set as a level loaded; this
// exe has no such code, and drew the material as it is: its own placeholder, a checkerboard. Its draws get the map's
// still (UI_SetLoadingScreenMaterial: loadscreen_<map>); the map's loading movie, while it plays, is the screen alone
// (client/cl_cin.cpp: CL_DrawLoadingMovie).
static Material *UI_LoadingScreenPicture(Material *material)
{
    const char *name = material ? Material_GetName(material) : 0;
    if ( !name || name[0] != '$' || I_stricmp(name, "$levelbriefing") )
        return material;
    if ( sharedUiInfo.loadingScreen )
        return sharedUiInfo.loadingScreen;
    return material;
}

// zombies: the map's still over the whole screen (the briefing's mapimage, its 16:9 box on a wide screen, 640x480 on a
// 4:3 one; the connect menu's level briefing) in its proportions, 16:9 (a picture the game keeps square), as large as
// the screen takes it, over black: as the loading movie, the same picture moving, is drawn (client/cl_cin.cpp:
// CL_DrawCinematicPicture). The menus' box took a 16:10 screen's height, cutting off the picture's sides, and a screen
// wider than 16:9, which the engine draws on as a 16:9 one with wider pixels (r_aspectRatio auto: R_SetWndParms), or a
// 3:2 one, as a 4:3 one, stretched it. True when it drew it so.
static bool UI_DrawLoadingScreenPictureWhole(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float w,
                float h,
                int horzAlign,
                int vertAlign,
                const float *color,
                Material *material)
{
    if ( !material || material != sharedUiInfo.loadingScreen )
        return false;
    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    const float sw = scrPlace->realViewportSize[0], sh = scrPlace->realViewportSize[1];
    if ( x > 1.0f || y > 1.0f || x + w < sw - 1.0f || y + h < sh - 1.0f )
        return false;
    float pw = sw, ph = sh;
    if ( sw * 9.0f > sh * 16.0f )
        pw = sh * 16.0f / 9.0f; // a wider screen: bars at the sides
    else
        ph = sw * 9.0f / 16.0f;
    const float black[4] = { 0.0f, 0.0f, 0.0f, color ? color[3] : 1.0f };
    CL_DrawStretchPicPhysical(0.0f, 0.0f, sw, sh, 0.0f, 0.0f, 1.0f, 1.0f, black, sharedUiInfo.assets.whiteMaterial);
    CL_DrawStretchPicPhysical((sw - pw) * 0.5f, (sh - ph) * 0.5f, pw, ph, 0.0f, 0.0f, 1.0f, 1.0f, color, material);
    return true;
}

void __cdecl UI_DrawHandlePic(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float w,
                float h,
                int horzAlign,
                int vertAlign,
                const float *color,
                Material *material)
{
    float t0; // [esp+30h] [ebp-10h]
    float t1; // [esp+34h] [ebp-Ch]
    float s1; // [esp+38h] [ebp-8h]
    float s0; // [esp+3Ch] [ebp-4h]

    material = UI_LoadingScreenPicture(material); // zombies
    if ( CL_DrawLoadingMoviePicture(material) ) // zombies: the loading movie, under the screen (client/cl_cin.cpp)
        return;
    if ( UI_DrawLoadingScreenPictureWhole(scrPlace, x, y, w, h, horzAlign, vertAlign, color, material) ) // zombies
        return;
    if ( w >= 0.0 )
    {
        s0 = 0.0f;
        s1 = 1.0f;
    }
    else
    {
        //LODWORD(w) ^= _mask__NegFloat_;
        w = -w;
        s0 = 1.0f;
        s1 = 0.0f;
    }
    if ( h >= 0.0 )
    {
        t0 = 0.0f;
        t1 = 1.0f;
    }
    else
    {
        //LODWORD(h) ^= _mask__NegFloat_;
        h = -h;
        t0 = 1.0f;
        t1 = 0.0f;
    }
    CL_DrawStretchPic(scrPlace, x, y, w, h, horzAlign, vertAlign, s0, t0, s1, t1, color, material);
}

void __cdecl UI_DrawHandlePicRotated(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float w,
                float h,
                int horzAlign,
                int vertAlign,
                const float *color,
                float angle,
                Material *material)
{
    float t0; // [esp+2Ch] [ebp-10h]
    float t1; // [esp+30h] [ebp-Ch]
    float s1; // [esp+34h] [ebp-8h]
    float s0; // [esp+38h] [ebp-4h]

    if ( w >= 0.0 )
    {
        s0 = 0.0f;
        s1 = 1.0f;
    }
    else
    {
        //LODWORD(w) ^= _mask__NegFloat_;
        w = -w;
        s0 = 1.0f;
        s1 = 0.0f;
    }
    if ( h >= 0.0 )
    {
        t0 = 0.0f;
        t1 = 1.0f;
    }
    else
    {
        //LODWORD(h) ^= _mask__NegFloat_;
        h = -h;
        t0 = 1.0f;
        t1 = 0.0f;
    }
    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    CL_DrawStretchPicPhysicalRotateXY(x, y, w, h, s0, t0, s1, t1, angle, color, material);
}

void __cdecl UI_DrawHandlePicFramed(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float w,
                float h,
                int horzAlign,
                int vertAlign,
                float frameSize,
                float frameTexSize,
                int frameSides,
                const float *color,
                Material *material)
{
    float thicknessW; // [esp+28h] [ebp-8h]
    float thicknessH; // [esp+2Ch] [ebp-4h]

    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    thicknessW = ScrPlace_ApplyW(scrPlace, frameSize, horzAlign);
    thicknessH = ScrPlace_ApplyH(scrPlace, frameSize, vertAlign);
    CL_DrawFramedPicPhysical(
        x,
        y,
        w,
        h,
        frameSize,
        (float)(thicknessH / thicknessW) * frameSize,
        frameTexSize,
        frameSides,
        color,
        material);
}

void __cdecl UI_DrawLoadBar(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float w,
                float h,
                int horzAlign,
                int vertAlign,
                const float *color,
                Material *material)
{
    double v9; // st7
    float percentDone; // [esp+3Ch] [ebp-10h]

    if ( useFastFile->current.enabled )
        v9 = (BO1_FNCAST(double (__cdecl *)(double (__cdecl *)()), UI_LoadBarProgress_FastFile))(UI_LoadBarProgress_FastFile);
    else
        v9 = (BO1_FNCAST(double (__cdecl *)(double (__cdecl *)()), UI_LoadBarProgress_LoadObj))(UI_LoadBarProgress_LoadObj);
    percentDone = v9;
    CL_DrawStretchPic(
        scrPlace,
        x,
        y,
        w * percentDone,
        h,
        horzAlign,
        vertAlign,
        0.0,
        0.0,
        percentDone,
        1.0,
        color,
        material);
}

double __cdecl UI_LoadBarProgress_LoadObj()
{
    float v2; // [esp+4h] [ebp-8h]
    float v3; // [esp+8h] [ebp-4h]

    if ( com_expectedHunkUsage <= 0 )
        return 0.0;
    v2 = (float)com_expectedHunkUsage;
    v3 = (float)Hunk_Used() / v2;
    if ( (float)(v3 - 1.0) < 0.0 )
        return v3;
    else
        return 1.0f;
}

double __cdecl UI_LoadBarProgress_FastFile()
{
    return DB_GetLoadedFraction();
}

void __cdecl UI_FillRectPhysical(float x, float y, float width, float height, const float *color)
{
    if ( sharedUiInfo.assets.whiteMaterial )
        CL_DrawStretchPicPhysical(x, y, width, height, 0.0, 0.0, 0.0, 0.0, color, sharedUiInfo.assets.whiteMaterial);
}

void __cdecl UI_FillRect(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float width,
                float height,
                int horzAlign,
                int vertAlign,
                const float *color)
{
    if ( sharedUiInfo.assets.whiteMaterial )
        CL_DrawStretchPic(
            scrPlace,
            x,
            y,
            width,
            height,
            horzAlign,
            vertAlign,
            0.0,
            0.0,
            0.0,
            0.0,
            color,
            sharedUiInfo.assets.whiteMaterial);
}

void __cdecl UI_DrawSpinner(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float width,
                float height,
                int horzAlign,
                int vertAlign,
                const float *color)
{
    CL_DrawSpinner(scrPlace, x, y, width, height, horzAlign, vertAlign, color);
}

void __cdecl UI_DrawSpinnerLoadbar(
                const ScreenPlacement *scrPlace,
                float x,
                float y,
                float width,
                float height,
                int horzAlign,
                int vertAlign,
                const float *color)
{
    double v8; // st7
    float percentDone; // [esp+28h] [ebp-4h]

    if ( useFastFile->current.enabled )
        v8 = (BO1_FNCAST(double (__cdecl *)(double (__cdecl *)()), UI_LoadBarProgress_FastFile))(UI_LoadBarProgress_FastFile);
    else
        v8 = (BO1_FNCAST(double (__cdecl *)(double (__cdecl *)()), UI_LoadBarProgress_LoadObj))(UI_LoadBarProgress_LoadObj);
    percentDone = v8;
    CL_DrawSpinnerLoadbar(scrPlace, x, y, width, height, horzAlign, vertAlign, color, percentDone);
}

