#include "win_shared.h"

#include <Windows.h>

int sys_timeBase;

unsigned int __cdecl Sys_Milliseconds()
{
    static int initialized_1 = 0;
    if ( !initialized_1 )
    {
        sys_timeBase = timeGetTime();
        initialized_1 = 1;
    }
    return timeGetTime() - sys_timeBase;
}

unsigned int __cdecl Sys_MillisecondsRaw()
{
    return timeGetTime();
}

//void __cdecl Sys_SnapVector(float *v)
//{
//    float *va; // [esp+10h] [ebp+8h]
//
//    *v = (float)(int)*v;
//    va = v + 1;
//    *va = (float)(int)*va;
//    va[1] = (float)(int)va[1];
//}

// https://github.com/id-Software/Quake-III-Arena/blob/master/code/win32/win_shared.c
void Sys_SnapVector(float *v)
{
#ifdef _M_IX86
    int i;
    float f;

    f = *v;
    __asm	fld		f;
    __asm	fistp	i;
    *v = i;
    v++;
    f = *v;
    __asm	fld		f;
    __asm	fistp	i;
    *v = i;
    v++;
    f = *v;
    __asm	fld		f;
    __asm	fistp	i;
    *v = i;
#else
    // fistp: round to nearest even
    v[0] = (float)lrintf(v[0]);
    v[1] = (float)lrintf(v[1]);
    v[2] = (float)lrintf(v[2]);
#endif
}