#pragma once

// zombies: the gamepad's rumble (cg_rumble.cpp)
void CG_PlayRumble(int localClientNum, const char *name, const float *origin);
void CG_RumbleShake(int localClientNum, float size);
void CG_UpdateRumble(int localClientNum);
