#include "r_devgui.h"
#include <qcommon/cmd.h>

void __cdecl R_CreateDevGui()
{
    if ( Cmd_ConfigFound("devgui_visibility") ) // zombies: the dev builds' config, which installs don't have
        Cbuf_InsertText(0, "exec devgui_visibility");
}

