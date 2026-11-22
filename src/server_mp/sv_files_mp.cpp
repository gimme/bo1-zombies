// zombies: the files of the install a map's game reads outside its fastfiles, which zombies-hub's docker/list-files.sh
// asks a dedicated server for once per map, into zombies-hub's server/reads/ (zombies-hub's server/files.go:
// zombies-server list-files), run with +set bo1_files 1 +map <map>. The web client reads the install from the browser's
// store alone, which downloads a map's files ahead with the list, and keeps them. As the map has loaded, the server
// says on its console each image whose pixels come from an .iwi file in the archives (a GfxImage of the load-from-file
// category, which the renderer reads once the zone has loaded: r_image.cpp, R_DelayLoadImage) and each sound streamed
// from a file (an alias whose SoundFile is streamed or primed: snd_db.cpp, SND_AliasGetFileName), then quits:
//
//   bo1_files: images/<name>.iwi
//   bo1_files: sound/<path>.wav
//   bo1_files: end
//
// The fastfiles it says as it loads them ("Loaded fastfile '<name>'"), as any game does. Every image and sound of every
// zone loaded is listed, the code zones' too: the list is what the client may read, not what one game happened to. A
// dedicated server loads the zones a client does but ui_mp, whose images draw the game's menus, and which the level's
// zones unload as they load: listing, it loads that one too, before the level, and lists then as well as after.
#include "sv_files_mp.h"

#include <database/database.h>
#include <database/db_registry.h>
#include <gfx_d3d/r_material.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <sound/snd.h>
#include <sound/snd_bank.h>
#include <sound/snd_db.h>
#include <universal/com_memory.h>
#include <universal/dvar.h>

const dvar_s *bo1_files;

void SV_Files_RegisterDvars()
{
    bo1_files = _Dvar_RegisterBool("bo1_files", false, 0,
        "List what the map reads of the install outside its fastfiles once it has loaded, and quit");
}

bool SV_Files_Listing()
{
    return bo1_files && bo1_files->current.enabled;
}

static void SV_Files_Image(XAssetHeader header, void *)
{
    const GfxImage *image = header.image;
    if (image->category == 3) // IMG_CATEGORY_LOAD_FROM_FILE: its pixels are in images/<name>.iwi (r_image_load_obj.cpp)
        Com_Printf(15, "bo1_files: images/%s.iwi\n", image->name);
}

static void SV_Files_Sound(XAssetHeader header, void *)
{
    const SndBank *bank = header.sound;
    for (unsigned int i = 0; i < bank->aliasCount; ++i)
    {
        const snd_alias_list_t &list = bank->alias[i];
        for (int j = 0; j < list.count; ++j)
        {
            const snd_alias_t &alias = list.head[j];
            if (!alias.soundFile || alias.soundFile->type < 2) // unknown, or loaded: in the fastfile (snd_db.cpp)
                continue;
            char filename[256];
            SND_AliasGetFileName(&alias, filename, sizeof(filename));
            if (filename[0])
                Com_Printf(15, "bo1_files: %s\n", filename);
        }
    }
}

static void SV_Files_List()
{
    DB_SyncXAssets();
    DB_EnumXAssets(ASSET_TYPE_IMAGE, SV_Files_Image, 0, true);
    DB_EnumXAssets(ASSET_TYPE_SOUND, SV_Files_Sound, 0, true);
}

// The code zones are loaded, and the level's are about to be (SV_SpawnServer): ui_mp, as a client has it loaded by
// then (Com_LoadUiFastFile), and what is loaded so far.
void SV_Files_BeforeLevel()
{
    if (!SV_Files_Listing())
        return;
    XZoneInfo ui = { "ui_mp", 0x4000000, 0 };
    DB_LoadXAssets(&ui, 1, 0);
    SV_Files_List();
}

// The level's zones are loaded (SV_SpawnServer, Com_LoadLevelFastFiles): the rest of the list, then quit.
void SV_Files_LevelLoaded()
{
    if (!SV_Files_Listing())
        return;
    SV_Files_List();
    Com_Printf(15, "bo1_files: end\n");
    Cbuf_AddText(0, "quit\n");
}
