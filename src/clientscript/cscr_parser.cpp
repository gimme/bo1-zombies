#include "cscr_parser.h"
#include "cscr_variable.h"
#include <universal/assertive.h>
#include <cstring>
#include "cscr_compiler.h"
#include <win32/win_net.h>
#include <universal/com_memory.h>
#include <universal/com_shared.h>
#include <universal/com_files.h>
#include <universal/q_shared.h>
#include <zlib/zlib.h>
#include <qcommon/common.h>
#include <monkey/monkey.h>
#include "cscr_parsetree.h"
#include "cscr_stringlist.h"
#include "cscr_instance.h"
#include "cscr_vm.h"

scrParserPub_t gScrParserPub[2];
scrParserGlob_t gScrParserGlob[2];

char g_EndPos;

thread_local bool g_loadedImpureScript = false;

void __cdecl Scr_InitOpcodeLookup(scriptInstance_t inst)
{
    if (gScrParserGlob[inst].opcodeLookup
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            94,
            0,
            "%s",
            "!gScrParserGlob[inst].opcodeLookup"))
    {
        __debugbreak();
    }
    if (gScrParserGlob[inst].sourcePosLookup
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            95,
            0,
            "%s",
            "!gScrParserGlob[inst].sourcePosLookup"))
    {
        __debugbreak();
    }
    if (gScrParserPub[inst].sourceBufferLookup
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            96,
            0,
            "%s",
            "!gScrParserPub[inst].sourceBufferLookup"))
    {
        __debugbreak();
    }
    if (gScrVarPub[inst].developer)
    {
        gScrParserGlob[inst].delayedSourceIndex = -1;
        gScrParserGlob[inst].opcodeLookupMaxLen = inst != SCRIPTINSTANCE_CLIENT ? 0x40000 : 0x4000;
        gScrParserGlob[inst].opcodeLookupLen = 0;
        gScrParserGlob[inst].opcodeLookup = (OpcodeLookup *)Hunk_UserAlloc(
            g_DebugHunkUser,
            12 * gScrParserGlob[inst].opcodeLookupMaxLen,
            4,
            "Scr_InitOpcodeLookup");
        memset((unsigned __int8 *)gScrParserGlob[inst].opcodeLookup, 0, 12 * gScrParserGlob[inst].opcodeLookupMaxLen);
        gScrParserGlob[inst].sourcePosLookupMaxLen = inst != SCRIPTINSTANCE_CLIENT ? 393216 : 16;
        gScrParserGlob[inst].sourcePosLookupLen = 0;
        gScrParserGlob[inst].sourcePosLookup = (SourceLookup *)Hunk_UserAlloc(
            g_DebugHunkUser,
            8 * gScrParserGlob[inst].sourcePosLookupMaxLen,
            4,
            "Scr_InitOpcodeLookup");
        memset((unsigned __int8 *)gScrParserGlob[inst].sourcePosLookup, 0, 8 * gScrParserGlob[inst].sourcePosLookupMaxLen);
        gScrParserGlob[inst].currentCodePos = 0;
        gScrParserGlob[inst].currentSourcePosCount = 0;
        gScrParserGlob[inst].sourceBufferLookupMaxLen = inst != SCRIPTINSTANCE_CLIENT ? 256 : 16;
        gScrParserPub[inst].sourceBufferLookupLen = 0;
        gScrParserPub[inst].sourceBufferLookup = (SourceBufferInfo *)Hunk_UserAlloc(
            g_DebugHunkUser,
            24 * gScrParserGlob[inst].sourceBufferLookupMaxLen,
            4,
            "Scr_InitOpcodeLookup");
        memset(
            (unsigned __int8 *)gScrParserPub[inst].sourceBufferLookup,
            0,
            24 * gScrParserGlob[inst].sourceBufferLookupMaxLen);
    }
}

void __cdecl Scr_ShutdownOpcodeLookup(scriptInstance_t inst)
{
    unsigned int i; // [esp+0h] [ebp-4h]
    unsigned int ia; // [esp+0h] [ebp-4h]

    if (gScrParserGlob[inst].opcodeLookup)
    {
        Hunk_UserFree(g_DebugHunkUser, gScrParserGlob[inst].opcodeLookup);
        gScrParserGlob[inst].opcodeLookup = 0;
    }
    if (gScrParserGlob[inst].sourcePosLookup)
    {
        Hunk_UserFree(g_DebugHunkUser, gScrParserGlob[inst].sourcePosLookup);
        gScrParserGlob[inst].sourcePosLookup = 0;
    }
    if (gScrParserPub[inst].sourceBufferLookup)
    {
        for (i = 0; i < gScrParserPub[inst].sourceBufferLookupLen; ++i)
            Hunk_UserFree(g_DebugHunkUser, gScrParserPub[inst].sourceBufferLookup[i].buf);
        Hunk_UserFree(g_DebugHunkUser, gScrParserPub[inst].sourceBufferLookup);
        gScrParserPub[inst].sourceBufferLookup = 0;
    }
    if (gScrParserGlob[inst].saveSourceBufferLookup)
    {
        for (ia = 0; ia < gScrParserGlob[inst].saveSourceBufferLookupLen; ++ia)
        {
            if (gScrParserGlob[inst].saveSourceBufferLookup[ia].sourceBuf)
                Hunk_UserFree(g_DebugHunkUser, gScrParserGlob[inst].saveSourceBufferLookup[ia].sourceBuf);
        }
        Hunk_UserFree(g_DebugHunkUser, gScrParserGlob[inst].saveSourceBufferLookup);
        gScrParserGlob[inst].saveSourceBufferLookup = 0;
    }
}

void __cdecl AddOpcodePos(scriptInstance_t inst, unsigned int sourcePos, int type)
{
    unsigned int sourcePosLookupIndex; // [esp+0h] [ebp-14h]
    OpcodeLookup *opcodeLookup; // [esp+4h] [ebp-10h]
    SourceLookup *sourcePosLookup; // [esp+8h] [ebp-Ch]
    unsigned __int8 *newSourcePosLookup; // [esp+Ch] [ebp-8h]
    unsigned __int8 *newOpcodeLookup; // [esp+10h] [ebp-4h]

    if (gScrVarPub[inst].developer)
    {
        if (gScrCompilePub[inst].developer_statement == 2)
        {
            if (gScrVarPub[inst].developer_script
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    177,
                    0,
                    "%s",
                    "!gScrVarPub[inst].developer_script"))
            {
                __debugbreak();
            }
        }
        else if (gScrCompilePub[inst].developer_statement != 3)
        {
            if (!gScrCompilePub[inst].allowedBreakpoint)
                type &= ~1u;
            if (!gScrParserGlob[inst].opcodeLookup
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    189,
                    0,
                    "%s",
                    "gScrParserGlob[inst].opcodeLookup"))
            {
                __debugbreak();
            }
            if (!gScrParserGlob[inst].opcodeLookupMaxLen
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    190,
                    0,
                    "%s",
                    "gScrParserGlob[inst].opcodeLookupMaxLen"))
            {
                __debugbreak();
            }
            if (!gScrParserGlob[inst].sourcePosLookup
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    191,
                    0,
                    "%s",
                    "gScrParserGlob[inst].sourcePosLookup"))
            {
                __debugbreak();
            }
            if (!gScrCompilePub[inst].opcodePos
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    192,
                    0,
                    "%s",
                    "gScrCompilePub[inst].opcodePos"))
            {
                __debugbreak();
            }
            if (gScrParserGlob[inst].opcodeLookupLen >= gScrParserGlob[inst].opcodeLookupMaxLen)
            {
                gScrParserGlob[inst].opcodeLookupMaxLen += 0x10000;
                if (gScrParserGlob[inst].opcodeLookupLen >= gScrParserGlob[inst].opcodeLookupMaxLen
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        197,
                        0,
                        "%s",
                        "gScrParserGlob[inst].opcodeLookupLen < gScrParserGlob[inst].opcodeLookupMaxLen"))
                {
                    __debugbreak();
                }
                newOpcodeLookup = (unsigned __int8 *)Hunk_UserAlloc(
                    g_DebugHunkUser,
                    12 * gScrParserGlob[inst].opcodeLookupMaxLen,
                    4,
                    "AddOpcodePos");
                memset(newOpcodeLookup, 0, 12 * gScrParserGlob[inst].opcodeLookupMaxLen);
                memcpy(
                    newOpcodeLookup,
                    (unsigned __int8 *)gScrParserGlob[inst].opcodeLookup,
                    12 * gScrParserGlob[inst].opcodeLookupLen);
                Hunk_UserFree(g_DebugHunkUser, gScrParserGlob[inst].opcodeLookup);
                gScrParserGlob[inst].opcodeLookup = (OpcodeLookup *)newOpcodeLookup;
            }
            if (gScrParserGlob[inst].sourcePosLookupLen >= gScrParserGlob[inst].sourcePosLookupMaxLen)
            {
                gScrParserGlob[inst].sourcePosLookupMaxLen += 0x10000;
                if (gScrParserGlob[inst].sourcePosLookupLen >= gScrParserGlob[inst].sourcePosLookupMaxLen
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        208,
                        0,
                        "%s",
                        "gScrParserGlob[inst].sourcePosLookupLen < gScrParserGlob[inst].sourcePosLookupMaxLen"))
                {
                    __debugbreak();
                }
                newSourcePosLookup = (unsigned __int8 *)Hunk_UserAlloc(
                    g_DebugHunkUser,
                    8 * gScrParserGlob[inst].sourcePosLookupMaxLen,
                    4,
                    "AddOpcodePos");
                memset(newSourcePosLookup, 0, 8 * gScrParserGlob[inst].sourcePosLookupMaxLen);
                memcpy(
                    newSourcePosLookup,
                    (unsigned __int8 *)gScrParserGlob[inst].sourcePosLookup,
                    8 * gScrParserGlob[inst].sourcePosLookupLen);
                Hunk_UserFree(g_DebugHunkUser, gScrParserGlob[inst].sourcePosLookup);
                gScrParserGlob[inst].sourcePosLookup = (SourceLookup *)newSourcePosLookup;
            }
            if (gScrParserGlob[inst].currentCodePos == gScrCompilePub[inst].opcodePos)
            {
                if (!gScrParserGlob[inst].currentSourcePosCount
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        218,
                        0,
                        "%s",
                        "gScrParserGlob[inst].currentSourcePosCount"))
                {
                    __debugbreak();
                }
                opcodeLookup = &gScrParserGlob[inst].opcodeLookup[--gScrParserGlob[inst].opcodeLookupLen];
                if (gScrParserGlob[inst].currentSourcePosCount + opcodeLookup->sourcePosIndex != gScrParserGlob[inst].sourcePosLookupLen
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        224,
                        0,
                        "%s",
                        "opcodeLookup->sourcePosIndex + gScrParserGlob[inst].currentSourcePosCount == gScrParserGlob[inst].sourcePosLookupLen"))
                {
                    __debugbreak();
                }
                if (opcodeLookup->codePos != (const char *)gScrParserGlob[inst].currentCodePos
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        225,
                        0,
                        "%s",
                        "opcodeLookup->codePos == (char *)gScrParserGlob[inst].currentCodePos"))
                {
                    __debugbreak();
                }
            }
            else
            {
                gScrParserGlob[inst].currentSourcePosCount = 0;
                gScrParserGlob[inst].currentCodePos = gScrCompilePub[inst].opcodePos;
                opcodeLookup = &gScrParserGlob[inst].opcodeLookup[gScrParserGlob[inst].opcodeLookupLen];
                opcodeLookup->sourcePosIndex = gScrParserGlob[inst].sourcePosLookupLen;
                opcodeLookup->codePos = (const char *)gScrParserGlob[inst].currentCodePos;
            }
            sourcePosLookupIndex = gScrParserGlob[inst].currentSourcePosCount + opcodeLookup->sourcePosIndex;
            sourcePosLookup = &gScrParserGlob[inst].sourcePosLookup[sourcePosLookupIndex];
            sourcePosLookup->sourcePos = sourcePos;
            if (sourcePos == -1)
            {
                if (gScrParserGlob[inst].delayedSourceIndex != -1
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        244,
                        0,
                        "%s",
                        "gScrParserGlob[inst].delayedSourceIndex == -1"))
                {
                    __debugbreak();
                }
                if ((type & 1) == 0
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        245,
                        0,
                        "%s",
                        "type & SOURCE_TYPE_BREAKPOINT"))
                {
                    __debugbreak();
                }
                gScrParserGlob[inst].delayedSourceIndex = sourcePosLookupIndex;
            }
            else if (sourcePos == -2)
            {
                gScrParserGlob[inst].threadStartSourceIndex = sourcePosLookupIndex;
            }
            else if (gScrParserGlob[inst].delayedSourceIndex >= 0 && (type & 1) != 0)
            {
                gScrParserGlob[inst].sourcePosLookup[gScrParserGlob[inst].delayedSourceIndex].sourcePos = sourcePos;
                gScrParserGlob[inst].delayedSourceIndex = -1;
            }
            sourcePosLookup->type |= type;
            opcodeLookup->sourcePosCount = ++gScrParserGlob[inst].currentSourcePosCount;
            ++gScrParserGlob[inst].opcodeLookupLen;
            ++gScrParserGlob[inst].sourcePosLookupLen;
        }
    }
}

void __cdecl RemoveOpcodePos(scriptInstance_t inst)
{
    OpcodeLookup *opcodeLookup; // [esp+0h] [ebp-4h]

    if (gScrVarPub[inst].developer)
    {
        if (gScrCompilePub[inst].developer_statement == 2)
        {
            if (gScrVarPub[inst].developer_script
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    281,
                    0,
                    "%s",
                    "!gScrVarPub[inst].developer_script"))
            {
                __debugbreak();
            }
        }
        else
        {
            if (!gScrParserGlob[inst].opcodeLookup
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    285,
                    0,
                    "%s",
                    "gScrParserGlob[inst].opcodeLookup"))
            {
                __debugbreak();
            }
            if (!gScrParserGlob[inst].opcodeLookupMaxLen
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    286,
                    0,
                    "%s",
                    "gScrParserGlob[inst].opcodeLookupMaxLen"))
            {
                __debugbreak();
            }
            if (!gScrParserGlob[inst].sourcePosLookup
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    287,
                    0,
                    "%s",
                    "gScrParserGlob[inst].sourcePosLookup"))
            {
                __debugbreak();
            }
            if (!gScrCompilePub[inst].opcodePos
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    288,
                    0,
                    "%s",
                    "gScrCompilePub[inst].opcodePos"))
            {
                __debugbreak();
            }
            if (!gScrParserGlob[inst].sourcePosLookupLen
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    290,
                    0,
                    "%s",
                    "gScrParserGlob[inst].sourcePosLookupLen"))
            {
                __debugbreak();
            }
            --gScrParserGlob[inst].sourcePosLookupLen;
            if (!gScrParserGlob[inst].opcodeLookupLen
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    293,
                    0,
                    "%s",
                    "gScrParserGlob[inst].opcodeLookupLen"))
            {
                __debugbreak();
            }
            --gScrParserGlob[inst].opcodeLookupLen;
            if (!gScrParserGlob[inst].currentSourcePosCount
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    296,
                    0,
                    "%s",
                    "gScrParserGlob[inst].currentSourcePosCount"))
            {
                __debugbreak();
            }
            --gScrParserGlob[inst].currentSourcePosCount;
            opcodeLookup = &gScrParserGlob[inst].opcodeLookup[gScrParserGlob[inst].opcodeLookupLen];
            if (gScrParserGlob[inst].currentCodePos != gScrCompilePub[inst].opcodePos
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    301,
                    0,
                    "%s",
                    "gScrParserGlob[inst].currentCodePos == gScrCompilePub[inst].opcodePos"))
            {
                __debugbreak();
            }
            if (gScrParserGlob[inst].currentSourcePosCount + opcodeLookup->sourcePosIndex != gScrParserGlob[inst].sourcePosLookupLen
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    302,
                    0,
                    "%s",
                    "opcodeLookup->sourcePosIndex + gScrParserGlob[inst].currentSourcePosCount == gScrParserGlob[inst].sourcePosLookupLen"))
            {
                __debugbreak();
            }
            if (opcodeLookup->codePos != (const char *)gScrParserGlob[inst].currentCodePos
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    303,
                    0,
                    "%s",
                    "opcodeLookup->codePos == (char *)gScrParserGlob[inst].currentCodePos"))
            {
                __debugbreak();
            }
            if (!gScrParserGlob[inst].currentSourcePosCount)
                gScrParserGlob[inst].currentCodePos = 0;
            opcodeLookup->sourcePosCount = gScrParserGlob[inst].currentSourcePosCount;
        }
    }
}

void __cdecl AddThreadStartOpcodePos(scriptInstance_t inst, unsigned int sourcePos)
{
    SourceLookup *sourcePosLookup; // [esp+0h] [ebp-4h]

    if (gScrVarPub[inst].developer)
    {
        if (gScrCompilePub[inst].developer_statement == 2)
        {
            if (gScrVarPub[inst].developer_script
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    323,
                    0,
                    "%s",
                    "!gScrVarPub[inst].developer_script"))
            {
                __debugbreak();
            }
        }
        else
        {
            if (gScrParserGlob[inst].threadStartSourceIndex < 0
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    327,
                    0,
                    "%s",
                    "gScrParserGlob[inst].threadStartSourceIndex >= 0"))
            {
                __debugbreak();
            }
            sourcePosLookup = &gScrParserGlob[inst].sourcePosLookup[gScrParserGlob[inst].threadStartSourceIndex];
            sourcePosLookup->sourcePos = sourcePos;
            if (sourcePosLookup->type
                && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    330,
                    0,
                    "%s",
                    "!sourcePosLookup->type"))
            {
                __debugbreak();
            }
            sourcePosLookup->type = 4;
            gScrParserGlob[inst].threadStartSourceIndex = -1;
        }
    }
}


const char *__cdecl Scr_GetOpcodePosOfType(
    scriptInstance_t inst,
    unsigned int bufferIndex,
    unsigned int startSourcePos,
    unsigned int endSourcePos,
    int type,
    unsigned int *sourcePos)
{
    const char *codePos; // [esp+0h] [ebp-34h]
    SourceBufferInfo *v8; // [esp+4h] [ebp-30h]
    unsigned int j; // [esp+8h] [ebp-2Ch]
    signed int sourcePosCount; // [esp+10h] [ebp-24h]
    unsigned int firstSourcePos; // [esp+14h] [ebp-20h]
    int k; // [esp+18h] [ebp-1Ch]
    SourceLookup *sourcePosLookup; // [esp+1Ch] [ebp-18h]
    SourceBufferInfo *sourceBufData; // [esp+20h] [ebp-14h]
    const char *startBufferCodePos; // [esp+24h] [ebp-10h]
    const char *firstOpcodePos; // [esp+28h] [ebp-Ch]
    const char *opcodePos; // [esp+2Ch] [ebp-8h]

    sourceBufData = &gScrParserPub[inst].sourceBufferLookup[bufferIndex];
    if (bufferIndex + 1 >= gScrParserPub[inst].sourceBufferLookupLen)
        v8 = 0;
    else
        v8 = &gScrParserPub[inst].sourceBufferLookup[bufferIndex + 1];
    startBufferCodePos = sourceBufData->codePos;
    if (sourceBufData->codePos)
    {
        firstOpcodePos = 0;
        firstSourcePos = 0;
        if (v8)
            codePos = v8->codePos;
        else
            codePos = 0;
        for (j = 0; j < gScrParserGlob[inst].opcodeLookupLen; ++j)
        {
            sourcePosCount = gScrParserGlob[inst].opcodeLookup[j].sourcePosCount;
            for (k = 0; k < sourcePosCount; ++k)
            {
                opcodePos = gScrParserGlob[inst].opcodeLookup[j].codePos;
                if (opcodePos >= startBufferCodePos)
                {
                    if (opcodePos <= codePos - 1)
                    {
                        sourcePosLookup = &gScrParserGlob[inst].sourcePosLookup[k
                            + gScrParserGlob[inst].opcodeLookup[j].sourcePosIndex];
                        if ((type & sourcePosLookup->type) == type
                            && sourcePosLookup->sourcePos >= startSourcePos
                            && sourcePosLookup->sourcePos < endSourcePos
                            && (!firstOpcodePos || gScrParserGlob[inst].opcodeLookup[j].codePos < firstOpcodePos))
                        {
                            firstOpcodePos = gScrParserGlob[inst].opcodeLookup[j].codePos;
                            firstSourcePos = sourcePosLookup->sourcePos;
                        }
                    }
                    else if (!v8
                        && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                            379,
                            0,
                            "%s",
                            "nextSourceBufData"))
                    {
                        __debugbreak();
                    }
                }
            }
        }
        *sourcePos = firstSourcePos;
        return firstOpcodePos;
    }
    else
    {
        *sourcePos = 0;
        return 0;
    }
}

unsigned int __cdecl Scr_GetClosestSourcePosOfType(
    scriptInstance_t inst,
    unsigned int bufferIndex,
    unsigned int sourcePos,
    int type)
{
    const char *codePos; // [esp+0h] [ebp-30h]
    SourceBufferInfo *v6; // [esp+4h] [ebp-2Ch]
    unsigned int j; // [esp+8h] [ebp-28h]
    signed int sourcePosCount; // [esp+10h] [ebp-20h]
    int k; // [esp+14h] [ebp-1Ch]
    SourceLookup *sourcePosLookup; // [esp+18h] [ebp-18h]
    SourceBufferInfo *sourceBufData; // [esp+1Ch] [ebp-14h]
    const char *startBufferCodePos; // [esp+20h] [ebp-10h]
    unsigned int bestSourcePos; // [esp+24h] [ebp-Ch]
    const char *opcodePos; // [esp+28h] [ebp-8h]

    bestSourcePos = 0;
    sourceBufData = &gScrParserPub[inst].sourceBufferLookup[bufferIndex];
    if (bufferIndex + 1 >= gScrParserPub[inst].sourceBufferLookupLen)
        v6 = 0;
    else
        v6 = &gScrParserPub[inst].sourceBufferLookup[bufferIndex + 1];
    startBufferCodePos = sourceBufData->codePos;
    if (!sourceBufData->codePos)
        return 0;
    if (v6)
        codePos = v6->codePos;
    else
        codePos = 0;
    for (j = 0; j < gScrParserGlob[inst].opcodeLookupLen; ++j)
    {
        sourcePosCount = gScrParserGlob[inst].opcodeLookup[j].sourcePosCount;
        for (k = 0; k < sourcePosCount; ++k)
        {
            opcodePos = gScrParserGlob[inst].opcodeLookup[j].codePos;
            if (opcodePos >= startBufferCodePos)
            {
                if (opcodePos <= codePos - 1)
                {
                    sourcePosLookup = &gScrParserGlob[inst].sourcePosLookup[k
                        + gScrParserGlob[inst].opcodeLookup[j].sourcePosIndex];
                    if ((type & sourcePosLookup->type) == type
                        && sourcePosLookup->sourcePos >= bestSourcePos
                        && sourcePosLookup->sourcePos <= sourcePos)
                    {
                        bestSourcePos = sourcePosLookup->sourcePos;
                    }
                }
                else if (!v6
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                        439,
                        0,
                        "%s",
                        "nextSourceBufData"))
                {
                    __debugbreak();
                }
            }
        }
    }
    return bestSourcePos;
}

unsigned int __cdecl Scr_GetPrevSourcePos(scriptInstance_t inst, const char *codePos, unsigned int index)
{
    return gScrParserGlob[inst].sourcePosLookup[index + Scr_GetPrevSourcePosOpcodeLookup(inst, codePos)->sourcePosIndex].sourcePos;
}

OpcodeLookup *__cdecl Scr_GetPrevSourcePosOpcodeLookup(scriptInstance_t inst, const char *codePos)
{
    unsigned int low; // [esp+0h] [ebp-Ch]
    unsigned int middle; // [esp+4h] [ebp-8h]
    unsigned int high; // [esp+8h] [ebp-4h]

    if (!Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            462,
            0,
            "%s",
            "Scr_IsInOpcodeMemory( inst, codePos )"))
    {
        __debugbreak();
    }
    if (!gScrParserGlob[inst].opcodeLookup
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            463,
            0,
            "%s",
            "gScrParserGlob[inst].opcodeLookup"))
    {
        __debugbreak();
    }
    low = 0;
    high = gScrParserGlob[inst].opcodeLookupLen - 1;
    while (low <= high)
    {
        middle = (high + low) >> 1;
        if (codePos < gScrParserGlob[inst].opcodeLookup[middle].codePos)
        {
            high = middle - 1;
        }
        else
        {
            low = middle + 1;
            if (middle + 1 == gScrParserGlob[inst].opcodeLookupLen
                || codePos < gScrParserGlob[inst].opcodeLookup[low].codePos)
            {
                return &gScrParserGlob[inst].opcodeLookup[middle];
            }
        }
    }
    if (!Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 482, 0, "unreachable"))
        __debugbreak();
    return 0;
}

unsigned int __cdecl Scr_GetLineNum(scriptInstance_t inst, unsigned int bufferIndex, unsigned int sourcePos)
{
    const char *startLine; // [esp+0h] [ebp-8h] BYREF
    int col; // [esp+4h] [ebp-4h] BYREF

    if (!gScrVarPub[inst].developer
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            645,
            0,
            "%s",
            "gScrVarPub[inst].developer"))
    {
        __debugbreak();
    }
    return Scr_GetLineNumInternal(
        gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf,
        sourcePos,
        &startLine,
        &col);
}

unsigned int __cdecl Scr_GetLineNumInternal(const char *buf, unsigned int sourcePos, const char **startLine, int *col)
{
    unsigned int lineNum; // [esp+0h] [ebp-4h]

    if ( !buf
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 618, 0, "%s", "buf") )
    {
        __debugbreak();
    }
    *startLine = buf;
    lineNum = 0;
    while ( sourcePos )
    {
        if ( !*buf )
        {
            *startLine = buf + 1;
            ++lineNum;
        }
        ++buf;
        --sourcePos;
    }
    *col = buf - *startLine;
    return lineNum;
}

unsigned int __cdecl Scr_GetFunctionLineNumInternal(const char *buf, unsigned int sourcePos, const char **startLine)
{
    unsigned int lineNum; // [esp+0h] [ebp-Ch]
    unsigned int functionLine; // [esp+4h] [ebp-8h]
    const char *functionStartLine; // [esp+8h] [ebp-4h]

    if ( !buf
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 564, 0, "%s", "buf") )
    {
        __debugbreak();
    }
    *startLine = buf;
    lineNum = 0;
    functionLine = 0;
    functionStartLine = buf;
    while ( sourcePos )
    {
        if ( !*buf )
        {
            if ( buf[1] == 123 )
            {
                functionLine = lineNum;
                functionStartLine = *startLine;
            }
            *startLine = buf + 1;
            ++lineNum;
        }
        ++buf;
        --sourcePos;
    }
    *startLine = functionStartLine;
    return functionLine;
}

int __cdecl Scr_GetSourcePosOfType(scriptInstance_t inst, const char *codePos, int type, Scr_SourcePos_t *pos)
{
    OpcodeLookup *SourcePosOpcodeLookup; // [esp+0h] [ebp-Ch]
    unsigned int index; // [esp+8h] [ebp-4h]

    if (type)
        SourcePosOpcodeLookup = Scr_GetSourcePosOpcodeLookup(inst, codePos);
    else
        SourcePosOpcodeLookup = Scr_GetPrevSourcePosOpcodeLookup(inst, codePos);
    if (SourcePosOpcodeLookup)
    {
        for (index = 0; index < SourcePosOpcodeLookup->sourcePosCount; ++index)
        {
            if ((type & gScrParserGlob[inst].sourcePosLookup[index + SourcePosOpcodeLookup->sourcePosIndex].type) == type)
            {
                pos->sourcePos = gScrParserGlob[inst].sourcePosLookup[index + SourcePosOpcodeLookup->sourcePosIndex].sourcePos;
                pos->bufferIndex = Scr_GetSourceBuffer(inst, codePos);
                pos->lineNum = Scr_GetLineNum(inst, pos->bufferIndex, pos->sourcePos);
                return 1;
            }
        }
    }
    pos->sourcePos = 0;
    pos->bufferIndex = 0;
    pos->lineNum = 0;
    return 0;
}

OpcodeLookup *__cdecl Scr_GetSourcePosOpcodeLookup(scriptInstance_t inst, const char *codePos)
{
    int low; // [esp+0h] [ebp-Ch]
    int middle; // [esp+4h] [ebp-8h]
    int high; // [esp+8h] [ebp-4h]

    if (!Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            523,
            0,
            "%s",
            "Scr_IsInOpcodeMemory( inst, codePos )"))
    {
        __debugbreak();
    }
    if (!gScrParserGlob[inst].opcodeLookup
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            524,
            0,
            "%s",
            "gScrParserGlob[inst].opcodeLookup"))
    {
        __debugbreak();
    }
    low = 0;
    high = gScrParserGlob[inst].opcodeLookupLen - 1;
    while (low <= high)
    {
        middle = (high + low) / 2;
        if (codePos < gScrParserGlob[inst].opcodeLookup[middle].codePos)
        {
            high = middle - 1;
        }
        else
        {
            if (codePos == gScrParserGlob[inst].opcodeLookup[middle].codePos)
                return &gScrParserGlob[inst].opcodeLookup[middle];
            low = middle + 1;
        }
    }
    return 0;
}

void __cdecl Scr_SendSource(scriptInstance_t inst)
{
    unsigned int i; // [esp+0h] [ebp-8h]
    SourceBufferInfo *sourceBufferLookup; // [esp+4h] [ebp-4h]

    if (!Sys_IsRemoteDebugServer()
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            818,
            0,
            "%s",
            "Sys_IsRemoteDebugServer()"))
    {
        __debugbreak();
    }
    if (gScrVarPub[inst].developer)
    {
        Sys_WriteDebugSocketMessageType(2u);
        Sys_EndWriteDebugSocket();
        for (i = 0; i < gScrParserPub[inst].sourceBufferLookupLen; ++i)
        {
            sourceBufferLookup = &gScrParserPub[inst].sourceBufferLookup[i];
            if (sourceBufferLookup->codePos)
            {
                Sys_WriteDebugSocketMessageType(1u);
                Sys_WriteDebugSocketString(sourceBufferLookup->buf);
                Sys_WriteDebugSocketInt(sourceBufferLookup->len);
                if (sourceBufferLookup->len > 0)
                    Sys_WriteDebugSocketData((unsigned __int8 *)sourceBufferLookup->sourceBuf, sourceBufferLookup->len);
                Sys_EndWriteDebugSocket();
            }
        }
        Sys_WriteDebugSocketMessageType(3u);
        Sys_EndWriteDebugSocket();
    }
}

char *__cdecl Scr_AddSourceBuffer(
    scriptInstance_t inst,
    const char *filename,
    char *extFilename,
    const char *codePos,
    bool archive)
{
    char v6; // [esp+3h] [ebp-1Dh]
    const char *source; // [esp+4h] [ebp-1Ch]
    char c; // [esp+Bh] [ebp-15h]
    SaveSourceBufferInfo *saveSourceBuffer; // [esp+Ch] [ebp-14h]
    int len; // [esp+10h] [ebp-10h]
    char *dest; // [esp+14h] [ebp-Ch]
    int i; // [esp+18h] [ebp-8h]
    char *sourceBuf; // [esp+1Ch] [ebp-4h]

    if (!archive || !gScrParserGlob[inst].saveSourceBufferLookup)
        return Scr_ReadFile(inst, filename, extFilename, codePos, archive);
    if (!gScrParserGlob[inst].saveSourceBufferLookupLen
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1010,
            0,
            "%s",
            "gScrParserGlob[inst].saveSourceBufferLookupLen > 0"))
    {
        __debugbreak();
    }
    saveSourceBuffer = &gScrParserGlob[inst].saveSourceBufferLookup[--gScrParserGlob[inst].saveSourceBufferLookupLen];
    len = saveSourceBuffer->len;
    if (len < -1
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 1014, 0, "%s", "len >= -1"))
    {
        __debugbreak();
    }
    if (len >= 0)
    {
        sourceBuf = (char *)Hunk_AllocateTempMemoryHigh(len + 1, "Scr_AddSourceBuffer1");
        source = saveSourceBuffer->sourceBuf;
        dest = sourceBuf;
        for (i = 0; i < len; ++i)
        {
            c = *source++;
            if (c)
                v6 = c;
            else
                v6 = 10;
            *dest++ = v6;
        }
        *dest = 0;
        if (saveSourceBuffer->sourceBuf)
            Hunk_UserFree(
                g_DebugHunkUser,
                gScrParserGlob[inst].saveSourceBufferLookup[gScrParserGlob[inst].saveSourceBufferLookupLen].sourceBuf);
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, sourceBuf, len, 1, archive);
    }
    else
    {
        sourceBuf = 0;
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, 0, len, 1, archive);
    }
    return sourceBuf;
}

void __cdecl Scr_AddSourceBufferInternal(
                scriptInstance_t inst,
                const char *extFilename,
                const char *codePos,
                char *sourceBuf,
                int len,
                bool doEolFixup,
                bool archive)
{
    SourceBufferInfo *NewSourceBuffer; // eax
    const char *v8; // [esp+0h] [ebp-48h]
    char v9; // [esp+7h] [ebp-41h]
    char *v10; // [esp+Ch] [ebp-3Ch]
    const char *v11; // [esp+10h] [ebp-38h]
    unsigned int v12; // [esp+14h] [ebp-34h]
    const char *source; // [esp+2Ch] [ebp-1Ch]
    char c; // [esp+33h] [ebp-15h]
    char ca; // [esp+33h] [ebp-15h]
    char *buf; // [esp+34h] [ebp-14h]
    char *dest; // [esp+40h] [ebp-8h]
    int i; // [esp+44h] [ebp-4h]
    int ia; // [esp+44h] [ebp-4h]

    if ( gScrParserPub[inst].sourceBufferLookup )
    {
        v12 = strlen(extFilename);
        buf = (char *)Hunk_UserAlloc(g_DebugHunkUser, v12 + 1 + len + 2, 4, "Scr_AddSourceBuffer3");
        v11 = extFilename;
        v10 = buf;
        do
        {
            v9 = *v11;
            *v10++ = *v11++;
        }
        while ( v9 );
        if ( sourceBuf )
            v8 = &buf[v12 + 1];
        else
            v8 = 0;
        source = sourceBuf;
        dest = (char *)v8;
        if ( doEolFixup )
        {
            for ( i = 0; i <= len; ++i )
            {
                c = *source++;
                if ( c == 10 || c == 13 && *source != 10 )
                    *dest = 0;
                else
                    *dest = c;
                ++dest;
            }
        }
        else
        {
            for ( ia = 0; ia <= len; ++ia )
            {
                ca = *source++;
                *dest++ = ca;
            }
        }
        NewSourceBuffer = Scr_GetNewSourceBuffer(inst);
        NewSourceBuffer->codePos = codePos;
        NewSourceBuffer->buf = buf;
        NewSourceBuffer->sourceBuf = v8;
        NewSourceBuffer->len = len;
        NewSourceBuffer->sortedIndex = -1;
        NewSourceBuffer->archive = archive;
        //BLOPS_NULLSUB();
        if ( v8 )
            gScrParserPub[inst].sourceBuf = v8;
    }
    else
    {
        gScrParserPub[inst].sourceBuf = 0;
    }
}

SourceBufferInfo *__cdecl Scr_GetNewSourceBuffer(scriptInstance_t inst)
{
    unsigned __int8 *newSourceBufferInfo; // [esp+4h] [ebp-4h]

    if (!gScrParserPub[inst].sourceBufferLookup
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            700,
            0,
            "%s",
            "gScrParserPub[inst].sourceBufferLookup"))
    {
        __debugbreak();
    }
    if (!gScrParserGlob[inst].sourceBufferLookupMaxLen
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            701,
            0,
            "%s",
            "gScrParserGlob[inst].sourceBufferLookupMaxLen"))
    {
        __debugbreak();
    }
    if (gScrParserPub[inst].sourceBufferLookupLen >= gScrParserGlob[inst].sourceBufferLookupMaxLen)
    {
        gScrParserGlob[inst].sourceBufferLookupMaxLen += 0x4000;
        if (gScrParserPub[inst].sourceBufferLookupLen >= gScrParserGlob[inst].sourceBufferLookupMaxLen
            && !Assert_MyHandler(
                "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                706,
                0,
                "%s",
                "gScrParserPub[inst].sourceBufferLookupLen < gScrParserGlob[inst].sourceBufferLookupMaxLen"))
        {
            __debugbreak();
        }
        newSourceBufferInfo = (unsigned __int8 *)Hunk_UserAlloc(
            g_DebugHunkUser,
            24 * gScrParserGlob[inst].sourceBufferLookupMaxLen,
            4,
            "Scr_AddSourceBuffer4");
        memset(newSourceBufferInfo, 0, 24 * gScrParserGlob[inst].sourceBufferLookupMaxLen);
        Com_Memcpy(
            newSourceBufferInfo,
            (unsigned char*)gScrParserPub[inst].sourceBufferLookup,
            24 * gScrParserPub[inst].sourceBufferLookupLen);
        Hunk_UserFree(g_DebugHunkUser, gScrParserPub[inst].sourceBufferLookup);
        gScrParserPub[inst].sourceBufferLookup = (SourceBufferInfo *)newSourceBufferInfo;
    }
    return &gScrParserPub[inst].sourceBufferLookup[gScrParserPub[inst].sourceBufferLookupLen++];
}

// mod (L43): bo1_mod_scriptperf 1 rewrites, in the server's source text before it is compiled, two Five loops that
// cost O(n^2) script time with n zombies. The natives (g_scr_sp_ai.cpp) return what the loops
// computed. Matching ignores whitespace and case; the rewrite keeps the buffer length and every newline (moved to the end
// of the span), so line numbers and positions after it are unchanged. No match (another script version): left alone.
struct ScrModSourcePatch
{
    const char *file;
    const char *name;
    const char *original;
    const char *replacement;
};
static const ScrModSourcePatch s_scrModSourcePatches[] =
{
    { "maps/_zombiemode_utility.gsc", "get_enemy_count",
      "get_enemy_count(){enemies=[];valid_enemies=[];enemies=GetAiSpeciesArray(\"axis\",\"all\");"
      "for(i=0;i<enemies.size;i++){if(is_true(enemies[i].ignore_enemy_count)){continue;}"
      "if(isDefined(enemies[i].animname)){valid_enemies=array_add(valid_enemies,enemies[i]);}}return valid_enemies.size;}",
      "get_enemy_count(){return bo1_mod_getenemycount();}" },
    { "maps/_zombiemode_spawner.gsc", "find_flesh crowd loop",
      "near_zombies=getaiarray(\"axis\");same_enemy_count=0;for(i=0;i<near_zombies.size;i++){if(isdefined(near_zombies[i])"
      "&&isalive(near_zombies[i])){if(isdefined(near_zombies[i].favoriteenemy)&&isdefined(self.favoriteenemy)"
      "&&near_zombies[i].favoriteenemy==self.favoriteenemy){if(distancesquared(near_zombies[i].origin,"
      "self.favoriteenemy.origin)<225*225&&distancesquared(near_zombies[i].origin,self.origin)>525*525)"
      "{same_enemy_count++;}}}}",
      "same_enemy_count=self bo1_mod_sameenemycount();" },
    // mod (L55): the same value unless bo1_mod_ailod 1 (g_scr_sp_ai.cpp G_m_mod_runanimfreq)
    { "animscripts/zombie_run.gsc", "getRunAnimUpdateFrequency",
      "getRunAnimUpdateFrequency(){return GetDvarFloat(#\"ai_runAnimUpdateFrequency\");}",
      "getRunAnimUpdateFrequency(){return self bo1_mod_runanimfreq();}" },
};

// zombies: retail script bugs, which the server's source text has fixed as it is compiled, in the same way, whatever the
// mods. map_is_early_in_the_game (the red damage overlay's "take cover" hint: _gameskill.gsc shouldShowCoverWarning)
// gives a map that set_early_level doesn't list false in a developer block alone, which retail's scripts skip, so in a
// zombie map it was undefined and the overlay's thread logged an error at each hit. The others end a game in retail:
// their comments say how.
static const ScrModSourcePatch s_scrSourceFixes[] =
{
    { "maps/_load_common.gsc", "map_is_early_in_the_game",
      "map_is_early_in_the_game(){/#if(IsDefined(level.testmap)){return true;}#/"
      "/#if(!IsDefined(level.early_level[level.script])){level.early_level[level.script]=false;}#/"
      "return level.early_level[level.script];}",
      "map_is_early_in_the_game(){/#if(IsDefined(level.testmap)){return true;}#/"
      "if(!IsDefined(level.early_level[level.script])){level.early_level[level.script]=false;}"
      "return level.early_level[level.script];}" },
    // Ascension: its easter egg ends in a reward alone, where the other maps' add to a stat (set_sidequest_completed),
    // so nothing kept that it was done. Each player's reward adds to a stat too, for the lobby's leaderboards, as it
    // begins: the reward waits for a player who is down, spectating or reviving, and game over may come first, but the
    // easter egg is theirs either way. The function's comments above it make the room.
    { "maps/zombie_cosmodrome_eggs.gsc", "reward_wait's stat for the easter egg",
      "// Don't give them the minigun if they're downed, spectatin or reviving "
      "// due to weapon switching issues reward_wait() {",
      "reward_wait(){self setdstat(\"PlayerStatsList\",\"ZOMBIE_COSMO_EGG\","
      "self getdstat(\"PlayerStatsList\",\"ZOMBIE_COSMO_EGG\")+1);" },
    // A game's intermission puts each player's total in place of the points they had left to spend, for its end
    // scoreboard. The lobby's report has both (sv_lobby_mp.cpp: SV_Lobby_Stats), so it keeps what they had left first.
    // Moon has an intermission of its own. The comment above the line makes the room.
    { "maps/_zombiemode.gsc", "player_intermission's points left to spend",
      "//Show total gained point for end scoreboard and lobby self.score = self.score_total;",
      "self.score_unspent=self.score;self.score=self.score_total;" },
    { "maps/zombie_moon_utility.gsc", "moon_intermission's points left to spend",
      "//Show total gained point for end scoreboard and lobby self.score = self.score_total;",
      "self.score_unspent=self.score;self.score=self.score_total;" },
    // Shangri-La: each shrink of the 31-79 JGb215 spawns the zombie a trigger, to kick it or step on it
    // (watch_for_kicked's self.shrinkTrigger), which only its death while small deletes (kicked_death, shrink_death).
    // A zombie that grew back kept the trigger for good, so a game that shrank zombies often enough ran out of entities
    // ("G_Spawn: no free entities"). The trigger goes as the zombie grows back.
    { "maps/_zombiemode_weap_shrink_ray.gsc", "shrink_zombie's kick trigger",
      "self.meleeDamage = saved_meleeDamage; //Enable Attacks //self.in_special_attack = undefined; }",
      "self.meleeDamage=saved_meleeDamage;if(IsDefined(self.shrinkTrigger))self.shrinkTrigger Delete();}" },
    // Five: the thief steals what the player holds. thief_take_loot takes a primary instead of a perk bottle, the bowie
    // knife's flourish or nothing, but took Pack-a-Punch's fists (zombie_knuckle_crack), held while the machine has
    // the gun, which crashed the game (BO1 Reimagined's fix, 59710d9). The syrette a reviver holds is no gun either.
    { "maps/_zombiemode_ai_thief.gsc", "thief_take_loot's weapon in hand",
      "// don't take these items...choose random primary instead\n"
      "if ( is_offhand_weapon( weapon ) || weapon == \"zombie_bowie_flourish\" || weapon == \"none\" "
      "|| isSubStr( weapon, \"zombie_perk_bottle\" ) || is_laststand )",
      "if(is_offhand_weapon(weapon)||weapon==\"zombie_bowie_flourish\"||weapon==\"none\""
      "||isSubStr(weapon,\"zombie_perk_bottle\")||is_laststand"
      "||weapon==\"zombie_knuckle_crack\"||weapon==\"syrette_sp\")" },
    // Winter's Howl: freezegun_death sets a frozen zombie's client flags (the frost on it), then spawns the triggers
    // that shatter or crumple it: a gib, and the zombie is deleted. BO1 Reimagined found that a zombie shattered in the
    // frame it died crashes the game in time, and has its triggers wait two network frames (ca1a397). Here they come
    // two network frames later, once the clients have had the flags, so its gib never goes out with them.
    // The wait at the function's end, the length of the freeze animation, after which a zombie no one shot or touched
    // crumples (a gib, hidden, deleted), is shorter by what the triggers waited. With the whole of it after them, it
    // ended two frames after the animation: the zombie was a corpse by then (G_CorpseFromActor frees the script's
    // entity), freezegun_do_crumple's isDefined( self ) said no, and the frozen body stood where it died until the
    // corpses' queue took it. The script's own timing ends it in the frame the corpse is made or the one before.
    { "maps/_zombiemode_weap_freezegun.gsc", "freezegun_death's shatter triggers",
      "self thread freezegun_set_torso_damage_fx(); "
      "shatter_trigger = spawn( \"trigger_damage\", self.origin, 0, 15, 72 ); "
      "shatter_trigger enablelinkto(); shatter_trigger linkto( self ); spawnflags = 1 + 2 + 4 + 16 + 64; "
      "// SF_TOUCH_AI_AXIS | SF_TOUCH_AI_ALLIES | SF_TOUCH_AI_NEUTRAL | SF_TOUCH_VEHICLE | SF_TOUCH_ONCE\n"
      "crumple_trigger = spawn( \"trigger_radius\", self.origin, spawnflags, 15, 72 ); "
      "crumple_trigger enablelinkto(); crumple_trigger linkto( self ); weap = self.damageweapon; "
      "self thread freezegun_wait_for_shatter( player, weap, shatter_trigger, crumple_trigger ); "
      "self thread freezegun_wait_for_crumple( weap, shatter_trigger, crumple_trigger ); "
      "self endon( \"cleanup_freezegun_triggers\" ); wait( anim_len ); "
      "// force the zombie to crumple if he is untouched for the length of the freeze anim",
      "self thread freezegun_set_torso_damage_fx();bo1_t=GetTime();wait_network_frame();wait_network_frame();"
      "if(!IsDefined(self))return;"
      "shatter_trigger=spawn(\"trigger_damage\",self.origin,0,15,72);shatter_trigger enablelinkto();"
      "shatter_trigger linkto(self);spawnflags=1+2+4+16+64;"
      "crumple_trigger=spawn(\"trigger_radius\",self.origin,spawnflags,15,72);crumple_trigger enablelinkto();"
      "crumple_trigger linkto(self);weap=self.damageweapon;"
      "self thread freezegun_wait_for_shatter(player,weap,shatter_trigger,crumple_trigger);"
      "self thread freezegun_wait_for_crumple(weap,shatter_trigger,crumple_trigger);"
      "self endon(\"cleanup_freezegun_triggers\");anim_len-=(GetTime()-bo1_t)*0.001;if(anim_len>0)wait(anim_len);" },
};

// mod: Unique Wall Weapons, a room's mod (zombies-hub's server/mods.go, bo1_mod_uniquewallweapons 1): each wall weapon
// has one holder at a time. No script of the mod's own: the wall buy's loop has no hook, and decide_hide_show_hint sets
// each player's prompt again every 0.1 s. bo1_wall_taken takes the place of the box's setup comment, and says whether a
// gun off the wall is another's: a player's who is in the game (one bled out keeps their weapons as a spectator), or in
// Pack-a-Punch. Only guns: every player starts with grenades. The box needs nothing: no map's box has a wall gun. A
// player with the gun still sees the prompt, for its ammo. decide_hide_show_hint is every wall buy's, Nacht's cabinet's
// too, and the purchase's check is for the 0.1 s between.
static const ScrModSourcePatch s_scrUniqueWallWeapons[] =
{
    { "maps/_zombiemode_weapons.gsc", "bo1_wall_taken",
      "// for the random weapon chest // // The chests need to be setup as follows: // trigger_use - for the chest "
      "// targets the lid // lid - script_model. Flips open to reveal the items "
      "// targets the script origin inside the box // script_origin - inside the box, used for spawning the weapons "
      "// targets the box // box - script_model of the outer casing of the chest "
      "// rubble - pieces that show when the box isn't there "
      "// script_noteworthy should be the same as the use_trigger + \"_rubble\" //",
      "bo1_wall_taken(w,p){if(!IsDefined(w)||WeaponType(w)!=\"bullet\")return false;"
      "if(p has_weapon_or_upgrade(w))return false;a=get_players();for(i=0;i<a.size;i++)"
      "if(a[i].sessionstate==\"playing\"&&a[i] has_weapon_or_upgrade(w))return true;"
      "a=GetEntArray(\"zombie_vending_upgrade\",\"targetname\");for(i=0;i<a.size;i++)"
      "if(IsDefined(a[i].current_weapon)&&a[i].current_weapon==w)return true;return false;}" },
    { "maps/_zombiemode_weapons.gsc", "decide_hide_show_hint's prompt",
      "if( players[i] can_buy_weapon()) { self SetInvisibleToPlayer( players[i], false ); } "
      "else { self SetInvisibleToPlayer( players[i], true ); }",
      "self SetInvisibleToPlayer(players[i],!(players[i] can_buy_weapon())"
      "||bo1_wall_taken(self.zombie_weapon_upgrade,players[i]));" },
    { "maps/_zombiemode_weapons.gsc", "weapon_spawn_think's purchase",
      "// Allow people to get ammo off the wall for upgraded weapons "
      "player_has_weapon = player has_weapon_or_upgrade( self.zombie_weapon_upgrade ); "
      "if( !player_has_weapon ) { // else make the weapon show and give it if( player.score >= cost )",
      "if(bo1_wall_taken(self.zombie_weapon_upgrade,player)){wait(0.1);continue;}"
      "player_has_weapon=player has_weapon_or_upgrade(self.zombie_weapon_upgrade);"
      "if(!player_has_weapon){if(player.score>=cost)" },
};

// mod: Grief, a room's mod (zombies-hub's server/mods.go, bo1_mod_grief 1): two teams in one game, and a power-up is
// its taker's team's alone. The teams go by seat (GetEntityNumber: a player's place in the room,
// src/server_mp/sv_lobby_mp.cpp): the first half of the room's players (bo1_players, rounded up: 3 of 5) against the
// rest. The scripts have no teams of their own, every player being an ally, and what a power-up does is the level's.
// So:
// - Its functions take the place of four commented-out ones in maps/_utility.gsc, which every script includes.
//   bo1_team is a player's team, bo1_mates a team's players (everyone, of no team), and level.bo1_g the team of the
//   player taking a power-up, through the frame in which powerup_grab starts what it does (bo1_grab). What pays later
//   keeps it in a local (the nuke's 400 points, the carpenter's 200). What a script starts with no one taking anything
//   is everyone's, as it was (a nuke of Ascension's easter egg).
// - Max Ammo, the perk bottle and the QED's bonus points go to the team's players. The nuke still kills every zombie,
//   and the carpenter mends every window.
// - Double points and insta-kill are the level's in retail (zombie_vars), a timer and an icon each. Here a player has
//   each until a time (bo1_t[0], [1], which bo1_timed sets for a team: 30 s from the last one it took), and what asked
//   the level asks the player (bo1_on): the points' multiplier, a hit's instant kill and its doubled bonus, the kill's
//   line, Five's thief (no bonfire sale after insta-kill) and Shangri-La's napalm zombie (double damage). The level's
//   own timers still run, for their sounds, which everyone hears. Its two icons give way to two of each player's
//   (bo1_hud, bo1_icon: Power_up_hud's place and blinking).
// - Of two players who reach a power-up in the same 0.1 s the nearer takes it, where the lower seat did.
// - A round's drops are counted for each team, where retail stops them at the map's number (4; Der Riese 3) for the
//   whole room, which two teams would share, and one could use up. A drop counts for the team of the player whose kill
//   it came from: zombie_death_points keeps it in level.bo1_k while powerup_drop starts, which takes it into a local
//   before its first wait, and bo1_drops counts it (level.bo1_d, which powerup_round_start empties). Each team's stop
//   at the map's number. A drop with no player's kill behind it (a zombie a nuke killed, the ape's) has no team, and
//   counts on the level's own counter, as it did. What makes a kill drop, and who may take it, are as they were.
// - Five's thief gives what it took back to the team of the player who killed it, where it gave it back to everyone:
//   the other team's is gone, as when the thief gets away.
// - What grows with the number of players grows with a team's, the bigger team's of two (bo1_big): the thief's health
//   and George's (Call of the Dead), the astronaut's (Moon) and the napalm zombie's (Shangri-La). A round's zombies,
//   hellhounds, crawlers and monkeys still go by the game's players.
// - No player's weapon does anything to another player. Callback_PlayerDamage (maps/_callbackglobal.gsc) drops what
//   one ally does to another, but only after player_damage_override, which has by then put the player in last stand
//   where a hit takes all their health and everyone else is down: a downed player's shot at the last one up, or a
//   player's own frag that downs them beside another who is hurt, ended the game. player_damage_override gives
//   another player's hit 0, after its crossbow's and blow gun's notes.
// Left the level's: fire sale and bonfire sale (the box and Pack-a-Punch have one price for all), the announcer and
// "Max Ammo!", which tell everyone what was taken, and what a zombie takes (the QED's). The death machine, Call of the
// Dead's Wunderwaffe and the QED's weapon and points were their taker's alone already. Nothing in the game shows the
// teams: the room does, and its report, which has each team's cash (zombies-hub's server/mods.go: teamOf).
static const ScrModSourcePatch s_scrGrief[] =
{
    { "maps/_utility.gsc", "the teams' functions",
      "// SCRIPTER_MOD: dguzzo: 3/20/2009 : pretty sure these are all deprecated //objective_is_active( msg ) //{ // "
      "active = false; // // objective must be active for this trigger to hit // for( i = 0;i < "
      "level.active_objective.size;i++ ) // { // if( level.active_objective[ i ] != msg ) // continue; // active = "
      "true; // break; // } // return( active ); //} // //objective_is_inactive( msg ) //{ // inactive = false; // // "
      "objective must be active for this trigger to hit // for( i = 0;i < level.inactive_objective.size;i++ ) // { // "
      "if( level.inactive_objective[ i ] != msg ) // continue; // inactive = true; // break; // } // return( inactive "
      "); //} // //set_objective_inactive( msg ) //{ // // remove the objective from the active list // array = []; "
      "// for( i = 0;i < level.active_objective.size;i++ ) // { // if( level.active_objective[ i ] == msg ) // "
      "continue; // array[ array.size ] = level.active_objective[ i ]; // } // level.active_objective = array; // // "
      "// // add it to the inactive list // exists = false; // for( i = 0;i < level.inactive_objective.size;i++ ) // "
      "{ // if( level.inactive_objective[ i ] != msg ) // continue; // exists = true; // } // if( !exists ) // "
      "level.inactive_objective[ level.inactive_objective.size ] = msg; // // /# // // assert that each objective is "
      "only on one list // for( i = 0;i < level.active_objective.size;i++ ) // { // for( p = 0;p < "
      "level.inactive_objective.size;p ++ ) // assertEx( level.active_objective[ i ] != level.inactive_objective[ p "
      "], \"Objective is both inactive and active\" ); // } // #/ //} // //set_objective_active( msg ) //{ // // "
      "remove the objective from the inactive list // array = []; // for( i = 0;i < level.inactive_objective.size;i++ "
      ") // { // if( level.inactive_objective[ i ] == msg ) // continue; // array[ array.size ] = "
      "level.inactive_objective[ i ]; // } // level.inactive_objective = array; // // // add it to the active list // "
      "exists = false; // for( i = 0;i < level.active_objective.size;i++ ) // { // if( level.active_objective[ i ] != "
      "msg ) // continue; // exists = true; // } // if( !exists ) // level.active_objective[ "
      "level.active_objective.size ] = msg; // // /# // // assert that each objective is only on one list // for( i = "
      "0;i < level.active_objective.size;i++ ) // { // for( p = 0;p < level.inactive_objective.size;p ++ ) // "
      "assertEx( level.active_objective[ i ] != level.inactive_objective[ p ], \"Objective is both inactive and "
      "active\" ); // } // #/ //}",
      "bo1_team(p){return p GetEntityNumber()>=int((GetDvarInt(\"bo1_players\")+1)/2);}"
      "bo1_grab(p){level.bo1_g=bo1_team(p);waittillframeend;level.bo1_g=undefined;}"
      "bo1_mates(t){a=[];p=GetPlayers();for(i=0;i<p.size;i++)if(!IsDefined(t)||bo1_team(p[i])==t)a[a.size]=p[i];"
      "return a;}"
      "bo1_timed(k){p=bo1_mates(level.bo1_g);for(i=0;i<p.size;i++)p[i].bo1_t[k]=GetTime()+30000;}"
      "bo1_left(p,k){if(!IsDefined(p)||!IsDefined(p.bo1_t)||!IsDefined(p.bo1_t[k]))return 0;"
      "return p.bo1_t[k]-GetTime();}"
      "bo1_on(p,k){return bo1_left(p,k)>0;}"
      "bo1_hud(){level.powerup_hud[0] destroy();level.powerup_hud[1] destroy();"
      "flag_wait(\"all_players_connected\");p=GetPlayers();for(i=0;i<p.size;i++){"
      "p[i] thread bo1_icon(\"specialty_doublepoints_zombies\",-44,0);"
      "p[i] thread bo1_icon(\"specialty_instakill_zombies\",-4,1);}}"
      "bo1_icon(s,x,k){self endon(\"disconnect\");h=NewClientHudElem(self);h.foreground=true;h.sort=2;"
      "h.alignX=\"center\";h.alignY=\"bottom\";h.horzAlign=\"user_center\";h.vertAlign=\"user_bottom\";h.x=x;h.y=-5;"
      "h.alpha=0;h setshader(s,32,32);for(;;){t=bo1_left(self,k);if(t>0&&t<5000){wait 0.1;h.alpha=0;wait 0.1;}"
      "else if(t>0&&t<10000){wait 0.2;h.alpha=0;wait 0.18;}h.alpha=bo1_on(self,k);wait 0.05;}}"
      "bo1_drops(k,n){if(!IsDefined(k)){level.powerup_drop_count+=n;return level.powerup_drop_count;}"
      "if(!IsDefined(level.bo1_d))level.bo1_d=[];if(!IsDefined(level.bo1_d[k]))level.bo1_d[k]=0;level.bo1_d[k]+=n;"
      "return level.bo1_d[k];}"
      "bo1_big(){a=bo1_mates(0);b=bo1_mates(1);if(b.size>a.size)return b;return a;}" },
    // the players in the order of their distance, so that the first of them in reach is the nearest (a comment's place)
    { "maps/_zombiemode_powerups.gsc", "powerup_grab's nearest player",
      "range_squared = 64 * 64; while (isdefined(self)) { players = get_players(); for (i = 0; i < players.size; i++) "
      "{ // Don't let them grab the minigun, tesla, or random weapon if they're downed or reviving",
      "range_squared=64*64;while(isdefined(self)){players=get_array_of_closest(self.origin,get_players());"
      "for(i=0;i<players.size;i++){" },
    { "maps/_zombiemode_powerups.gsc", "powerup_grab's team",
      "if ( DistanceSquared( players[i].origin, self.origin ) < range_squared ) { "
      "if( IsDefined( level.zombie_powerup_grab_func ) ) { level thread [[level.zombie_powerup_grab_func]](); } else {",
      "if(DistanceSquared(players[i].origin,self.origin)<range_squared){level thread bo1_grab(players[i]);"
      "if(IsDefined(level.zombie_powerup_grab_func)){level thread [[level.zombie_powerup_grab_func]]();}else{" },
    { "maps/_zombiemode_powerups.gsc", "the icons of double points and insta-kill",
      "level thread Power_up_hud( \"specialty_doublepoints_zombies\", level.powerup_hud[0], -44, "
      "\"zombie_powerup_point_doubler_time\", \"zombie_powerup_point_doubler_on\" ); "
      "level thread Power_up_hud( \"specialty_instakill_zombies\", level.powerup_hud[1], -04, "
      "\"zombie_powerup_insta_kill_time\", \"zombie_powerup_insta_kill_on\" );",
      "level thread bo1_hud();" },
    { "maps/_zombiemode_powerups.gsc", "nuke_powerup's team",
      "nuke_powerup( drop_item ) { zombies = getaispeciesarray(\"axis\"); location = drop_item.origin; "
      "PlayFx( drop_item.fx, location );",
      "nuke_powerup(drop_item){t=level.bo1_g;zombies=getaispeciesarray(\"axis\");location=drop_item.origin;"
      "PlayFx(drop_item.fx,location);" },
    { "maps/_zombiemode_powerups.gsc", "nuke_powerup's points",
      "players = get_players(); for(i = 0; i < players.size; i++) { "
      "players[i] maps\\_zombiemode_score::player_add_points( \"nuke_powerup\", 400 );",
      "players=bo1_mates(t);for(i=0;i<players.size;i++){"
      "players[i] maps\\_zombiemode_score::player_add_points(\"nuke_powerup\",400);" },
    // in the place of a comment each, in double_points_powerup and insta_kill_powerup
    { "maps/_zombiemode_powerups.gsc", "double_points_powerup's team",
      "// players = get_players(); // array_thread(level,::point_doubler_on_hud, drop_item);",
      "bo1_timed(0);" },
    { "maps/_zombiemode_powerups.gsc", "full_ammo_powerup's players",
      "full_ammo_powerup( drop_item ) { players = get_players(); for (i = 0; i < players.size; i++)",
      "full_ammo_powerup(drop_item){players=bo1_mates(level.bo1_g);for(i=0;i<players.size;i++)" },
    { "maps/_zombiemode_powerups.gsc", "insta_kill_powerup's team",
      "// array_thread (players, ::insta_kill_on_hud, drop_item);",
      "bo1_timed(1);" },
    { "maps/_zombiemode_powerups.gsc", "check_for_instakill's player",
      "(level.zombie_vars[\"zombie_insta_kill\"] || is_true( player.personal_instakill ))",
      "(bo1_on(player,1)||is_true(player.personal_instakill))" },
    { "maps/_zombiemode_powerups.gsc", "free_perk_powerup's players",
      "free_perk_powerup( item ) { players = getplayers(); for ( i = 0; i < players.size; i++ )",
      "free_perk_powerup(item){players=bo1_mates(level.bo1_g);for(i=0;i<players.size;i++)" },
    { "maps/_zombiemode_powerups.gsc", "bonus_points_team_powerup's players",
      "bonus_points_team_powerup( item ) { points = RandomIntRange( 1, 25 ) * 100; players = getplayers(); "
      "for ( i = 0; i < players.size; i++ )",
      "bonus_points_team_powerup(item){points=RandomIntRange(1,25)*100;players=bo1_mates(level.bo1_g);"
      "for(i=0;i<players.size;i++)" },
    // start_carpenter_new is every map's carpenter (level.use_new_carpenter_func); its //COLLIN makes the room
    { "maps/_zombiemode_powerups.gsc", "start_carpenter_new's team",
      "start_carpenter_new( origin ) { window_boards = getstructarray( \"exterior_goal\", \"targetname\" ); //COLLIN",
      "start_carpenter_new(origin){t=level.bo1_g;window_boards=getstructarray(\"exterior_goal\",\"targetname\");" },
    { "maps/_zombiemode_powerups.gsc", "start_carpenter_new's points",
      "carp_ent waittill( \"sound_done\" ); players = get_players(); for(i = 0; i < players.size; i++) { "
      "players[i] maps\\_zombiemode_score::player_add_points( \"carpenter_powerup\", 200 );",
      "carp_ent waittill(\"sound_done\");players=bo1_mates(t);for(i=0;i<players.size;i++){"
      "players[i] maps\\_zombiemode_score::player_add_points(\"carpenter_powerup\",200);" },
    { "maps/_zombiemode_score.gsc", "player_add_points' insta-kill",
      "if( level.zombie_vars[\"zombie_powerup_insta_kill_on\"] == 1 && mod == \"MOD_UNKNOWN\" )",
      "if(bo1_on(self,1)&&mod==\"MOD_UNKNOWN\")" },
    { "maps/_zombiemode_score.gsc", "get_points_multiplier's double points",
      "multiplier = level.zombie_vars[\"zombie_point_scalar\"];",
      "multiplier=1+bo1_on(self,0);" },
    { "maps/_zombiemode_audio.gsc", "player_zombie_kill_vox's insta-kill",
      "instakill = level.zombie_vars[\"zombie_insta_kill\"];",
      "instakill=bo1_on(player,1);" },
    { "maps/_zombiemode_ai_thief.gsc", "thief_actor_damage's insta-kill",
      "// disable bonfire if player used insta-kill if ( level.zombie_vars[\"zombie_insta_kill\"] )",
      "if(bo1_on(attacker,1))" },
    { "maps/_zombiemode_ai_napalm.gsc", "_napalm_zombie_damage's insta-kill",
      "_napalm_zombie_damage( inflictor, attacker, damage, flags, meansofdeath, weapon, vpoint, vdir, sHitLoc, "
      "modelIndex, psOffsetTime ) { if ( level.zombie_vars[\"zombie_insta_kill\"] )",
      "_napalm_zombie_damage(inflictor,attacker,damage,flags,meansofdeath,weapon,vpoint,vdir,sHitLoc,modelIndex,"
      "psOffsetTime){if(bo1_on(attacker,1))" },
    // a team's own count of the round's drops: level.bo1_k is the killer's team while powerup_drop starts
    { "maps/_zombiemode_spawner.gsc", "zombie_death_points' team",
      "if( zombie_can_drop_powerups( zombie ) ) { // DCS 031611: hack to prevent risers from dropping powerups under "
      "the ground. if(IsDefined(zombie.in_the_ground) && zombie.in_the_ground == true) { trace = "
      "BulletTrace(zombie.origin + (0, 0, 100), zombie.origin + (0, 0, -100), false, undefined); origin = "
      "trace[\"position\"]; level thread maps\\_zombiemode_powerups::powerup_drop( origin ); } else { trace = "
      "GroundTrace(zombie.origin + (0, 0, 5), zombie.origin + (0, 0, -300), false, undefined); origin = "
      "trace[\"position\"]; level thread maps\\_zombiemode_powerups::powerup_drop( origin ); } }",
      "if(zombie_can_drop_powerups(zombie)){level.bo1_k=bo1_team(attacker);if(IsDefined(zombie.in_the_ground)"
      "&&zombie.in_the_ground==true){trace=BulletTrace(zombie.origin+(0,0,100),zombie.origin+(0,0,-100),false,"
      "undefined);origin=trace[\"position\"];level thread maps\\_zombiemode_powerups::powerup_drop(origin);}else{"
      "trace=GroundTrace(zombie.origin+(0,0,5),zombie.origin+(0,0,-300),false,undefined);origin=trace[\"position\"];"
      "level thread maps\\_zombiemode_powerups::powerup_drop(origin);}level.bo1_k=undefined;}" },
    // the end of include_zombie_powerup, above it, makes the room
    { "maps/_zombiemode_powerups.gsc", "powerup_round_start's teams",
      "level.zombie_include_powerups = []; } level.zombie_include_powerups[powerup_name] = true; } "
      "powerup_round_start() { level.powerup_drop_count = 0; }",
      "level.zombie_include_powerups=[];}level.zombie_include_powerups[powerup_name]=true;}powerup_round_start(){"
      "level.powerup_drop_count=0;level.bo1_d=[];}" },
    { "maps/_zombiemode_powerups.gsc", "powerup_drop's team",
      "if( level.powerup_drop_count >= level.zombie_vars[\"zombie_powerup_drop_max_per_round\"] ) { /# println( "
      "\"^3POWERUP DROP EXCEEDED THE MAX PER ROUND!\" ); #/ return; }",
      "k=level.bo1_k;if(bo1_drops(k,0)>=level.zombie_vars[\"zombie_powerup_drop_max_per_round\"]){return;}" },
    { "maps/_zombiemode_powerups.gsc", "powerup_drop's count",
      "level.powerup_drop_count++;",
      "bo1_drops(k,1);" },
    { "maps/_zombiemode_powerups.gsc", "powerup_drop's count, of a drop outside the map",
      "level.powerup_drop_count--;",
      "bo1_drops(k,-1);" },
    // Five's thief gives what it took back to the team of the player who killed it (self.attacker, whose points the
    // kill is); to everyone, killed by no player. The comment above the function makes the room.
    { "maps/_zombiemode_ai_thief.gsc", "thief_return_loot's team",
      "// give back whatever was taken //----------------------------------------------------------------- "
      "thief_return_loot() { players = getplayers();",
      "thief_return_loot(){players=getplayers();if(IsDefined(self.attacker)&&IsPlayer(self.attacker))"
      "players=bo1_mates(bo1_team(self.attacker));" },
    // what grows with the players grows with a team's: the thief's health and its lower health in the chase, George's
    // (Call of the Dead), the astronaut's (Moon), the napalm zombie's (Shangri-La)
    { "maps/_zombiemode_ai_thief.gsc", "thief_scale_health's players",
      "thief_scale_health( health ) { players = getplayers();",
      "thief_scale_health(health){players=bo1_big();" },
    { "maps/_zombiemode_ai_thief.gsc", "thief_adjust_health's players",
      "// lower health during the chase if ( self.health > GetDvarInt( #\"scr_thief_health_endgame\" ) ) { players = "
      "getplayers();",
      "if(self.health>GetDvarInt(#\"scr_thief_health_endgame\")){players=bo1_big();" },
    { "maps/_zombiemode_ai_director.gsc", "director_reset_health's players",
      "director_reset_health( easy ) { players = getplayers(); num_players = players.size;",
      "director_reset_health(easy){players=bo1_big();num_players=players.size;" },
    { "maps/_zombiemode_ai_astro.gsc", "the astronaut's health",
      "self.maxhealth = level.zombie_health * GetPlayers().size * level.astro_zombie_health_mult;",
      "self.maxhealth=level.zombie_health*bo1_big().size*level.astro_zombie_health_mult;" },
    { "maps/_zombiemode_ai_napalm.gsc", "the napalm zombie's health",
      "self.maxhealth *= (GetPlayers().size * level.napalmHealthMultiplier);",
      "self.maxhealth*=(bo1_big().size*level.napalmHealthMultiplier);" },
    // no player's weapon does anything to another player: in the place of a comment, before the function's own
    // reckoning of who is still up
    { "maps/_zombiemode.gsc", "player_damage_override's other players",
      "// WW (8/20/10) - Sledgehammer fix for Issue 43492. This should stop the player from taking any damage while "
      "in laststand",
      "if(IsDefined(eAttacker)&&IsPlayer(eAttacker)&&eAttacker!=self)return 0;" },
};

// mod: Zombie Cap, a room's mod (zombies-hub's server/mods.go, bo1_mod_zombiecap <n>): n zombies at once, where the
// level keeps to 24 ("must have for network purposes", its script says, of 2010's consoles). Every map's spawning waits
// on level.zombie_ai_limit, and Moon's QED adds as many to the round; a round's total is as it was. 32 at most, the
// engine's own count of actors (MAX_ACTORS_RETAIL), which SetAILimit takes no more than and SpawnActor refuses a spawn
// past: more is bo1_mod_maxactors, which only a client in the server's own process knows of yet. The comment above the
// line makes the room.
static const ScrModSourcePatch s_scrZombieCap[] =
{
    { "maps/_zombiemode.gsc", "the zombies at once",
      "//Limit zombie to 24 max, must have for network purposes if ( !isdefined( level.zombie_ai_limit ) ) { "
      "level.zombie_ai_limit = 24;",
      "if(!isdefined(level.zombie_ai_limit)){level.zombie_ai_limit=GetDvarInt(\"bo1_mod_zombiecap\");" },
};

// zombies: a room of more than 4 players (bo1_players, lobby -max-players: up to 8). The levels were made for 4, and
// their scripts go by a player's number (GetEntityNumber, entity_num) as if it were 0 to 3: the fifth player had no
// character, nor arms, nor voice, and no spawn point, the thread that placed them dying on it at the map's origin; some
// maps' teleporters, seats and plates stopped at 4, and so did the co-op difficulty tables. The server's source text
// is rewritten as it is compiled, as for the fixes above: a player past the fourth takes the character of the player 4
// before them (the fifth the first's), and a spawn point again, starting near it; the rest share what there are 4 of,
// and of two who stand in one place the later steps aside, where SP downed both. What the scripts look up by the
// number of players has rows for 1 to 4, and reads the fourth's (the starting points, 500 at every size; the co-op
// difficulty's tables; as the points of a kill, the box's moves and the blockers' kills stop at 4's by themselves).
// What they multiply by it is left to grow: a round's zombies, hellhounds, crawlers, monkeys, George's health.
// Each rewrite leaves players 1 to 4 as they were, and none of it is done in a game of 4 or fewer, which runs the
// retail scripts. The client scripts need nothing: what they keep of a player goes by their entity, with no bound, or
// by their model.
static const ScrModSourcePatch s_scrMorePlayers[] =
{
    // Every switch on a player's character (entity_num: arms, voice, favourite weapon, hero and rival lines, respawn
    // points) has cases 0 to 3 alone. Nothing needs it unique: damage_assists' payout is commented out.
    { "maps/_zombiemode.gsc", "onPlayerConnect's character number",
      "player.entity_num = player GetEntityNumber();",
      "player.entity_num=player GetEntityNumber()%4;" },
    // structs[4] was undefined: player 5 stayed at the map's origin, and never respawned (spectator_respawn).
    // Players past the spawn points take one again, and a spot near it (src/game_sp/g_scr_sp_players.cpp:
    // bo1_spawnspot).
    { "maps/_zombiemode.gsc", "coop_player_spawn_placement's spawn points",
      "//chrisp - adding support for overriding the default spawning method players = get_players(); for( i = 0; i < "
      "players.size; i++ ) { players[i] setorigin( structs[i].origin ); players[i] setplayerangles( "
      "structs[i].angles ); players[i].spectator_respawn = structs[i]; }",
      "players=get_players();for(i=0;i<players.size;i++){s=structs[i%structs.size];o=s.origin;"
      "if(i>=structs.size)o=players[i] bo1_spawnspot(o,s.angles);players[i] setorigin(o);players[i] "
      "setplayerangles(s.angles);players[i].spectator_respawn=s;}" },
    // mp/zombiemode.csv has rows for 1 to 4 players; 5 got the script's 3000 points to start with. min is a float.
    { "maps/_zombiemode.gsc", "the starting points of a party's size",
      "players = get_players(); points = 500; // Get individual starting points points = set_zombie_var( "
      "(\"zombie_score_start_\"+players.size+\"p\"), 3000, false, column );",
      "players=get_players();points=500;"
      "points=set_zombie_var((\"zombie_score_start_\"+int(min(players.size,4))+\"p\"),3000,false,column);" },
    // the power-ups' first drop, from the same row (and its level.zombie_vars, which the line above sets)
    { "maps/_zombiemode_powerups.gsc", "watch_for_drop's starting points",
      "players = get_players(); score_to_drop = ( players.size * "
      "level.zombie_vars[\"zombie_score_start_\"+players.size+\"p\"] ) + "
      "level.zombie_vars[\"zombie_powerup_drop_increment\"];",
      "players=get_players();score_to_drop=players.size*"
      "level.zombie_vars[\"zombie_score_start_\"+int(min(players.size,4))+\"p\"]"
      "+level.zombie_vars[\"zombie_powerup_drop_increment\"];" },
    // every map's give_player_model_override has cases 0 to 3 alone: player 5 had no character
    { "maps/_zombiemode_load.gsc", "the character model's number",
      "self [[ level.zombiemode_give_player_model_override ]]( self GetEntityNumber() );",
      "self [[level.zombiemode_give_player_model_override]](self GetEntityNumber()%4);" },
    // where onPlayerConnect hasn't set it yet
    { "maps/_zombiemode_load.gsc", "player_set_viewmodel's character number",
      "self.entity_num =  self GetEntityNumber();",
      "self.entity_num=self GetEntityNumber()%4;" },
    // Kino's and Five's characters (no override)
    { "maps/_loadout.gsc", "give_model's character number",
      "// BJoyal (6/1/09) Used to determine if it is player 1, 2, 3, or 4, to give the appropriate model. entity_num "
      "= self GetEntityNumber();",
      "entity_num=self GetEntityNumber()%4;" },
    // Moon: by the order they connected, the last Richtofen: 0, 1, 2, 3, 4 (none), 3 for six
    { "maps/zombie_moon.gsc", "give_player_model_override's character",
      "else { self.zm_random_char = level._override_num_chars_connected; self.entity_num = "
      "level._override_num_chars_connected; entity_num = level._override_num_chars_connected; }",
      "else{entity_num=level._override_num_chars_connected%4;self.zm_random_char=entity_num;"
      "self.entity_num=entity_num;}" },
    // Moon's teleporter: target_positions has 4 spots, and [4] ended the teleport of those after
    { "maps/zombie_moon_teleporter.gsc", "teleport_player_to_target's spot",
      "player_index = player GetEntityNumber();",
      "player_index=player GetEntityNumber()%4;" },
    // Shi No Numa's own coop_player_spawn_placement (above)
    { "maps/zombie_cod5_sumpf.gsc", "sumpf_player_spawn_placement's spawn points",
      "// override to prevent player spawning on roof "
      "//------------------------------------------------------------------------------- "
      "sumpf_player_spawn_placement() { structs = getstructarray( \"initial_spawn_points\", \"targetname\" ); "
      "flag_wait( \"all_players_connected\" ); players = get_players(); for( i = 0; i < players.size; i++ ) { "
      "players[i] setorigin( structs[i].origin ); players[i] setplayerangles( structs[i].angles ); "
      "players[i].spectator_respawn = structs[i]; }",
      "sumpf_player_spawn_placement(){structs=getstructarray(\"initial_spawn_points\",\"targetname\");"
      "flag_wait(\"all_players_connected\");players=get_players();for(i=0;i<players.size;i++){"
      "s=structs[i%structs.size];o=s.origin;if(i>=structs.size)o=players[i] bo1_spawnspot(o,s.angles);players[i] "
      "setorigin(o);players[i] setplayerangles(s.angles);players[i].spectator_respawn=s;}" },
    // Verrückt: players 1 and 2 on one side, 3 and 4 on the other (side2[2], [3]); side2[4] was undefined. Of the
    // 4 points a side, 5 and 6 take the first side's other two, 7 and 8 the second's.
    { "maps/zombie_cod5_asylum.gsc", "spawn_point_override's spawn points",
      "//spawn players on a specific side, but randomize it up a bit for( i = 0; i < players.size; i++ ) { if(i<2) { "
      "players[i] setorigin( side1[i].origin ); players[i] setplayerangles( side1[i].angles ); "
      "players[i].respawn_point = side1[i]; players[i].spawn_side = side1[i].script_noteworthy; "
      "players[i].spectator_respawn = side1[i]; } else { players[i] setorigin( side2[i].origin); players[i] "
      "setplayerangles( side2[i].angles); players[i].respawn_point = side2[i]; players[i].spawn_side = "
      "side2[i].script_noteworthy; players[i].spectator_respawn = side2[i]; } }",
      "for(i=0;i<players.size;i++){s=side1[i];if(i>=2)s=side2[i];if(i>=4)s=side1[2+i%2];if(i>=6)s=side2[i%2];"
      "players[i] setorigin(s.origin);players[i] setplayerangles(s.angles);players[i].respawn_point=s;"
      "players[i].spawn_side=s.script_noteworthy;players[i].spectator_respawn=s;}" },
    // Kino's teleporter: a fifth player on the pad read dest_room[4], undefined, with the players frozen in the black
    // room. Its spots for each player, the 4 again past them.
    { "maps/zombie_theater_teleporter.gsc", "get_array_spots' spots",
      "get_array_spots(sname, spots) { for (i = 0; i < 4; i++) { spots[i] = getent(sname + i, \"targetname\"); } "
      "return spots; }",
      "get_array_spots(n,s){for(i=0;i<4||i<GetPlayers().size;i++)s[i]=getent(n+i%4,\"targetname\");return s;}" },
    // Der Riese's teleporter took players 1 to 4 to the black room and the core, and left the others on the pad
    { "maps/zombie_cod5_factory_teleporter.gsc", "teleport_players' spots",
      "// send players to a black room to flash images for a few seconds for ( i = 0; i < 4; i++ ) { core_pos[i] = "
      "getent( \"origin_teleport_player_\" + i, \"targetname\" ); occupied[i] = false; image_room[i] = getent( "
      "\"teleport_room_\" + i, \"targetname\" );",
      "for(i=0;i<4||i<players.size;i++){core_pos[i]=getent(\"origin_teleport_player_\"+i%4,\"targetname\");"
      "occupied[i]=false;image_room[i]=getent(\"teleport_room_\"+i%4,\"targetname\");" },
    // Ascension's lander has 4 seats: a fifth player had none (spots[-1]) as the intro links them all to it
    { "maps/zombie_cosmodrome_lander.gsc", "lock_players_intro's seat",
      "grab = -1; for ( j = 0; j < 4; j++ ) { if ( isdefined( taken[j] ) && taken[j] == 1 ) { continue; } grab = j; }",
      "grab=i%4;for(j=0;j<4;j++){if(isdefined(taken[j])&&taken[j]==1){continue;}grab=j;}" },
    { "maps/zombie_cosmodrome_lander.gsc", "a rider's seat",
      "max_dist = 10000; grab = -1;",
      "max_dist=10000;grab=i%4;" },
    // the last of the lander's seats, shared once they are taken
    { "maps/zombie_cosmodrome_lander.gsc", "a downed rider's seat",
      "max_dist = 196; grab = -1;",
      "max_dist=196;grab=i%4;" },
    // Shangri-La: one plate a player, of 4: with 5 the machine never opened
    { "maps/zombie_temple_pack_a_punch.gsc", "Pack-a-Punch's plates",
      "num_plates_needed = players.size; //}",
      "num_plates_needed=min(players.size,4);" },
    // Shangri-La's easter egg: its skit has 4 lines, a character's each, and an undefined line silenced every
    // player's voice for good (level.skit_vox_override)
    { "maps/zombie_temple_sq_skits.gsc", "fail_skit's line of the group",
      "for(i = 0; i < proposed_group.size; i ++) { level thread do_skit_line(skit[proposed_group[i] "
      "GetEntityNumber()]);",
      "for(i=0;i<proposed_group.size;i++){level thread do_skit_line(skit[proposed_group[i] GetEntityNumber()%4]);" },
    { "maps/zombie_temple_sq_skits.gsc", "fail_skit's line of one player",
      "player = players[RandomIntRange(0, players.size)]; level thread do_skit_line(skit[player GetEntityNumber()]);",
      "player=players[RandomIntRange(0,players.size)];level thread do_skit_line(skit[player GetEntityNumber()%4]);" },
    // the line of the player who breaks a skit up, by their number: "vox_plr_4_safety_0" is no one's, and the skit
    // waits for the line's end with the players' voices off (level.skit_vox_override). The speaker is past the fourth
    // only where the first four have all left (get_players()[0]).
    { "maps/zombie_temple_sq_skits.gsc", "skit_interupt's speaker",
      "character = speaker GetEntityNumber();",
      "character=speaker GetEntityNumber()%4;" },
    // Shangri-La's easter egg lines: a character's variant, Dempsey's for any other number
    { "maps/zombie_temple_sq.gsc", "get_variant_from_entity_num's character",
      "post_fix = \"a\"; switch(player_number)",
      "post_fix=\"a\";switch(player_number%4)" },
    // and the radio's, of the player who pressed the first step's switch (level._player_who_pressed_the_switch, their
    // number, which also finds them among the players)
    { "maps/zombie_temple_sq_brock.gsc", "radio2_override's character",
      "post_fix = \"a\"; switch(player_number)",
      "post_fix=\"a\";switch(player_number%4)" },
    // Five: the thief's health in its chase is 4 players' with 4, and a quarter of it with more
    { "maps/_zombiemode_ai_thief.gsc", "the thief's health of four players or more",
      "if ( players.size == 4 )",
      "if(players.size>=4)" },
    // Two players within 18 units of each other both took 1000 damage, checked every 0.5 s, which downed them: SP's
    // guard against standing in each other. What players past the fourth share (a lander's seat, a teleporter's spot,
    // a character's respawn point) puts two there: Ascension's lander downed all who shared a seat as it landed. The
    // later of the two, unless linked to something (a seat, a teleporter's black room), steps aside to a clear spot
    // near them instead.
    { "maps/_zombiemode_spawner.gsc", "track_players_intersection_tracker's two players in one place",
      "//IPrintLnBold( \"PLAYERS ARE TOO FRIENDLY!!!!!\" ); players[i] dodamage( 1000, (0, 0, 0) ); players[j] "
      "dodamage( 1000, (0, 0, 0) ); if ( !killed_players ) { if( is_true( level.player_4_vox_override ) ) { "
      "players[i] playlocalsound( \"zmb_laugh_rich\" ); } else { players[i] playlocalsound( \"zmb_laugh_child\" ); } "
      "} killed_players = true;",
      "p=players[i];if(p GetEntityNumber()<players[j] GetEntityNumber())p=players[j];if(!isdefined(p "
      "getlinkedent()))p setorigin(p bo1_spawnspot(p.origin,p.angles));" },
    // The co-op difficulty tables (coopPlayerDifficultyHealth, ...) have 1 to 4 players: past them the players' damage
    // multiplier and their invulnerability after a hit went unset, script errors as all players spawned. 4's.
    { "maps/_gameskill.gsc", "getCoopValue's number of players",
      "if (numplayers <= 0) { numplayers = 1; } // value = ( level.difficultySettings[ msg ][ GetDvar( "
      "#\"currentDifficulty\" ) ][numplayers - 1]);",
      "if(numplayers<=0)numplayers=1;if(numplayers>4)numplayers=4;" },
};

static bool Scr_ModIsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool Scr_ModIsIdent(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// end of the whitespace-insensitive match of pattern at src[start], or -1
static int Scr_ModMatchLoose(const char *src, int len, int start, const char *pattern)
{
    int s = start;
    for (const char *p = pattern; *p; ++p)
    {
        if (Scr_ModIsSpace(*p))
            continue;
        while (s < len && Scr_ModIsSpace(src[s]))
            ++s;
        if (s >= len || tolower((unsigned char)src[s]) != tolower((unsigned char)*p))
            return -1;
        ++s;
    }
    return s;
}

static void Scr_PatchSource(const ScrModSourcePatch *patches, int count, const char *file, char *src, int len,
    const char *what)
{
    for (int p = 0; p < count; ++p)
    {
        const ScrModSourcePatch &patch = patches[p];
        if (I_stricmp(file, patch.file))
            continue;
        int start = 0, end = -1;
        for (; start < len; ++start)
        {
            if (tolower((unsigned char)src[start]) != tolower((unsigned char)patch.original[0])
                || (start && Scr_ModIsIdent(src[start - 1])))
                continue;
            end = Scr_ModMatchLoose(src, len, start, patch.original);
            if (end >= 0)
                break;
        }
        int newlines = 0;
        for (int i = start; i < end; ++i)
            newlines += src[i] == '\n';
        const int replacementLen = (int)strlen(patch.replacement);
        if (end < 0 || replacementLen + newlines > end - start)
        {
            Com_Printf(23, "%s: %s: %s not found, left as is\n", what, file, patch.name);
            continue;
        }
        memcpy(src + start, patch.replacement, replacementLen);
        memset(src + start + replacementLen, ' ', end - start - replacementLen - newlines);
        memset(src + end - newlines, '\n', newlines);
        Com_Printf(23, "%s: %s: %s rewritten\n", what, file, patch.name);
    }
}

static void Scr_ModPatchSource(scriptInstance_t inst, const char *extFilename, char *src, int len)
{
    static const dvar_s *scriptPerf, *uniqueWallWeapons, *grief, *zombieCap, *players;
    if (inst != SCRIPTINSTANCE_SERVER || !src || len <= 0)
        return;
    char file[64];
    I_strncpyz(file, extFilename, sizeof(file));
    for (char *c = file; *c; ++c)
        if (*c == '\\')
            *c = '/';
    Scr_PatchSource(s_scrSourceFixes, ARRAY_COUNT(s_scrSourceFixes), file, src, len, "scripts");
    if (!scriptPerf)
        scriptPerf = Dvar_FindVar("bo1_mod_scriptperf");
    if (scriptPerf && scriptPerf->current.integer)
        Scr_PatchSource(s_scrModSourcePatches, ARRAY_COUNT(s_scrModSourcePatches), file, src, len, "bo1_mod_scriptperf");
    if (!uniqueWallWeapons)
        uniqueWallWeapons = Dvar_FindVar("bo1_mod_uniquewallweapons");
    if (uniqueWallWeapons && uniqueWallWeapons->current.integer)
        Scr_PatchSource(s_scrUniqueWallWeapons, ARRAY_COUNT(s_scrUniqueWallWeapons), file, src, len, "bo1_mod_uniquewallweapons");
    if (!grief)
        grief = Dvar_FindVar("bo1_mod_grief");
    if (grief && grief->current.integer)
        Scr_PatchSource(s_scrGrief, ARRAY_COUNT(s_scrGrief), file, src, len, "bo1_mod_grief");
    if (!zombieCap)
        zombieCap = Dvar_FindVar("bo1_mod_zombiecap");
    if (zombieCap && zombieCap->current.integer)
        Scr_PatchSource(s_scrZombieCap, ARRAY_COUNT(s_scrZombieCap), file, src, len, "bo1_mod_zombiecap");
    if (!players)
        players = Dvar_FindVar("bo1_players"); // src/server_mp/sv_lobby_mp.cpp
    if (players && players->current.integer > 4)
        Scr_PatchSource(s_scrMorePlayers, ARRAY_COUNT(s_scrMorePlayers), file, src, len, "players past 4");
}

char *__cdecl Scr_ReadFile_LoadObj(
    scriptInstance_t inst,
    const char *filename,
    char *extFilename,
    const char *codePos,
    bool archive)
{
    int len; // [esp+4h] [ebp-Ch]
    int f; // [esp+8h] [ebp-8h] BYREF
    char *sourceBuf; // [esp+Ch] [ebp-4h]

    len = FS_FOpenFileByMode(extFilename, &f, FS_READ);
    if (len >= 0)
    {
        if (!FS_ModsActive()) // mod: was 'fs_game unset' (fs_mods is a mod too)
            g_loadedImpureScript = 1;
        sourceBuf = (char *)Hunk_AllocateTempMemoryHigh(len + 1, "Scr_ReadFile");
        FS_Read((unsigned __int8 *)sourceBuf, len, f);
        sourceBuf[len] = 0;
        FS_FCloseFile(f);
        Scr_ModPatchSource(inst, extFilename, sourceBuf, len); // mod (L43) bo1_mod_scriptperf
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, sourceBuf, len, 1, archive);
        return sourceBuf;
    }
    else
    {
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, 0, -1, 1, archive);
        return 0;
    }
}

char *__cdecl Scr_ReadFile(
                scriptInstance_t inst,
                const char *filename,
                char *extFilename,
                const char *codePos,
                bool archive)
{
    int file; // [esp+34h] [ebp-4h] BYREF

    // mod: fs_mods - maps/_modstack_list.gsc is written here, not read: register_all() calls maps\<name>\_hooks::register()
    // of each stacked mod that has that file, in load order (FS_ModStackNames). mods/_stack/maps/_modstack.gsc calls it.
    if ( FS_ModStackActive() && !I_stricmp(extFilename, "maps/_modstack_list.gsc") )
    {
        char names[16][64];
        int count = FS_ModStackNames(names, 16);
        const int size = 256 + 16 * 96;
        char *sourceBuf = (char *)Hunk_AllocateTempMemoryHigh(size, "Scr_ReadFile");
        int len = sprintf_s(sourceBuf, size, "// generated by the engine from fs_game / fs_mods (Scr_ReadFile)\nregister_all()\n{\n");
        for ( int m = 0; m < count; ++m )
        {
            if ( FS_FOpenFileRead(va("maps/%s/_hooks.gsc", names[m]), &file) < 0 )
                continue;
            FS_FCloseFile(file);
            len += sprintf_s(sourceBuf + len, size - len, "\tmaps\\%s\\_hooks::register();\n", names[m]);
            Com_Printf(24, "fs_mods: hooks of mods/%s\n", names[m]);
        }
        len += sprintf_s(sourceBuf + len, size - len, "}\n");
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, sourceBuf, len, 1, archive);
        return sourceBuf;
    }
    if ( FS_ModsActive() ) // mod: was 'fs_game set': with fs_mods the stacked mods' files win over the fastfile's too
    {
        if ( (FS_FOpenFileRead(extFilename, &file) & 0x80000000) != 0 )
        {
            return Scr_ReadFile_FastFile(inst, filename, extFilename, codePos, archive);
        }
        else
        {
            FS_FCloseFile(file);
            return Scr_ReadFile_LoadObj(inst, filename, extFilename, codePos, archive);
        }
    }
    else if ( useFastFile->current.enabled )
    {
        return Scr_ReadFile_FastFile(inst, filename, extFilename, codePos, archive);
    }
    else
    {
        return Scr_ReadFile_LoadObj(inst, filename, extFilename, codePos, archive);
    }
}

char *__cdecl Scr_ReadFile_FastFile(
                scriptInstance_t inst,
                const char *filename,
                const char *extFilename,
                const char *codePos,
                bool archive)
{
    uLongf outlen; // [esp+14h] [ebp-18h] BYREF
    unsigned int inlen; // [esp+18h] [ebp-14h]
    RawFile *rawfile; // [esp+1Ch] [ebp-10h]
    const char *extension; // [esp+20h] [ebp-Ch]
    int len; // [esp+24h] [ebp-8h]
    char *sourceBuf; // [esp+28h] [ebp-4h]

    if ( useFastFile->current.enabled
        && (rawfile = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, (char*)extFilename, 1, -1).rawfile) != 0 )
    {
        extension = Com_GetExtensionSubString(extFilename);
        if ( I_stricmp(extension, ".gsc") && I_stricmp(extension, ".csc") )
        {
            sourceBuf = (char *)rawfile->buffer;
            len = strlen(sourceBuf);
        }
        else
        {
            len = *(unsigned int *)rawfile->buffer;
            outlen = len;
            inlen = *((unsigned int *)rawfile->buffer + 1);
            sourceBuf = (char *)Hunk_AllocateTempMemoryHigh(len, "Scr_ReadFile");
            uncompress((unsigned __int8 *)sourceBuf, &outlen, (unsigned __int8 *)rawfile->buffer + 8, inlen);
            if ( len != outlen
                && !Assert_MyHandler(
                            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                            882,
                            0,
                            "%s",
                            "(uLongf)len==outlen") )
            {
                __debugbreak();
            }
            Scr_ModPatchSource(inst, extFilename, sourceBuf, len); // mod (L43) bo1_mod_scriptperf (own buffer)
        }
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, sourceBuf, len, 1, archive);
        return sourceBuf;
    }
    else
    {
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, 0, -1, 1, archive);
        return 0;
    }
}

char *__cdecl x(
                scriptInstance_t inst,
                const char *filename,
                char *extFilename,
                const char *codePos,
                bool archive)
{
    int len; // [esp+4h] [ebp-Ch]
    int f; // [esp+8h] [ebp-8h] BYREF
    char *sourceBuf; // [esp+Ch] [ebp-4h]

    len = FS_FOpenFileByMode(extFilename, &f, FS_READ);
    if ( len >= 0 )
    {
        if ( !FS_ModsActive() ) // mod: was 'fs_game unset' (fs_mods is a mod too)
            g_loadedImpureScript = 1;
        sourceBuf = (char *)Hunk_AllocateTempMemoryHigh(len + 1, "Scr_ReadFile");
        FS_Read((unsigned __int8 *)sourceBuf, len, f);
        sourceBuf[len] = 0;
        FS_FCloseFile(f);
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, sourceBuf, len, 1, archive);
        return sourceBuf;
    }
    else
    {
        Scr_AddSourceBufferInternal(inst, extFilename, codePos, 0, -1, 1, archive);
        return 0;
    }
}

unsigned int __cdecl Scr_GetSourcePos(
    scriptInstance_t inst,
    unsigned int bufferIndex,
    unsigned int sourcePos,
    char *outBuf,
    unsigned int outBufLen)
{
    unsigned int lineNum; // [esp+4h] [ebp-40Ch]
    char line[1024]; // [esp+8h] [ebp-408h] BYREF
    int col; // [esp+40Ch] [ebp-4h] BYREF

    if (!gScrVarPub[inst].developer
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1190,
            0,
            "%s",
            "gScrVarPub[inst].developer"))
    {
        __debugbreak();
    }
    lineNum = Scr_GetLineInfo(gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf, sourcePos, &col, line);
    if (gScrParserGlob[inst].saveSourceBufferLookup)
        Com_sprintf(
            outBuf,
            outBufLen,
            "%s // %s%s, line %d",
            line,
            gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf,
            " (savegame)",
            lineNum + 1);
    else
        Com_sprintf(
            outBuf,
            outBufLen,
            "%s // %s%s, line %d",
            line,
            gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf,
            "",
            lineNum + 1);
    return lineNum;
}

unsigned int __cdecl Scr_GetLineInfo(const char *buf, unsigned int sourcePos, int *col, char *line)
{
    const char *startLine; // [esp+0h] [ebp-8h] BYREF
    unsigned int lineNum; // [esp+4h] [ebp-4h]

    if ( buf )
    {
        lineNum = Scr_GetLineNumInternal(buf, sourcePos, &startLine, col);
    }
    else
    {
        lineNum = 0;
        startLine = "";
        *col = 0;
    }
    Scr_CopyFormattedLine(line, startLine);
    return lineNum;
}

void __cdecl Scr_CopyFormattedLine(char *line, const char *rawLine)
{
    char v2; // [esp+3h] [ebp-1Dh]
    int len; // [esp+18h] [ebp-8h]
    int i; // [esp+1Ch] [ebp-4h]

    len = strlen(rawLine);
    if ( len >= 1024 )
        len = 1023;
    for ( i = 0; i < len; ++i )
    {
        if ( rawLine[i] == 9 )
            v2 = 32;
        else
            v2 = rawLine[i];
        line[i] = v2;
    }
    if ( line[len - 1] == 13 )
        line[len - 1] = 0;
    line[len] = 0;
}

unsigned int __cdecl Scr_GetSourceBuffer(scriptInstance_t inst, const char *codePos)
{
    unsigned int bufferIndex; // [esp+0h] [ebp-4h]

    if ( !Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    1202,
                    0,
                    "%s",
                    "Scr_IsInOpcodeMemory( inst, codePos )") )
    {
        __debugbreak();
    }
    if ( !gScrParserPub[inst].sourceBufferLookupLen
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                    1203,
                    0,
                    "%s",
                    "gScrParserPub[inst].sourceBufferLookupLen > 0") )
    {
        __debugbreak();
    }
    for ( bufferIndex = gScrParserPub[inst].sourceBufferLookupLen - 1;
                bufferIndex
         && (!gScrParserPub[inst].sourceBufferLookup[bufferIndex].codePos
            || gScrParserPub[inst].sourceBufferLookup[bufferIndex].codePos > codePos);
                --bufferIndex )
    {
        ;
    }
    return bufferIndex;
}

void __cdecl Scr_PrintPrevCodePos(scriptInstance_t inst, int channel, char *codePos, unsigned int index)
{
    char *v4; // eax
    unsigned int PrevSourcePos; // eax
    char *v6; // eax
    unsigned int bufferIndex; // [esp+0h] [ebp-4h]

    if (!codePos)
    {
        Com_PrintMessage(channel, (char*)"<frozen thread>\n", 0);
        return;
    }
    if (codePos == &g_EndPos)
    {
        Com_PrintMessage(channel, (char *)"<removed thread>\n", 0);
    }
    else
    {
        if (gScrVarPub[inst].developer)
        {
            if (gScrVarPub[inst].programBuffer && Scr_IsInOpcodeMemory(inst, codePos))
            {
                bufferIndex = Scr_GetSourceBuffer(inst, codePos - 1);
                PrevSourcePos = Scr_GetPrevSourcePos(inst, codePos - 1, index);
                Scr_PrintSourcePos(
                    inst,
                    channel,
                    gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf,
                    gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf,
                    PrevSourcePos);
                return;
            }
        }
        else if (Scr_IsInOpcodeMemory(inst, codePos - 1))
        {
            v4 = va("@ %d\n", codePos - gScrVarPub[inst].programBuffer);
            Com_PrintMessage(channel, v4, 0);
            return;
        }
        v6 = va("%s\n\n", codePos);
        Com_PrintMessage(channel, v6, 0);
    }
}

void __cdecl Scr_PrintSourcePos(
    scriptInstance_t inst,
    int channel,
    const char *filename,
    const char *buf,
    unsigned int sourcePos)
{
    char *v5; // eax
    char *v6; // eax
    const char *v7; // [esp+0h] [ebp-418h]
    unsigned int lineNum; // [esp+4h] [ebp-414h]
    char line[1028]; // [esp+8h] [ebp-410h] BYREF
    int i; // [esp+410h] [ebp-8h]
    int col; // [esp+414h] [ebp-4h] BYREF

    if (!filename
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 1136, 0, "%s", "filename"))
    {
        __debugbreak();
    }
    lineNum = Scr_GetLineInfo(buf, sourcePos, &col, line);
    if (gScrParserGlob[inst].saveSourceBufferLookup)
        v7 = " (savegame)";
    else
        v7 = "";
    v5 = va("(file '%s'%s, line %d)\n", filename, v7, lineNum + 1);
    Com_PrintMessage(channel, v5, 0);
    v6 = va("%s\n", line);
    Com_PrintMessage(channel, v6, 0);
    for (i = 0; i < col; ++i)
        Com_PrintMessage(channel, (char*)" ", 0);
    Com_PrintMessage(channel, (char *)"*\n", 0);
}

char *__cdecl Scr_PrevCodePosFileName(scriptInstance_t inst, char *codePos)
{
    if (!gScrVarPub[inst].developer
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1319,
            0,
            "%s",
            "gScrVarPub[inst].developer"))
    {
        __debugbreak();
    }
    if (!codePos)
        return (char*)"<frozen thread>";
    if (codePos == &g_EndPos)
        return (char *)"<removed thread>";
    if (gScrVarPub[inst].programBuffer && Scr_IsInOpcodeMemory(inst, codePos))
        return gScrParserPub[inst].sourceBufferLookup[Scr_GetSourceBuffer(inst, codePos - 1)].buf;
    return codePos;
}

const char *__cdecl Scr_PrevCodePosFunctionName(scriptInstance_t inst, char *codePos)
{
    unsigned int PrevSourcePos; // eax
    unsigned int bufferIndex; // [esp+0h] [ebp-8h]
    const char *startLine; // [esp+4h] [ebp-4h] BYREF

    if (!gScrVarPub[inst].developer
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1367,
            0,
            "%s",
            "gScrVarPub[inst].developer"))
    {
        __debugbreak();
    }
    if (!codePos)
        return "<frozen thread>";
    if (codePos == &g_EndPos)
        return "<removed thread>";
    if (!gScrVarPub[inst].programBuffer || !Scr_IsInOpcodeMemory(inst, codePos))
        return "<unknown>";
    bufferIndex = Scr_GetSourceBuffer(inst, codePos - 1);
    PrevSourcePos = Scr_GetPrevSourcePos(inst, codePos - 1, 0);
    Scr_GetFunctionLineNumInternal(
        gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf,
        PrevSourcePos,
        &startLine);
    return startLine;
}

bool __cdecl Scr_PrevCodePosFileNameMatches(scriptInstance_t inst, char *codePos, const char *fileName)
{
    char *codePosFileName; // [esp+0h] [ebp-4h]

    if (!fileName
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 1422, 0, "%s", "fileName"))
    {
        __debugbreak();
    }
    if (!gScrVarPub[inst].developer
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1423,
            0,
            "%s",
            "gScrVarPub[inst].developer"))
    {
        __debugbreak();
    }
    codePosFileName = Scr_PrevCodePosFileName(inst, codePos);
    return codePosFileName && I_stristr(codePosFileName, fileName) != 0;
}

void __cdecl Scr_PrintPrevCodePosSpreadSheet(
    scriptInstance_t inst,
    int channel,
    char *codePos,
    bool summary,
    bool functionSummary)
{
    unsigned int PrevSourcePos; // eax
    char *v6; // eax
    unsigned int bufferIndex; // [esp+0h] [ebp-4h]

    if (!gScrVarPub[inst].developer
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1438,
            0,
            "%s",
            "gScrVarPub[inst].developer"))
    {
        __debugbreak();
    }
    if (codePos)
    {
        if (codePos == &g_EndPos)
        {
            Com_PrintMessage(channel, (char *)"<removed thread>\n", 0);
        }
        else if (gScrVarPub[inst].programBuffer && Scr_IsInOpcodeMemory(inst, codePos))
        {
            bufferIndex = Scr_GetSourceBuffer(inst, codePos - 1);
            if (summary)
            {
                Scr_GetPrevSourcePos(inst, codePos - 1, 0);
                Scr_PrintSourcePosSummary(inst, channel, gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf);
            }
            else
            {
                PrevSourcePos = Scr_GetPrevSourcePos(inst, codePos - 1, 0);
                if (functionSummary)
                    Scr_PrintFunctionPosSpreadSheet(
                        inst,
                        channel,
                        gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf,
                        gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf,
                        PrevSourcePos);
                else
                    Scr_PrintSourcePosSpreadSheet(
                        inst,
                        channel,
                        gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf,
                        gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf,
                        PrevSourcePos);
            }
        }
        else
        {
            v6 = va("%s\n", codePos);
            Com_PrintMessage(channel, v6, 0);
        }
    }
    else
    {
        Com_PrintMessage(channel, (char*)"<frozen thread>\n", 0);
    }
}

void __cdecl Scr_PrintSourcePosSpreadSheet(
    scriptInstance_t inst,
    int channel,
    const char *filename,
    const char *buf,
    unsigned int sourcePos)
{
    char *v5; // eax
    const char *v6; // [esp+0h] [ebp-410h]
    unsigned int lineNum; // [esp+4h] [ebp-40Ch]
    char line[1024]; // [esp+8h] [ebp-408h] BYREF
    int col; // [esp+40Ch] [ebp-4h] BYREF

    if (!filename
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 1155, 0, "%s", "filename"))
    {
        __debugbreak();
    }
    lineNum = Scr_GetLineInfo(buf, sourcePos, &col, line);
    if (gScrParserGlob[inst].saveSourceBufferLookup)
        v6 = "(savegame)";
    else
        v6 = "";
    v5 = va("%s%s\t%d\t%s\t%d\n", filename, v6, lineNum + 1, line, col);
    Com_PrintMessage(channel, v5, 0);
}

void __cdecl Scr_PrintFunctionPosSpreadSheet(
    scriptInstance_t inst,
    int channel,
    const char *filename,
    const char *buf,
    unsigned int sourcePos)
{
    char *v5; // eax
    const char *v6; // [esp+0h] [ebp-410h]
    unsigned int lineNum; // [esp+4h] [ebp-40Ch]
    char line[1028]; // [esp+8h] [ebp-408h] BYREF

    if (!filename
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 1168, 0, "%s", "filename"))
    {
        __debugbreak();
    }
    lineNum = Scr_GetFunctionInfo(buf, sourcePos, line);
    if (gScrParserGlob[inst].saveSourceBufferLookup)
        v6 = "(savegame)";
    else
        v6 = "";
    v5 = va("%s%s\t%d\t%s\n", filename, v6, lineNum + 1, line);
    Com_PrintMessage(channel, v5, 0);
}

unsigned int __cdecl Scr_GetFunctionInfo(const char *buf, unsigned int sourcePos, char *line)
{
    const char *startLine; // [esp+0h] [ebp-8h] BYREF
    unsigned int lineNum; // [esp+4h] [ebp-4h]

    lineNum = Scr_GetFunctionLineNumInternal(buf, sourcePos, &startLine);
    Scr_CopyFormattedLine(line, startLine);
    return lineNum;
}

void __cdecl Scr_PrintSourcePosSummary(scriptInstance_t inst, int channel, const char *filename)
{
    char *v3; // eax
    const char *v4; // [esp+0h] [ebp-4h]

    if (!filename
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp", 1178, 0, "%s", "filename"))
    {
        __debugbreak();
    }
    if (gScrParserGlob[inst].saveSourceBufferLookup)
        v4 = "(savegame)";
    else
        v4 = "";
    v3 = va("%s%s\t\n", filename, v4);
    Com_PrintMessage(channel, v3, 0);
}

void __cdecl Scr_GetCodePos(
    scriptInstance_t inst,
    const char *codePos,
    unsigned int index,
    char *outBuf,
    unsigned int outBufLen)
{
    Scr_SourcePos_t pos; // [esp+0h] [ebp-Ch] BYREF

    if (gScrVarPub[inst].developer)
    {
        Scr_GetSourcePosOfType(inst, codePos, 4, &pos);
        Scr_GetSourcePos(inst, pos.bufferIndex, pos.sourcePos, outBuf, outBufLen);
    }
    else
    {
        if (!Scr_IsInOpcodeMemory(inst, codePos)
            && !Assert_MyHandler(
                "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
                1474,
                0,
                "%s",
                "Scr_IsInOpcodeMemory( inst, codePos )"))
        {
            __debugbreak();
        }
        Com_sprintf(outBuf, outBufLen, "@ %d", codePos - gScrVarPub[inst].programBuffer);
    }
}

void __cdecl Scr_GetFileAndLine(scriptInstance_t inst, const char *codePos, char **filename, int *linenum)
{
    unsigned int bufferIndex; // [esp+0h] [ebp-Ch]
    OpcodeLookup *opcodeLookup; // [esp+4h] [ebp-8h]
    unsigned int sourcePos; // [esp+8h] [ebp-4h]

    if (!Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1489,
            0,
            "%s",
            "Scr_IsInOpcodeMemory( inst, codePos )"))
    {
        __debugbreak();
    }
    opcodeLookup = Scr_GetPrevSourcePosOpcodeLookup(inst, codePos);
    if (opcodeLookup)
    {
        sourcePos = gScrParserGlob[inst].sourcePosLookup[opcodeLookup->sourcePosIndex].sourcePos;
        bufferIndex = Scr_GetSourceBuffer(inst, codePos);
        *linenum = Scr_GetLineNum(inst, bufferIndex, sourcePos) + 1;
        *filename = gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf;
    }
    else
    {
        *linenum = 0;
        *filename = (char *)"";
    }
}

void CompileError(scriptInstance_t inst, unsigned int sourcePos, const char *msg, ...)
{
    char line[1028]; // [esp+10h] [ebp-810h] BYREF
    int lineNumber; // [esp+414h] [ebp-40Ch]
    char text[1024]; // [esp+418h] [ebp-408h] BYREF
    int col; // [esp+81Ch] [ebp-4h] BYREF
    va_list va; // [esp+834h] [ebp+14h] BYREF

    va_start(va, msg);
    lineNumber = 0;
    _vsnprintf(text, 0x400u, msg, va);
    if (gScrVarPub[inst].evaluate)
    {
        if (!gScrVarPub[inst].error_message)
            gScrVarPub[inst].error_message = ::va("%s", text);
    }
    else
    {
        Monkey_GrabComPrints(1);
        Scr_IgnoreLeaks(inst);
        Scr_ShutdownAllocNode(inst);
        Com_PrintError(24, "\n");
        if (inst)
            Com_PrintError(24, "******* %s script compile error *******\n", "Client");
        else
            Com_PrintError(24, "******* %s script compile error *******\n", "Server");
        if (gScrVarPub[inst].developer && gScrParserPub[inst].sourceBuf)
        {
            Com_PrintError(24, "%s: ", text);
            Scr_PrintSourcePos(inst, 24, gScrParserPub[inst].scriptfilename, gScrParserPub[inst].sourceBuf, sourcePos);
            lineNumber = Scr_GetLineInfo(gScrParserPub[inst].sourceBuf, sourcePos, &col, line);
            Monkey_Error(0);
            if (inst)
                Com_Error(ERR_SCRIPT_DROP, " %s script compile error %s %s(%d): %s (see console for details)", "Client", text, gScrParserPub[inst].scriptfilename, lineNumber, line);
            else
                Com_Error(ERR_SCRIPT_DROP, " %s script compile error %s %s(%d): %s (see console for details)", "Server", text, gScrParserPub[0].scriptfilename, lineNumber, line);
        }
        else
        {
            Com_PrintError(24, "%s\n", text);
            line[0] = 0;
            Com_Printf(24, "************************************\n");
            Monkey_Error(0);
            if (inst)
                Com_Error(ERR_SCRIPT_DROP, " %s script compile error %s %s (see console for details)", "Client", text, line);
            else
                Com_Error(ERR_SCRIPT_DROP, " %s script compile error %s %s (see console for details)", "Server", text, line);
        }
    }
}

void __cdecl Scr_IgnoreLeaks(scriptInstance_t inst)
{
    if ( gScrStringDebugGlob[inst] )
        gScrStringDebugGlob[inst]->ignoreLeaks = 1;
    Scr_ClearScrVarDebugPub(inst);
}

void CompileError2(scriptInstance_t inst, char *codePos, const char *msg, ...)
{
    char line[1024]; // [esp+Ch] [ebp-808h] BYREF
    char text[1028]; // [esp+40Ch] [ebp-408h] BYREF
    va_list va; // [esp+828h] [ebp+14h] BYREF

    va_start(va, msg);
    if (gScrVarPub[inst].evaluate
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1894,
            0,
            "%s",
            "!gScrVarPub[inst].evaluate"))
    {
        __debugbreak();
    }
    if (!Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1895,
            0,
            "%s",
            "Scr_IsInOpcodeMemory( inst, codePos )"))
    {
        __debugbreak();
    }
    Monkey_GrabComPrints(1);
    Scr_IgnoreLeaks(inst);
    Com_PrintError(24, "\n");
    if (inst)
        Com_PrintError(24, "******* %s script compile error *******\n", "Client");
    else
        Com_PrintError(24, "******* %s script compile error *******\n", "Server");
    _vsnprintf(text, 0x400u, msg, va);
    Com_PrintError(24, "%s: ", text);
    Scr_PrintPrevCodePos(inst, 24, codePos, 0);
    Com_Printf(24, "************************************\n");
    Monkey_Error(0);
    Scr_GetTextSourcePos(inst, gScrParserPub[inst].sourceBuf, codePos, line);
    if (inst)
        Com_Error(ERR_SCRIPT_DROP, "%s script compile error %s %s (see console for details)", "Client", text, line);
    else
        Com_Error(ERR_SCRIPT_DROP, "%s script compile error %s %s (see console for details)", "Server", text, line);
}

void __cdecl Scr_GetTextSourcePos(scriptInstance_t inst, const char *buf, char *codePos, char *line)
{
    unsigned int PrevSourcePos; // eax
    unsigned int bufferIndex; // [esp+0h] [ebp-8h]
    int col; // [esp+4h] [ebp-4h] BYREF

    if (gScrVarPub[inst].developer
        && codePos
        && codePos != &g_EndPos
        && gScrVarPub[inst].programBuffer
        && Scr_IsInOpcodeMemory(inst, codePos))
    {
        bufferIndex = Scr_GetSourceBuffer(inst, codePos - 1);
        PrevSourcePos = Scr_GetPrevSourcePos(inst, codePos - 1, 0);
        Scr_GetLineInfo(gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf, PrevSourcePos, &col, line);
    }
    else
    {
        *line = 0;
    }
}

#include <game_sp/scr_sp_debug.h>

void __cdecl RuntimeError(
    scriptInstance_t inst,
    char *codePos,
    unsigned int index,
    const char *msg,
    const char *dialogMessage)
{
    const char *v5; // [esp+4h] [ebp-428h]
    const char *v6; // [esp+8h] [ebp-424h]
    bool v7; // [esp+Ch] [ebp-420h]
    bool abort_on_error; // [esp+13h] [ebp-419h]
    int bufferIndex; // [esp+14h] [ebp-418h]
    unsigned int lineNum; // [esp+18h] [ebp-414h]
    char line[1028]; // [esp+1Ch] [ebp-410h] BYREF
    int sourcePos; // [esp+424h] [ebp-8h]
    int col; // [esp+428h] [ebp-4h] BYREF

    // zombies: bo1_scripterrors' report (src/game_sp/scr_sp_debug.cpp), whatever the VM's developer flag: a game's
    // logfile, which is never 0 (Com_InitDvars), sets it, and the error then goes to channel 6, which no log has
    Scr_SP_ReportRuntimeError(inst, codePos, msg);
    if (gScrVarPub[inst].developer)
        goto LABEL_30;
    if (!Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1962,
            0,
            "%s",
            "Scr_IsInOpcodeMemory( inst, codePos )"))
    {
        __debugbreak();
    }
    if (gScrVmPub[inst].terminal_error)
    {
    LABEL_30:
        if (gScrVmPub[inst].debugCode)
        {
            Com_Printf(24, "%s\n", msg);
            if (!gScrVmPub[inst].terminal_error)
                return;
            goto error;
        }
        v7 = gScrVmPub[inst].abort_on_error || gScrVmPub[inst].terminal_error;
        abort_on_error = v7;
        if (Scr_IgnoreErrors(inst) && inst != SCRIPTINSTANCE_CLIENT)
            abort_on_error = 0;
        RuntimeErrorInternal(inst, abort_on_error ? 24 : 6, codePos, index, msg);
        if (abort_on_error)
        {
        error:
            if (gScrVmPub[inst].terminal_error)
                Scr_IgnoreLeaks(inst);
            Monkey_Error(0);
            bufferIndex = Scr_GetSourceBuffer(inst, codePos - 1);
            sourcePos = Scr_GetPrevSourcePos(inst, codePos - 1, index);
            lineNum = Scr_GetLineInfo(gScrParserPub[inst].sourceBufferLookup[bufferIndex].sourceBuf, sourcePos, &col, line);
            if (dialogMessage)
                v6 = dialogMessage;
            else
                v6 = "";
            if (dialogMessage)
                v5 = "\n";
            else
                v5 = "";
            if (inst == SCRIPTINSTANCE_CLIENT)
                Com_Error(
                    (errorParm_t)(gScrVmPub[1].terminal_error + 4),
                    "%s script runtime error %s%s%s %s(%d) %s",
                    "client",
                    msg,
                    v5,
                    v6,
                    gScrParserPub[1].sourceBufferLookup[bufferIndex].buf,
                    lineNum,
                    line);
            else
                Com_Error(
                    (errorParm_t)(gScrVmPub[inst].terminal_error + 4),
                    "%s script runtime error %s%s%s %s(%d) %s",
                    "server",
                    msg,
                    v5,
                    v6,
                    gScrParserPub[inst].sourceBufferLookup[bufferIndex].buf,
                    lineNum,
                    line);
        }
    }
}

void __cdecl RuntimeErrorInternal(
    scriptInstance_t inst,
    int channel,
    char *codePos,
    unsigned int index,
    const char *msg)
{
    int i; // [esp+4h] [ebp-4h]

    if (!Scr_IsInOpcodeMemory(inst, codePos)
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\clientscript\\cscr_parser.cpp",
            1924,
            0,
            "%s",
            "Scr_IsInOpcodeMemory( inst, codePos )"))
    {
        __debugbreak();
    }
    Monkey_GrabComPrints(1);
    Com_PrintError(channel, "\n******* script runtime error *******\n%s: ", msg);
    Scr_PrintPrevCodePos(inst, channel, codePos, index);
    if (gScrVmPub[inst].function_count)
    {
        for (i = gScrVmPub[inst].function_count - 1; i >= 1; --i)
        {
            Com_PrintError(channel, "called from:\n");
            Scr_PrintPrevCodePos(
                inst,
                0,
                (char *)gScrVmPub[inst].function_frame_start[i].fs.pos,
                gScrVmPub[inst].function_frame_start[i].fs.localId == 0);
        }
        Com_PrintError(channel, "started from:\n");
        Scr_PrintPrevCodePos(inst, 0, (char *)gScrVmPub[inst].function_frame_start[0].fs.pos, 1u);
    }
    Com_PrintError(channel, "************************************\n");
}

void __cdecl Scr_SetLoadedImpureScript(bool loadedImpureScript)
{
    g_loadedImpureScript = loadedImpureScript;
}

