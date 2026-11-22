#include "r_shade.h"
#include "r_state.h"
#include "r_dvars.h"
#include "rb_logfile.h"
#include "rb_shade.h"
#include "r_water.h"
#include <universal/profile.h>
#ifdef BO1_WEB
#include "r_utils.h"
#endif

bool Use_R_SetVertexShaderConstantFromCode_New = true;


void __cdecl R_SetupPassPerPrimArgs(GfxCmdBufContext context)
{
    const MaterialPass *pass; // [esp+0h] [ebp-4h]

    pass = context.state->pass;
    if ( pass->perPrimArgCount )
        R_SetPassShaderPrimArguments(context, pass->perPrimArgCount, pass->localArgs);
}

void __cdecl R_SetPassShaderPrimArguments(const GfxCmdBufContext context, unsigned int argCount, const MaterialShaderArgument *arg)
{
    while ( arg->type == 3 )
    {
        if ( Use_R_SetVertexShaderConstantFromCode_New )
            R_SetVertexShaderConstantFromCode_New(context, arg);
        else
            R_SetVertexShaderConstantFromCode_Old(context, arg);
        ++arg;
        if ( !--argCount )
            return;
    }
    if ( !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 276, 0, "unreachable") )
        __debugbreak();
}

void __cdecl R_SetVertexShaderConstantFromCode_Old(GfxCmdBufContext context, const MaterialShaderArgument *routingData)
{
    const char *v2; // eax
    const char *v3; // eax
    unsigned int rowCount; // [esp+30h] [ebp-28h]
    const float *v5; // [esp+34h] [ebp-24h]
    unsigned int dest; // [esp+38h] [ebp-20h]
    IDirect3DDevice9 *device; // [esp+3Ch] [ebp-1Ch]
    int hr; // [esp+44h] [ebp-14h]
    GfxCmdBufContext source; // [esp+4Ch] [ebp-Ch] BYREF
    const float *data; // [esp+54h] [ebp-4h]

    if ( !R_IsVertexShaderConstantUpToDate(context, routingData) )
    {
        source = context;
        if ( routingData->u.codeConst.index < 0xC5u )
            data = (const float *)R_GetCodeConstant(source, routingData->u.codeConst.index);
        else
            data = (const float *)R_GetCodeMatrix(source.source, routingData->u.codeConst.index, routingData->u.codeConst.firstRow);
        rowCount = routingData->u.codeConst.rowCount;
        v5 = data;
        dest = routingData->dest;
        device = context.state->prim.device;
        if ( r_logFile && r_logFile->current.integer )
            RB_LogPrint("device->SetVertexShaderConstantF( dest, data, rowCount )\n");
        hr = device->SetVertexShaderConstantF(dest, v5, rowCount);
        if ( hr < 0 )
        {
            ++g_disableRendering;
            v2 = R_ErrorDescription(hr);
            Com_Error(
                ERR_FATAL,
                "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetVertexShaderConstantF( dest, data, "
                "rowCount ) failed: %s\n",
                123,
                v2);
        }
        if ( r_logFile->current.integer )
        {
            while ( rowCount )
            {
                v3 = va("vertex const %i: %g %g %g %g\n", dest, *v5, v5[1], v5[2], v5[3]);
                RB_LogPrint(v3);
                ++dest;
                v5 += 4;
                --rowCount;
            }
        }
    }
}

float *__cdecl R_GetCodeConstant(const GfxCmdBufContext context, unsigned int constant)
{
    iassert(context.state);
    iassert(context.source);
    bcassert(constant, CONST_SRC_CODE_COUNT_FLOAT4);

    return context.source->input.consts[constant];
}

char __cdecl R_IsVertexShaderConstantUpToDate(GfxCmdBufContext context, const MaterialShaderArgument *routingData)
{
    if ( routingData->dest >= 0x100u
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                    98,
                    0,
                    "routingData->dest doesn't index ARRAY_COUNT( context.state->vertexShaderConstState )\n\t%i not in [0, %i)",
                    routingData->dest,
                    256) )
    {
        __debugbreak();
    }
    if ( routingData->u.codeConst.index < 0xC5u )
        return R_IsShaderConstantUpToDate(
                         context.source,
                         (GfxShaderConstantState *)&context.state->vertexShaderConstState[routingData->dest],
                         routingData);
    else
        return R_IsShaderMatrixUpToDate(
                         context.source,
                         (GfxShaderConstantState *)&context.state->vertexShaderConstState[routingData->dest],
                         routingData);
}

bool __cdecl R_IsShaderMatrixUpToDate(
                const GfxCmdBufSourceState *source,
                GfxShaderConstantState *constant,
                const MaterialShaderArgument *routingData)
{
    GfxShaderConstantState newState; // [esp+4h] [ebp-10h]
    unsigned int rowCount; // [esp+Ch] [ebp-8h]

    newState.fields.codeConst = routingData->u.codeConst;
    newState.fields.version = source->matrixVersions[MATRIX_VERSIONS_INDEX(routingData->u.codeConst.index)];

    if (constant->packed == newState.packed)
        return true;

    *constant = newState;

    rowCount = routingData->u.codeConst.rowCount;
    iassert(rowCount);

    for (rowCount = rowCount - 1; rowCount; --rowCount)
    {
        ++constant;
        constant->packed = (unsigned __int64)-1;
    }

    return false;
}

bool __cdecl R_IsShaderConstantUpToDate(
                const GfxCmdBufSourceState *source,
                GfxShaderConstantState *constant,
                const MaterialShaderArgument *routingData)
{
    GfxShaderConstantState newState; // [esp+4h] [ebp-10h]

    iassert(source);

    newState.fields.codeConst = routingData->u.codeConst;
    newState.fields.version = source->constVersions[routingData->u.codeConst.index];

    if (constant->packed == newState.packed)
        return true;

    *constant = newState;

    iassert(routingData->u.codeConst.rowCount == 1);
    return false;
}

void __cdecl R_SetVertexShaderConstantFromCode_New(
    const GfxCmdBufContext context,
    const MaterialShaderArgument *routingData)
{
    const char *v2; // eax
    const char *v3; // eax
    const char *v4; // eax
    const char *v5; // eax
    //$26AC422158757CD6FC73CEC8E4188A45 *CodeConstant; // [esp+40h] [ebp-6Ch]
    unsigned int dest; // [esp+44h] [ebp-68h]
    IDirect3DDevice9 *device; // [esp+48h] [ebp-64h]
    int v11; // [esp+50h] [ebp-5Ch]
    int hr; // [esp+68h] [ebp-44h]
    unsigned int rowLoop; // [esp+70h] [ebp-3Ch]
    unsigned __int64 packed; // [esp+74h] [ebp-38h]
    unsigned __int64 *constant; // [esp+80h] [ebp-2Ch]
    unsigned __int64 version; // [esp+84h] [ebp-28h]
    unsigned int rowCount; // [esp+90h] [ebp-1Ch]
    unsigned __int64 codeConstBits; // [esp+94h] [ebp-18h]
    unsigned __int16 index; // [esp+A0h] [ebp-Ch]
    const float *data;

    bcassert(routingData->dest, ARRAY_COUNT(context.state->vertexShaderConstState));

    constant = &context.state->vertexShaderConstState[routingData->dest];
    index = routingData->u.codeConst.index;
    codeConstBits = (unsigned __int64)routingData->u.codeSampler << 32;
    
    if (index < CONST_SRC_FIRST_CODE_MATRIX)
    {
        iassert(routingData->u.codeConst.rowCount == 1);

        //packed = context.source->constVersions[index + 24] | codeConstBits;
        packed = context.source->constVersions[index] | codeConstBits;
        if (*constant != packed)
        {
            *constant = packed;
            rowCount = 1;
            data = R_GetCodeConstant(context, index);
            dest = routingData->dest;
            device = context.state->prim.device;


            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetVertexShaderConstantF( dest, data, rowCount )\n");

            v11 = device->SetVertexShaderConstantF(dest, data, rowCount);
            if (v11 < 0)
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(v11);
                Com_Error(
                    ERR_FATAL,
                    "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetVertexShaderConstantF( dest, data"
                    ", rowCount ) failed: %s\n",
                    123,
                    v4);
            }

            if (r_logFile->current.integer)
            {
                while (rowCount)
                {
                    RB_LogPrint(va("vertex const %i: %g %g %g %g\n", dest, data[0], data[1], data[2], data[3]));
                    dest++;
                    data += 4;
                    rowCount--;
                }
            }
        }
    }
    else
    {
        //version = *((unsigned __int16 *)&context.source->viewParms3D + ((index - 197) >> 2) + 1);
        version = context.source->matrixVersions[MATRIX_VERSIONS_INDEX(index)];
        packed = version | codeConstBits;
        if (*constant != packed)
        {
            *constant = packed;
            rowCount = routingData->u.codeConst.rowCount;
            iassert(rowCount);

            //for (rowLoop = rowCount - 1; rowLoop; --rowLoop)
            //{
            //    *(_DWORD *)++constant = -1;
            //    *((_DWORD *)constant + 1) = -1;
            //}
            for (rowLoop = rowCount - 1; rowLoop; --rowLoop)
            {
                ++constant;
                *constant = (unsigned __int64)-1;
            }

            data = R_GetCodeMatrix(context.source, index, routingData->u.codeConst.firstRow);
            dest = routingData->dest;

            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetVertexShaderConstantF( dest, data, rowCount )\n");
            hr = context.state->prim.device->SetVertexShaderConstantF(dest, data, rowCount);
            if (hr < 0)
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetVertexShaderConstantF( dest, data"
                    ", rowCount ) failed: %s\n",
                    123,
                    v2);
            }

            if (r_logFile->current.integer)
            {
                while (rowCount)
                {
                    RB_LogPrint(va("vertex const %i: %g %g %g %g\n", dest, data[0], data[1], data[2], data[3]));
                    ++dest;
                    data += 4;
                    --rowCount;
                }
            }
        }
    }
}

void __cdecl R_SetupPassPerObjectArgs(GfxCmdBufContext context)
{
    const MaterialPass *pass; // [esp+0h] [ebp-4h]

    pass = context.state->pass;
    if ( pass->perObjArgCount )
        R_SetPassShaderObjectArguments(context, pass->perObjArgCount, &pass->localArgs[pass->perPrimArgCount]);
}

void __cdecl R_SetPassShaderObjectArguments(const GfxCmdBufContext context, unsigned int argCount, const MaterialShaderArgument *arg)
{
    const GfxImage *image; // [esp+94h] [ebp-8h]
    unsigned __int8 samplerState; // [esp+9Bh] [ebp-1h] BYREF

    while ( arg->type == 3 )
    {
        if ( Use_R_SetVertexShaderConstantFromCode_New )
            R_SetVertexShaderConstantFromCode_New(context, arg);
        else
            R_SetVertexShaderConstantFromCode_Old(context, arg);
        ++arg;
        if ( !--argCount )
            return;
    }

    while ( arg->type == 4 )
    {
        if (context.source->input.codeImageRenderTargetControl[arg->u.codeSampler].fields.enable)
        {
            R_SetTextureSamplerCodeImageRenderTarget(
                context,
                arg->dest,
                context.source->input.codeImageRenderTargetControl[arg->u.codeSampler]);
            ++arg;
            if ( !--argCount )
                return;
        }
        else
        {
            image = R_GetTextureFromCode(context.source, arg->u.codeSampler, &samplerState);
            if ( !image )
                R_TextureFromCodeError(context, arg->u.codeSampler);
            R_SetSampler(context, arg->dest, samplerState, image);
            ++arg;
            if ( !--argCount )
                return;
        }
    }

    if ( !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 333, 0, "unreachable") )
        __debugbreak();
}

void __cdecl R_SetPassPixelShaderStableArguments(const GfxCmdBufContext context, unsigned int argCount, const MaterialShaderArgument *arg)
{
    const char *v3; // eax
    const Material *material; // [esp+14h] [ebp-8h]
    const MaterialConstantDef *constDef; // [esp+18h] [ebp-4h]

    material = context.state->material;
    while ( arg->type < 5u )
    {
        ++arg;
        if ( !--argCount )
            return;
    }
    while ( arg->type == 5 )
    {
        if ( !R_IsPixelShaderConstantUpToDate(context, arg) )
            R_SetPixelShaderConstantFromCode(context, arg);
        ++arg;
        if ( !--argCount )
            return;
    }
    constDef = material->localConstantTable;
    while ( arg->type == 6 )
    {
        while ( constDef->nameHash != arg->u.codeSampler )
        {
            if ( ++constDef == &material->localConstantTable[material->constantCount] )
            {
                v3 = va("material '%s' is missing a required named constant", material->info.name);
                if ( !Assert_MyHandler(
                                "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                                415,
                                0,
                                "%s\n\t%s",
                                "constDef != &material->constantTable[material->constantCount]",
                                v3) )
                    __debugbreak();
            }
        }
        R_SetPixelShaderConstantFromLiteral(context.state, arg->dest, constDef->literal);
        ++arg;
        if ( !--argCount )
            return;
    }
    while ( arg->type == 7 )
    {
        R_SetPixelShaderConstantFromLiteral(context.state, arg->dest, arg->u.literalConst);
        ++arg;
        if ( !--argCount )
            return;
    }
    if ( !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 428, 0, "unreachable") )
        __debugbreak();
}

void __cdecl R_SetPixelShaderConstantFromLiteral(GfxCmdBufState *state, unsigned int dest, const float *literal)
{
    if ( dest >= 0x100
        && !Assert_MyHandler(
                    "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_state.h",
                    1888,
                    0,
                    "dest doesn't index ARRAY_COUNT( state->pixelShaderConstState )\n\t%i not in [0, %i)",
                    dest,
                    256) )
    {
        __debugbreak();
    }
    LODWORD(state->pixelShaderConstState[dest]) = -1;
    HIDWORD(state->pixelShaderConstState[dest]) = -1;
    R_HW_SetPixelShaderConstant(state->prim.device, dest, literal, 1u);
}

void __cdecl R_HW_SetPixelShaderConstant(
                IDirect3DDevice9 *device,
                unsigned int dest,
                const float *data,
                unsigned int rowCount)
{
    const char *v4; // eax
    int hr; // [esp+4h] [ebp-4h]

    if ( r_logFile && r_logFile->current.integer )
        RB_LogPrint("device->SetPixelShaderConstantF( dest, data, rowCount )\n");

    hr = device->SetPixelShaderConstantF(dest, data, rowCount);
    //hr = ((int (__thiscall *)(IDirect3DDevice9 *, IDirect3DDevice9 *, unsigned int, const float *, unsigned int))device->SetPixelShaderConstantF)(
    //             device,
    //             device,
    //             dest,
    //             data,
    //             rowCount);
    if ( hr < 0 )
    {
        ++g_disableRendering;
        v4 = R_ErrorDescription(hr);
        Com_Error(
            ERR_FATAL,
            "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetPixelShaderConstantF( dest, data, row"
            "Count ) failed: %s\n",
            158,
            v4);
    }
}

int __cdecl R_IsPixelShaderConstantUpToDate(const GfxCmdBufContext context, const MaterialShaderArgument *routingData)
{
    GfxShaderConstantState newState; // [esp+4h] [ebp-10h]

    iassert(routingData->u.codeConst.rowCount == 1);
    iassert(context.source);

    newState.fields.codeConst = routingData->u.codeConst;
    newState.fields.version = context.source->constVersions[routingData->u.codeConst.index];

    iassert(newState.fields.version);

    if (context.state->pixelShaderConstState[routingData->dest] == newState.packed)
        return 1;

    context.state->pixelShaderConstState[routingData->dest] = newState.packed;

    return 0;
}

void __cdecl R_SetPixelShaderConstantFromCode(const GfxCmdBufContext context, const MaterialShaderArgument *routingData)
{
    iassert(context.source->constVersions[routingData->u.codeConst.index]);
    //bcassert(routingData->u.codeConst.index, CONST_SRC_FIRST_CODE_MATRIX);

    R_HW_SetPixelShaderConstant(
        context.state->prim.device,
        routingData->dest,
        R_GetCodeConstant(context, routingData->u.codeConst.index),
        routingData->u.codeConst.rowCount);
}

void __cdecl R_SetupPassCriticalPixelShaderArgs(GfxCmdBufContext context)
{
    const MaterialPass *pass; // [esp+0h] [ebp-4h]

    pass = context.state->pass;
    if ( pass->stableArgCount )
        R_SetPassPixelShaderStableArguments(
            context,
            pass->stableArgCount,
            &pass->localArgs[pass->perPrimArgCount + pass->perObjArgCount]);
}

void __cdecl R_SetupPassVertexShaderArgs(GfxCmdBufContext context)
{
    const MaterialPass *pass; // [esp+0h] [ebp-4h]

    pass = context.state->pass;
    if ( pass->stableArgCount )
        R_SetPassVertexShaderStableArguments(
            context,
            pass->stableArgCount,
            &pass->localArgs[pass->perPrimArgCount + pass->perObjArgCount]);
}

void __cdecl R_SetPassVertexShaderStableArguments(const GfxCmdBufContext context, unsigned int argCount, const MaterialShaderArgument *arg)
{
    while ( arg->type < 3u )
    {
        ++arg;
        if ( !--argCount )
            return;
    }
    do
    {
        if ( arg->type != 3 )
            break;
        if ( Use_R_SetVertexShaderConstantFromCode_New )
            R_SetVertexShaderConstantFromCode_New(context, arg);
        else
            R_SetVertexShaderConstantFromCode_Old(context, arg);
        ++arg;
        --argCount;
    }
    while ( argCount );
}

const MaterialTextureDef *__cdecl R_SetPixelSamplerFromMaterial(
                const GfxCmdBufContext context,
                const MaterialShaderArgument *arg,
                const MaterialTextureDef *texDef)
{
    const char *v3; // eax
    const GfxImage *image; // [esp+8h] [ebp-Ch] BYREF
    const Material *material; // [esp+Ch] [ebp-8h]

    material = context.state->material;

    while ( texDef->nameHash != arg->u.codeSampler )
    {
        if ( ++texDef == &material->textureTable[material->textureCount] )
        {
            v3 = va("material '%s' is missing a required named texture", material->info.name);
            if ( !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                            749,
                            1,
                            "%s\n\t%s",
                            "texDef != &material->textureTable[material->textureCount]",
                            v3) )
                __debugbreak();
        }
    }
    if ( texDef->semantic == 11 )
    {
        //image = (const GfxImage *)texDef->u.image[1].cardMemory.platform[0];
        image = texDef->u.water->image;
        R_UploadWaterTexture(texDef->u.water, context.source->sceneDef.floatTime);
    }
    else
    {
        image = texDef->u.image;
    }

    if ( rg.hasAnyImageOverrides )
        R_OverrideImage((GfxImage **)&image, texDef);

    /// removed mature content hack
    //if ( texDef->isMatureContent && context.source->input.data->hideMatureContent && (unsigned __int8)((unsigned __int8)IDA_HIBYTE(material->info.layeredSurfaceTypes) >> 5) > 1u )
    //{
    //    image = rgp.blankImage;
    //}

    R_SetSampler(context, arg->dest, texDef->samplerState, image);
    return texDef;
}

void __cdecl R_OverrideImage(GfxImage **image, const MaterialTextureDef *texdef)
{
    const char *v2; // eax
    unsigned __int8 semantic; // [esp+7h] [ebp-1h]

    if ( !image && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 672, 0, "%s", "image") )
        __debugbreak();
    if ( !texdef && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 673, 0, "%s", "texdef") )
        __debugbreak();
    if ( (*image)->mapType == 3 )
    {
        semantic = texdef->semantic;
        switch ( semantic )
        {
            case 0u:
            case 1u:
            case 0xBu:
            case 0x1Au:
            case 0x1Bu:
                return;
            case 2u:
            case 0xCu:
            case 0xDu:
            case 0xEu:
            case 0xFu:
            case 0x10u:
            case 0x11u:
            case 0x12u:
            case 0x13u:
            case 0x14u:
            case 0x15u:
            case 0x16u:
            case 0x17u:
            case 0x18u:
            case 0x19u:
            case 0x1Cu:
                if ( r_colorMap->current.integer != 1 )
                    *image = (GfxImage *)R_OverrideGrayscaleImage(r_colorMap);
                break;
            case 5u:
                if ( r_normalMap->current.integer )
                {
                    if ( r_normalMap->current.integer != 1
                        && !Assert_MyHandler(
                                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                                    722,
                                    0,
                                    "%s\n\t(r_normalMap->current.integer) = %i",
                                    "(r_normalMap->current.integer == R_NORMAL_OVERRIDE_NONE)",
                                    r_normalMap->current.integer) )
                    {
                        __debugbreak();
                    }
                }
                else
                {
                    *image = rgp.identityNormalMapImage;
                }
                break;
            case 8u:
                if ( r_specularRoughnessMap->current.integer )
                {
                    switch ( r_specularRoughnessMap->current.integer )
                    {
                        case 1:
                            *image = rgp.blackTransparentImage;
                            break;
                        case 2:
                            *image = rgp.blackImage;
                            break;
                        case 3:
                            *image = rgp.whiteTransparentImage;
                            break;
                        case 4:
                            *image = rgp.whiteImage;
                            break;
                    }
                }
                break;
            default:
                v2 = va("unhandled case %i", semantic);
                if ( !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 733, 1, v2) )
                    __debugbreak();
                break;
        }
    }
}

void __cdecl R_SetPassShaderStableArguments(
                const GfxCmdBufContext context,
                unsigned int argCount,
                const MaterialShaderArgument *arg)
{
    const char *v3; // eax
    const GfxImage *image; // [esp+C4h] [ebp-14h]
    const Material *material; // [esp+C8h] [ebp-10h]
    const MaterialTextureDef *texDef; // [esp+CCh] [ebp-Ch]
    unsigned __int8 samplerState; // [esp+D3h] [ebp-5h] BYREF
    const MaterialConstantDef *constDef; // [esp+D4h] [ebp-4h]

    material = context.state->material;
    constDef = material->localConstantTable;
    while ( !arg->type )
    {
        while ( constDef->nameHash != arg->u.codeSampler )
        {
            if ( ++constDef == &material->localConstantTable[material->constantCount] )
            {
                v3 = va("material '%s' is missing a required named constant", material->info.name);
                if ( !Assert_MyHandler(
                                "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                                826,
                                0,
                                "%s\n\t%s",
                                "constDef != &material->constantTable[material->constantCount]",
                                v3) )
                    __debugbreak();
            }
        }
        R_SetVertexShaderConstantFromLiteral(context.state, arg->dest, constDef->literal);
        ++arg;
        if ( !--argCount )
            return;
    }
    while ( arg->type == 1 )
    {
        R_SetVertexShaderConstantFromLiteral(context.state, arg->dest, arg->u.literalConst);
        ++arg;
        if ( !--argCount )
            return;
    }
    texDef = material->textureTable;

    while ( arg->type == 2 )
    {
        texDef = R_SetPixelSamplerFromMaterial(context, arg++, texDef);
        if ( !--argCount )
            return;
    }

    while ( arg->type == 3 )
    {
        if ( Use_R_SetVertexShaderConstantFromCode_New )
            R_SetVertexShaderConstantFromCode_New(context, arg);
        else
            R_SetVertexShaderConstantFromCode_Old(context, arg);
        ++arg;
        if ( !--argCount )
            return;
    }

    while ( arg->type == 4 )
    {
        if ( ((*(_QWORD *)&context.source->input.codeImageRenderTargetControl[arg->u.codeSampler].fields >> 8) & 1) != 0 )
        {
            R_SetTextureSamplerCodeImageRenderTarget(
                context,
                arg->dest,
                context.source->input.codeImageRenderTargetControl[arg->u.codeSampler]);
            ++arg;
            if ( !--argCount )
                return;
        }
        else
        {
            image = R_GetTextureFromCode(context.source, arg->u.codeSampler, &samplerState);
            if ( !image )
                R_TextureFromCodeError(context, arg->u.codeSampler);
            R_SetSampler(context, arg->dest, samplerState, image);
            ++arg;
            if ( !--argCount )
                return;
        }
    }
}

void __cdecl R_SetVertexShaderConstantFromLiteral(GfxCmdBufState *state, unsigned int dest, const float *literal)
{
    const char *v3; // eax
    const char *v4; // eax
    int v5; // [esp+30h] [ebp-18h]
    const float *v6; // [esp+34h] [ebp-14h]
    unsigned int v7; // [esp+38h] [ebp-10h]
    IDirect3DDevice9 *device; // [esp+3Ch] [ebp-Ch]
    int hr; // [esp+44h] [ebp-4h]

    if ( dest >= 0x100
        && !Assert_MyHandler(
                    "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_state.h",
                    1878,
                    0,
                    "dest doesn't index ARRAY_COUNT( state->vertexShaderConstState )\n\t%i not in [0, %i)",
                    dest,
                    256) )
    {
        __debugbreak();
    }
    LODWORD(state->vertexShaderConstState[dest]) = -1;
    HIDWORD(state->vertexShaderConstState[dest]) = -1;
    v5 = 1;
    v6 = literal;
    v7 = dest;
    device = state->prim.device;
    if ( r_logFile && r_logFile->current.integer )
        RB_LogPrint("device->SetVertexShaderConstantF( dest, data, rowCount )\n");
    hr = device->SetVertexShaderConstantF(dest, literal, 1u);
    if ( hr < 0 )
    {
        ++g_disableRendering;
        v3 = R_ErrorDescription(hr);
        Com_Error(
            ERR_FATAL,
            "c:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetVertexShaderConstantF( dest, data, ro"
            "wCount ) failed: %s\n",
            123,
            v3);
    }
    if ( r_logFile->current.integer )
    {
        while ( v5 )
        {
            v4 = va("vertex const %i: %g %g %g %g\n", v7, *v6, v6[1], v6[2], v6[3]);
            RB_LogPrint(v4);
            ++v7;
            v6 += 4;
            --v5;
        }
    }
}

int __cdecl R_ReserveIndexData(GfxCmdBufPrimState *state, int triCount)
{
    unsigned int v2; // ecx
    int indexCount; // [esp+8h] [ebp-4h]

    indexCount = 3 * triCount;
    if ( 3 * triCount > gfxBuf.dynamicIndexBuffer->total
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                    937,
                    0,
                    "%s\n\t(indexCount) = %i",
                    "(indexCount <= gfxBuf.dynamicIndexBuffer->total)",
                    indexCount) )
    {
        __debugbreak();
    }
    if ( indexCount + gfxBuf.dynamicIndexBuffer->used > gfxBuf.dynamicIndexBuffer->total )
        gfxBuf.dynamicIndexBuffer->used = 0;
    if ( !gfxBuf.dynamicIndexBuffer->used )
    {
        v2 = (gfxBuf.dynamicIndexBuffer - gfxBuf.dynamicIndexBufferPool + 1) & 0x80000000;
        if ( gfxBuf.dynamicIndexBuffer - gfxBuf.dynamicIndexBufferPool + 1 < 0 )
            v2 = 0;
        gfxBuf.dynamicIndexBuffer = &gfxBuf.dynamicIndexBufferPool[v2];
    }
    return gfxBuf.dynamicIndexBuffer->used;
}

int __cdecl R_SetIndexData(GfxCmdBufPrimState *state, unsigned __int8 *indices, int triCount)
{
    unsigned __int8 *v3; // eax
    int baseIndex; // [esp+0h] [ebp-18h]
    unsigned int indexDataSize; // [esp+4h] [ebp-14h]
    IDirect3DIndexBuffer9 *ib; // [esp+10h] [ebp-8h]
    unsigned __int16 *bufferData; // [esp+14h] [ebp-4h]

    baseIndex = R_ReserveIndexData(state, triCount);
    indexDataSize = 6 * triCount;
    ib = gfxBuf.dynamicIndexBuffer->buffer;
    if ( !ib && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 981, 0, "%s", "ib") )
        __debugbreak();
    if ( gfxBuf.dynamicIndexBuffer->used )
        v3 = (unsigned __int8 *)R_LockIndexBuffer(ib, 2 * gfxBuf.dynamicIndexBuffer->used, indexDataSize, 0x1000u);
    else
        v3 = (unsigned __int8 *)R_LockIndexBuffer(ib, 2 * gfxBuf.dynamicIndexBuffer->used, indexDataSize, 0x2000u);
    bufferData = (unsigned __int16 *)v3;
    if ( v3 )
        memcpy(v3, indices, indexDataSize);
    R_UnlockIndexBuffer(ib);
    if ( state->indexBuffer != ib )
        R_ChangeIndices(state, ib);
    if ( bufferData )
        gfxBuf.dynamicIndexBuffer->used += 3 * triCount;
    return baseIndex;
}

void __cdecl R_SetPixelShader(GfxCmdBufState *state, const MaterialPixelShader *pixelShader)
{
    if ( !pixelShader
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 1039, 0, "%s", "pixelShader") )
    {
        __debugbreak();
    }
    if ( !pixelShader->prog.ps
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                    1041,
                    0,
                    "%s\n\t(pixelShader->name) = %s",
                    "(pixelShader->prog.ps)",
                    pixelShader->name) )
    {
        __debugbreak();
    }
    if ( state->pixelShader != pixelShader )
    {
        R_HW_SetPixelShader(state->prim.device, pixelShader);
        state->pixelShader = pixelShader;
    }
}

void __cdecl R_UpdateVertexDecl(GfxCmdBufState *state)
{
    const MaterialPass *pass; // [esp+14h] [ebp-8h]
    const MaterialVertexShader *vertexShader; // [esp+18h] [ebp-4h]

    pass = state->pass;
    if ( !pass->vertexDecl
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 1299, 0, "%s", "pass->vertexDecl") )
    {
        __debugbreak();
    }
    vertexShader = pass->vertexShader;
    if ( !vertexShader
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 1331, 0, "%s", "vertexShader") )
    {
        __debugbreak();
    }
    R_SetVertexDecl(&state->prim, pass->vertexDecl);
    if ( !pass->pixelShader
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 1337, 0, "%s", "pass->pixelShader") )
    {
        __debugbreak();
    }
    if ( pass->vertexDecl == (MaterialVertexDeclaration *)-36
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                    1340,
                    0,
                    "%s",
                    "pass->vertexDecl->routing.decl") )
    {
        __debugbreak();
    }
    if ( !pass->vertexDecl->routing.decl[state->prim.vertDeclType] )
        Com_Error(
            ERR_FATAL,
            "Vertex type %i doesn't have the information used by shader %s in material %s\n",
            state->prim.vertDeclType,
            pass->vertexShader->name,
            state->material->info.name);
    R_SetVertexShader(state, vertexShader);
}

void __cdecl R_SetVertexShader(GfxCmdBufState *state, const MaterialVertexShader *vertexShader)
{
    if ( state->vertexShader != vertexShader )
    {
        if ( !vertexShader
            && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp", 1178, 0, "%s", "vertexShader") )
        {
            __debugbreak();
        }
        if ( !vertexShader->prog.vs
            && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                        1180,
                        0,
                        "%s\n\t(vertexShader->name) = %s",
                        "(vertexShader->prog.vs)",
                        vertexShader->name) )
        {
            __debugbreak();
        }
        R_HW_SetVertexShader(state->prim.device, vertexShader);
        state->vertexShader = vertexShader;
    }
}

#ifdef BO1_WEB
// web: the WebGPU executor draws what a pipeline still compiling would with a stand-in (web/src/d3d9_webgpu.cpp:
// stand-ins), a generated pixel shader that lights the pass's colour map with its lightmaps or primary light. Which of a
// pass's sampler stages and constant registers those are, only its arguments say (the shaders name stages and
// registers), so each pass tells the shim as it is set up, when it differs from the last told (web/src/d3d9_cmd.h:
// CmdPassHints, whose indices these are), with the material's and the technique's names and the kind of surface
// (g_bo1SurfType), which name the draw in the executor's log of waits for pipelines.
void Bo1_PassHints(const unsigned char *hints, const char *material, const char *technique, unsigned char surfType); // web/src/d3d9_device.cpp

// a code sampler of the scene's colour copy: distortion and the post effects read it, and a stand-in can't draw for them
static bool R_IsSceneColorCodeSampler(unsigned int codeSampler)
{
    switch ( codeSampler )
    {
    case TEXTURE_SRC_CODE_RESOLVED_POST_SUN:
    case TEXTURE_SRC_CODE_RESOLVED_SCENE:
    case TEXTURE_SRC_CODE_POST_EFFECT_SRC:
    case TEXTURE_SRC_CODE_POST_EFFECT_GODRAYS:
    case TEXTURE_SRC_CODE_POST_EFFECT_0:
    case TEXTURE_SRC_CODE_POST_EFFECT_1:
        return true;
    default:
        return false;
    }
}

// a code sampler of the depth buffer's copy: most smoke and fire read it to fade where they near a surface (feathering),
// as do water and depth of field; a stand-in draws them without that
static bool R_IsDepthCopyCodeSampler(unsigned int codeSampler)
{
    switch ( codeSampler )
    {
    case TEXTURE_SRC_CODE_FLOATZ:
    case TEXTURE_SRC_CODE_PROCESSED_FLOATZ:
    case TEXTURE_SRC_CODE_RAW_FLOATZ:
    case TEXTURE_SRC_CODE_FEATHER_FLOAT_Z:
        return true;
    default:
        return false;
    }
}

// web: the materials of the zone whose passes read the scene's colour copy, which the executor has no stand-in for (it
// compiles the map's list's pipelines of them before the player goes in instead), and those that read only the depth
// buffer's copy, whose stand-ins draw without the fade, counted once the materials are sorted (R_BeginFrame), for the log
void R_LogSceneReaders()
{
    int colorMaterials = 0, colorPasses = 0, depthMaterials = 0, depthPasses = 0;
    for ( int m = 0; m < rgp.materialCount; ++m )
    {
        const Material *material = rgp.sortedMaterials[m];
        if ( !material || !material->techniqueSet )
            continue;
        bool color = false, depth = false;
        for ( int t = 0; t < 130; ++t )
        {
            const MaterialTechnique *tech = material->techniqueSet->techniques[t];
            if ( !tech )
                continue;
            for ( unsigned int p = 0; p < tech->passCount; ++p )
            {
                const MaterialPass *pass = &tech->passArray[p];
                const MaterialShaderArgument *arg = pass->localArgs;
                unsigned int count = pass->perPrimArgCount + pass->perObjArgCount + pass->stableArgCount;
                bool passColor = false, passDepth = false;
                for ( unsigned int i = 0; i < count; ++i, ++arg )
                {
                    if ( arg->type != 4 || arg->dest >= 16 )
                        continue;
                    passColor = passColor || R_IsSceneColorCodeSampler(arg->u.codeSampler);
                    passDepth = passDepth || R_IsDepthCopyCodeSampler(arg->u.codeSampler);
                }
                colorPasses += passColor;
                depthPasses += passDepth && !passColor;
                color = color || passColor;
                depth = depth || passDepth;
            }
        }
        colorMaterials += color;
        depthMaterials += depth && !color;
    }
    Com_Printf(8, "R_LogSceneReaders: %d of %d materials read the scene's colour copy, in %d passes of their techniques (no stand-in draws for those); "
        "%d more read only the depth buffer's copy, in %d passes (their stand-ins draw without the fade)\n",
        colorMaterials, rgp.materialCount, colorPasses, depthMaterials, depthPasses);
}

unsigned char g_bo1SurfType = 16;

static void R_TellPassHints(const MaterialPass *pass, const Material *material, const MaterialTechnique *technique)
{
    enum
    {
        HINT_COLOR_MAP, HINT_LIGHTMAP_PRIMARY, HINT_LIGHTMAP_SECONDARY, HINT_PS_LIGHT_POSITION, HINT_PS_LIGHT_DIFFUSE,
        HINT_VS_LIGHT_POSITION, HINT_VS_LIGHT_DIFFUSE, HINT_FLAGS, HINT_MODEL_LIGHTING, HINT_PS_LOOKUP_SCALE, HINT_PS_HDR,
        HINT_VS_BASE_LIGHTING, HINT_PS_HERO_SCALE, HINT_SHADOW_SUN, HINT_VS_SHADOW_MATRIX, HINT_PS_SHADOW_PARTITION, HINT_COUNT = 16,
        HINT_READS_SCENE = 1, HINT_READS_DEPTH = 2
    };
    static unsigned char last[HINT_COUNT];
    static const MaterialPass *lastPass;
    static const Material *lastMaterial;
    static unsigned char lastSurfType;
    static bool told;
    static unsigned int colorMapHash;
    if ( !colorMapHash )
        colorMapHash = R_HashString("colorMap");
    unsigned char hints[HINT_COUNT];
    memset(hints, 255, sizeof(hints));
    hints[HINT_FLAGS] = 0;
    unsigned char colorBySemantic = 255;
    unsigned char sun[4] = {255, 255, 255, 255}; // the sun's position and diffuse colour, the pixel shader's then the vertex shader's
    const MaterialShaderArgument *arg = pass->localArgs;
    unsigned int count = pass->perPrimArgCount + pass->perObjArgCount + pass->stableArgCount;
    for ( unsigned int i = 0; i < count; ++i, ++arg )
    {
        switch ( arg->type )
        {
        case 2: // MTL_ARG_MATERIAL_PIXEL_SAMPLER: a texture of the material, by name
            if ( arg->dest >= 16 )
                break;
            if ( arg->u.nameHash == colorMapHash )
                hints[HINT_COLOR_MAP] = (unsigned char)arg->dest;
            else if ( colorBySemantic == 255 ) // a colour map by its semantic under another name (a character's skin)
                for ( unsigned int t = 0; t < material->textureCount; ++t )
                    if ( material->textureTable[t].nameHash == arg->u.nameHash && material->textureTable[t].semantic == 2 ) // TS_COLOR_MAP
                    {
                        colorBySemantic = (unsigned char)arg->dest;
                        break;
                    }
            break;
        case 4: // MTL_ARG_CODE_PIXEL_SAMPLER: a texture of the code's (MaterialTextureSource)
            if ( arg->dest >= 16 )
                break;
            switch ( arg->u.codeSampler )
            {
            case TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY:
                hints[HINT_LIGHTMAP_PRIMARY] = (unsigned char)arg->dest;
                break;
            case TEXTURE_SRC_CODE_LIGHTMAP_SECONDARY:
                hints[HINT_LIGHTMAP_SECONDARY] = (unsigned char)arg->dest;
                break;
            case TEXTURE_SRC_CODE_MODEL_LIGHTING: // a model's light from the light grid (r_model_lighting.cpp)
                hints[HINT_MODEL_LIGHTING] = (unsigned char)arg->dest;
                break;
            case TEXTURE_SRC_CODE_SHADOWMAP_SUN:
                hints[HINT_SHADOW_SUN] = (unsigned char)arg->dest;
                break;
            default:
                if ( R_IsSceneColorCodeSampler(arg->u.codeSampler) )
                    hints[HINT_FLAGS] |= HINT_READS_SCENE;
                else if ( R_IsDepthCopyCodeSampler(arg->u.codeSampler) )
                    hints[HINT_FLAGS] |= HINT_READS_DEPTH;
                break;
            }
            break;
        case 3: // MTL_ARG_CODE_VERTEX_CONST
        case 5: // MTL_ARG_CODE_PIXEL_CONST
            if ( arg->type == 3 && arg->u.codeConst.index == CONST_SRC_CODE_TRANSPOSE_SHADOW_LOOKUP_MATRIX && arg->dest < 255 )
                hints[HINT_VS_SHADOW_MATRIX] = (unsigned char)arg->dest; // four rows: the sun shadow map's coordinates
            if ( arg->u.codeConst.rowCount == 1 && arg->dest < 255 )
            {
                int at = arg->type == 3 ? HINT_VS_LIGHT_POSITION : HINT_PS_LIGHT_POSITION;
                switch ( arg->u.codeConst.index )
                {
                case CONST_SRC_CODE_LIGHT_POSITION:
                    hints[at] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_LIGHT_DIFFUSE:
                    hints[at + 1] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_SUN_POSITION: // the sun lights models in their light-probe techniques
                    sun[arg->type == 3 ? 2 : 0] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_SUN_DIFFUSE:
                    sun[arg->type == 3 ? 3 : 1] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_LIGHTING_LOOKUP_SCALE:
                    if ( arg->type == 5 )
                        hints[HINT_PS_LOOKUP_SCALE] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_HDRCONTROL_0: // its x scales the scene's light before the shaders' final square root
                    if ( arg->type == 5 )
                        hints[HINT_PS_HDR] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_BASE_LIGHTING_COORDS:
                    if ( arg->type == 3 )
                        hints[HINT_VS_BASE_LIGHTING] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_LIGHT_HERO_SCALE: // its x scales the primary light on models
                    if ( arg->type == 5 )
                        hints[HINT_PS_HERO_SCALE] = (unsigned char)arg->dest;
                    break;
                case CONST_SRC_CODE_SHADOWMAP_SWITCH_PARTITION: // the sun shadow map's far partition's place
                    if ( arg->type == 5 )
                        hints[HINT_PS_SHADOW_PARTITION] = (unsigned char)arg->dest;
                    break;
                default:
                    break;
                }
            }
            break;
        default:
            break;
        }
    }
    if ( hints[HINT_COLOR_MAP] == 255 )
        hints[HINT_COLOR_MAP] = colorBySemantic;
    // the sun where the pass has no primary light of its own (a model's light-probe technique lit by the sun)
    if ( hints[HINT_PS_LIGHT_POSITION] == 255 && hints[HINT_PS_LIGHT_DIFFUSE] == 255 )
    {
        hints[HINT_PS_LIGHT_POSITION] = sun[0];
        hints[HINT_PS_LIGHT_DIFFUSE] = sun[1];
    }
    if ( hints[HINT_VS_LIGHT_POSITION] == 255 && hints[HINT_VS_LIGHT_DIFFUSE] == 255 )
    {
        hints[HINT_VS_LIGHT_POSITION] = sun[2];
        hints[HINT_VS_LIGHT_DIFFUSE] = sun[3];
    }
    if ( told && pass == lastPass && material == lastMaterial && g_bo1SurfType == lastSurfType && !memcmp(hints, last, sizeof(hints)) )
        return;
    memcpy(last, hints, sizeof(hints));
    lastPass = pass;
    lastMaterial = material;
    lastSurfType = g_bo1SurfType;
    told = true;
    Bo1_PassHints(hints, material->info.name, technique && technique->name ? technique->name : "", g_bo1SurfType);
}
#endif

void __cdecl R_SetupPass(GfxCmdBufContext context, unsigned int passIndex)
{
    PROF_SCOPED("R_SetupPass");

    const MaterialPass *pass; // [esp+0h] [ebp-14h]
    const Material *material; // [esp+4h] [ebp-10h]
    const GfxStateBits *refStateBits; // [esp+8h] [ebp-Ch]
    unsigned int stateBits[2]; // [esp+Ch] [ebp-8h] BYREF

    pass = &context.state->technique->passArray[passIndex];
    context.state->pass = pass;
    context.state->passIndex = passIndex;
    material = context.state->material;
    if ( material->stateBitsEntry[context.state->techType] >= (unsigned int)material->stateBitsCount
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_shade.cpp",
                    1376,
                    0,
                    "material->stateBitsEntry[context.local.state->techType] doesn't index material->stateBitsCount\n"
                    "\t%i not in [0, %i)",
                    material->stateBitsEntry[context.state->techType],
                    material->stateBitsCount) )
    {
        __debugbreak();
    }
    refStateBits = &material->stateBitsTable[passIndex + material->stateBitsEntry[context.state->techType]];
    stateBits[0] = refStateBits->loadBits[0];
    stateBits[1] = refStateBits->loadBits[1];
    R_SetState(context.state, stateBits);
    R_SetPixelShader(context.state, pass->pixelShader);
#ifdef BO1_WEB
    R_TellPassHints(pass, material, context.state->technique);
#endif
    if ( g_bo1ProfOn ) // p1: bo1_frameperf first-use counts (measurement only)
    {
        char name[128];
        _snprintf_s(name, _TRUNCATE, "%s:%s", material->info.name, context.state->technique->name ? context.state->technique->name : "?");
        BO1_ProfFirstUse(BO1_FIRST_PASS, (unsigned long long)(size_t)pass, name);
        BO1_ProfFirstUse(BO1_FIRST_VS, (unsigned long long)(size_t)pass->vertexShader, 0);
        BO1_ProfFirstUse(BO1_FIRST_PS, (unsigned long long)(size_t)pass->pixelShader, 0);
        BO1_ProfFirstUse(BO1_FIRST_PAIR, ((unsigned long long)(size_t)pass->vertexShader << 32) | (size_t)pass->pixelShader, 0);
        const unsigned int comboParts[5] = { (unsigned int)(size_t)pass->vertexShader, (unsigned int)(size_t)pass->pixelShader,
            (unsigned int)(size_t)(pass->vertexDecl ? pass->vertexDecl->routing.decl[context.state->prim.vertDeclType] : 0),
            stateBits[0], stateBits[1] };
        unsigned long long comboKey = 14695981039346656037ull;
        for (unsigned int part : comboParts)
            comboKey = (comboKey ^ part) * 1099511628211ull;
        BO1_ProfFirstUse(BO1_FIRST_COMBO, comboKey, 0);
    }
    if ( pass->stableArgCount )
        R_SetPassShaderStableArguments(context, pass->stableArgCount, &pass->localArgs[pass->perPrimArgCount + pass->perObjArgCount]);
}

void __cdecl R_SetState(GfxCmdBufState *state, unsigned int *stateBits)
{
    if ( *stateBits != state->refStateBits[0] )
    {
        R_ChangeState_0(state, *stateBits);
        state->refStateBits[0] = *stateBits;
    }
    if ( stateBits[1] != state->refStateBits[1] )
    {
        R_ChangeState_1(state, stateBits[1]);
        state->refStateBits[1] = stateBits[1];
    }
}

int __cdecl R_SetVertexData(GfxCmdBufState *state, const void *data, int vertexCount, int stride)
{
    IDirect3DVertexBuffer9 *vb; // [esp+0h] [ebp-14h]
    volatile int vertexOffset; // [esp+4h] [ebp-10h]
    void *bufferData; // [esp+Ch] [ebp-8h]
    int totalSize; // [esp+10h] [ebp-4h]

    iassert(vertexCount > 0);
    totalSize = stride * vertexCount;

    iassert(totalSize <= gfxBuf.dynamicVertexBuffer->total);

    iassert(gfxBuf.dynamicVertexBuffer->used + totalSize <= gfxBuf.dynamicVertexBuffer->total);
    
    vb = gfxBuf.dynamicVertexBuffer->buffer;
    iassert(vb);

    bufferData = R_LockVertexBuffer(
                                 vb,
                                 gfxBuf.dynamicVertexBuffer->used,
                                 totalSize,
                                 gfxBuf.dynamicVertexBuffer->used != 0 ? 4096 : 0x2000);

    iassert(bufferData);
    
    if ( bufferData )
        Com_Memcpy(bufferData, data, totalSize);
    R_UnlockVertexBuffer(vb);
    vertexOffset = gfxBuf.dynamicVertexBuffer->used;
    gfxBuf.dynamicVertexBuffer->used += totalSize;
    return vertexOffset;
}

