#pragma once

static const int CHANGELIST_NUMBER = 794515;
static const int BUILD_NUMBER = 61;
static const char *BUILD_MACHINE = "CODPCAB-V64";
static const char *BUILD_TIME = "Fri Nov 05 11:33:52 2010";

char *__cdecl Com_GetBuildVersion();
int __cdecl Com_GetBuildNumber();
const char *__cdecl Com_GetBuildDisplayName();
const char *__cdecl Com_GetBuildOfficialNameR();
const char *__cdecl Com_GetBuildDisplayNameR();
const char *__cdecl Com_GetBuildName();
int __cdecl Com_GetChangelist();
const char *Com_GetBuildId(); // zombies: this build's ID (build_id.cmake)
struct dvar_s;
extern const dvar_s *bo1_build; // zombies: the build a client or server says it is (common.cpp)
