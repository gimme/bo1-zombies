#pragma once

struct dvar_s;

// zombies: what a map's game reads of the install outside its fastfiles, listed for the server (zombies-hub's
// server/files.go, +set bo1_files 1): sv_files_mp.cpp
extern const dvar_s *bo1_files;

void SV_Files_RegisterDvars();
bool SV_Files_Listing();
void SV_Files_BeforeLevel();
void SV_Files_LevelLoaded();
