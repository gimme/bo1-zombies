#pragma once
#include "bo1_web_prelude.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
struct _finddata_t
{
    unsigned attrib;
    long long time_create;
    long long time_access;
    long long time_write;
    unsigned long size;
    char name[260];
};
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
BO1_EXTERN_C intptr_t _findfirst(const char *filespec, struct _finddata_t *fileinfo);
BO1_EXTERN_C int _findnext(intptr_t handle, struct _finddata_t *fileinfo);
BO1_EXTERN_C int _findclose(intptr_t handle);
struct _finddata64i32_t
{
    unsigned attrib;
    long long time_create;
    long long time_access;
    long long time_write;
    unsigned long size;
    char name[260];
};
BO1_EXTERN_C intptr_t _findfirst64i32(const char *filespec, struct _finddata64i32_t *fileinfo);
BO1_EXTERN_C int _findnext64i32(intptr_t handle, struct _finddata64i32_t *fileinfo);
