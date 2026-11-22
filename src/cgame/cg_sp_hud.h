#pragma once
// zombies: SP client HUD pieces (cg_sp_hud.cpp). Called only in zombiemode, except the dvar registration.
struct rectDef_s;

void CG_SP_RegisterScoreBarDvars();
void CG_SP_SetupScoreboardDvars();
bool CG_SP_OwnerDraw_MiniScoreboard(int localClientNum, const rectDef_s *rect);
int CG_SP_DrawScoreboard(int localClientNum);
bool CG_SP_ScoreboardShows(int localClientNum);
void CG_SP_DrawIntermission(int localClientNum);
extern const struct dvar_s *cg_defaultFadeScreenColor; // SP 0x02FF67E8
void CG_SP_ScreenFadeIn(int localClientNum, int delayMs, int fadeMs); // SP 0x00403030
void CG_SP_DrawScreenFade(int localClientNum); // SP 0x00771D20
void CG_SP_ScreenFade(int localClientNum, int r, int g, int b, int a, int startTime, int duration, bool hold); // SP 0x0041EF70
void CG_SP_Fade_f(); // SP 0x00770AE0, console command "fade"
void CG_SP_Silence_f(); // SP 0x00770CA0, console command "silence"
void CG_SP_UpdateLevelSilence(int localClientNum); // the level silent while the player waits for the others
void CG_SP_RefreshAllHudFades(int localClientNum); // SP 0x004E31F0
// The co-op HUD's friendly arrows (ui/hud_coop.txt: ownerdraw 199; SP CG_OwnerDrawHud 0x005ACCB0 -> 0x00632F20): each
// teammate drawn in the last 800 ms gets a pointer at the screen's edge while off it, white, yellow while hurt
// (cg_timedDamageDuration), orange to red over the bleed-out while downed (player_lastStandBleedoutTime). A downed
// teammate, or one being revived, also gets the revive icon over them, held at the edge while off screen.
void CG_SP_DrawFriendlyArrows(int localClientNum);
void CG_SP_PlayerDrawn(int localClientNum, int clientNum); // SP 0x005AA370, from CG_Player
void CG_SP_RegisterFriendlyArrows(); // SP's waypoint_revive (SP 0x0088ECD4)
