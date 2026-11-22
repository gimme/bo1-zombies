#pragma once

struct cmd_function_s;
struct Material;

void __cdecl SCR_DrawCinematic();
void __cdecl SCR_StopCinematic();

// zombies: the maps' loading movies and the client's fullscreen cinematics (cl_cin.cpp)
void CL_LoadingMovieStart(const char *mapname);
bool CL_LoadingMovieShows();
bool CL_LoadingMovieOn();
bool CL_LoadingMovieHolds(int localClientNum);
bool CL_LoadingMovieSkip(int localClientNum);
bool CL_LoadingMovieOverGame(int localClientNum, int pmType);
bool CL_LoadingMovieCoversGame();
bool CL_DrawLoadingMovie(int localClientNum);
bool CL_DrawLoadingMoviePicture(Material *material);
bool CL_LoadingMovieHidesCursor(int localClientNum);
void CL_LoadingMovieStop();
void CL_PlayCinematic(const char *name, unsigned int playbackFlags);
bool CL_PlayOutro(const char *why, const char *text);
void __cdecl CL_PlayCinematic_f();
extern cmd_function_s CL_PlayCinematic_f_VAR;
