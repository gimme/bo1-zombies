#include "cg_anim_commands.h"
#include "cg_local_mp.h"
#include "cg_ents_mp.h"
#include <qcommon/msg_anim_commands.h>
#include <qcommon/msg.h>
#include <qcommon/dobj_management.h>
#include <xanim/xanim.h>
#include <xanim/xanim_clientnotify.h>
#include <ragdoll/ragdoll.h>
#include <algorithm>
#include <game_sp/g_sp_measure.h>
#include <game_mp/g_main_mp.h>
#include <game/actor_scripted.h>
#include <game_sp/actor_sp_ext.h>
#include <universal/com_math.h>

namespace
{
    // Keep MP structure layouts intact. These are the SP additions to clSnapshot_t,
    // clientActive_t and snapshot_s (SP 0x008851F0, 0x005D7A30), indexed alongside KB's snapshots.
    struct ParsedCommands
    {
        int messageNum;
        unsigned int first;
        int count;
    };
    struct SnapshotCommands
    {
        const snapshot_s *snapshot;
        int count;
        AnimCommand commands[1024];
        bool done[1024];
    };
    struct ClientCommands
    {
        unsigned int next;
        AnimCommand ring[4096];
        ParsedCommands parsed[32];
        SnapshotCommands snapshots[2];
        int lastTime;
        int readCount;
        int replayCount;
        int lostCount; // zombies: commands a snapshot took with it, not replayed (CountLost)
        int lastLogTime;
        // zombies: SP cent+0x308, by entity number (KB's centity_s has no room): the client time an actor or corpse
        // is not drawn before (CG_SP_AnimCommandsHeld); 0 for none.
        int heldUntil[MAX_CL_ENTNUM];
    };
    ClientCommands clients[MAX_LOCAL_CLIENTS];
    const dvar_s *cg_defensive_anim_delay;
    const dvar_s *bo1_animskipped; // mod: CG_SP_ReplayAnimCommandsLeft

    SnapshotCommands *FindCommands(int localClientNum, const snapshot_s *snapshot)
    {
        if (!snapshot)
            return nullptr;
        for (SnapshotCommands &commands : clients[localClientNum].snapshots)
            if (commands.snapshot == snapshot)
                return &commands;
        return nullptr;
    }

    // zombies: advance a newly commanded anim (SP 0x00584000). Only a node at time zero
    // is advanced. The exe clamps at .999 even for looping animations and delivers notetracks.
    void CG_AdvanceAnimCommand(DObj *obj, unsigned int anim, int elapsed)
    {
        XAnimTree_s *tree = DObjGetTree(obj);
        if (!tree->children)
            return;
        unsigned int index = XAnimGetInfoIndex(tree, anim);
        if (!index)
            return;
        XAnimInfo *info = GetAnimInfo(index);
        if (info->state.currentAnimTime != 0.0f)
            return;
        float time = (float)(elapsed * 0.001f * XAnimGetAverageRateFrequency(tree, index) * info->state.rate);
        time = (std::max)(0.0f, (std::min)(time, 0.999f));
        info->state.currentAnimTime = time;
        if (time > 0.0f && info->state.goalWeight != 0.0f)
            XAnimProcessClientNotify(info, elapsed * 0.001f);
        info->state.oldTime = time;
    }

    // zombies: command dispatch (SP 0x00525AB0). Types 7/8 have wire encodings but
    // this exe's dispatch handles ONLY 1..6; keep that distinction.
    void CG_ApplyAnimCommand(int localClientNum, DObj *obj, XAnimTree_s *tree, const AnimCommand &command)
    {
        if (ai_useServerAnims->current.enabled || ai_useServerAnimsEntity->current.integer == command.entnum)
            return;
        cg_s *cg = CG_GetLocalClientGlobals(localClientNum);
        int elapsed = (std::max)(0, cg->time - command.startTime);
        centity_s *cent = CG_GetEntity(localClientNum, command.entnum);
        // KB stores a RagdollBody pointer in the handle; SP 0x004A98D0 resolves an index. KB (not SP's test): a command
        // of the server frame that started the ragdoll still applies. The client starts a ragdoll on the launch event as
        // the snapshot arrives (CG_AddPacketEntities, ahead of cg->time), while cg_defensive_anim_delay holds that
        // frame's commands until cg->time reaches it, tens of ms later: the ragdoll runs two frames after it starts.
        // LaunchRagdoll and the death animscript's anim share that frame (the Thundergun's fling): without
        // them the corpse's tree kept the walk or run it died in, looping for as long as the corpse lay there, its
        // sndnt# footsteps sounding and the bones the ragdoll doesn't drive walking. SP races the same way: it handles
        // the next snapshot's events as it arrives (CG_AddPacketEntities 0x005E0CCF) and its clock trails it by a
        // snapshot interval (CL_AdjustTimeDelta 0x0087F570), as KB's do.
        const RagdollBody *ragdoll = cent->pose.isRagdoll && cent->pose.ragdollHandle > 0
            ? (const RagdollBody *)cent->pose.ragdollHandle : nullptr;
        if (ragdoll && ragdoll->state >= BS_RUNNING && command.startTime > ragdoll->spStartServerTime)
            return;
        XAnimClientNotifyList notifies;
        DObjSetClientNotifies(&notifies);
        unsigned int notifyType = command.weight > 0.001f ? 2 : 0;
        int restart = (command.flags >> 1) & 1;
        // zombies: jump table SP 0x00525E20. Type 1: flag 1 -> XAnimClearTree 0x0041F500 (the node and its
        // subtree, KB XAnimClearTreeGoalWeights), else 0x0050B0C0 (the node only, KB XAnimClearGoalWeight).
        // Type 2 (entry 0x00525BB4) -> 0x00665890, the root's children (KB XAnimClearTreeGoalWeightsStrict),
        // as the server's animscripted clear (actor_scripted.cpp). KB had types 1-else and 2 swapped.
        switch (command.type)
        {
        case 1:
            if (command.flags & 1)
                XAnimClearTreeGoalWeights(tree, command.anim, command.time, -1);
            else
                XAnimClearGoalWeight(tree, command.anim, command.time, (unsigned short)-1);
            break;
        case 2:
            XAnimClearTreeGoalWeightsStrict(tree, command.anim, command.time, -1);
            break;
        case 3:
            if (command.flags & 1)
                XAnimSetCompleteGoalWeight(obj, command.anim, command.weight, command.time, command.rate, 0, notifyType, restart, -1);
            else
                XAnimSetGoalWeight(obj, command.anim, command.weight, command.time, command.rate, 0, notifyType, restart, -1);
            CG_AdvanceAnimCommand(obj, command.anim, elapsed);
            break;
        case 4:
            if (command.flags & 1)
                XAnimSetCompleteGoalWeightKnob(obj, command.anim, command.weight, command.time, command.rate, 0, notifyType, restart, -1);
            else
                XAnimSetGoalWeightKnob(obj, command.anim, command.weight, command.time, command.rate, 0, notifyType, restart, -1);
            CG_AdvanceAnimCommand(obj, command.anim, elapsed);
            break;
        case 5:
            if (command.flags & 1)
                XAnimSetCompleteGoalWeightKnobAll(obj, command.anim, command.root, command.weight, command.time, command.rate, 0, notifyType, restart, -1);
            else
                XAnimSetGoalWeightKnobAll(obj, command.anim, command.root, command.weight, command.time, command.rate, 0, notifyType, restart, -1);
            CG_AdvanceAnimCommand(obj, command.anim, elapsed);
            break;
        case 6:
            XAnimSetTime(tree, command.anim, command.time, (unsigned short)-1);
            break;
        }
        // SP suppresses these during client save restoration (cg+0x18, set at
        // 0x0054C5B4). KB's live MP snapshot path has no SP client-save restoration.
        CG_ProcessFakeEntClientNoteTracks(localClientNum, command.entnum);
        DObjClearClientNotifies();
        ++clients[localClientNum].replayCount;
    }

    // zombies: ordered replay (SP 0x00897660); entity reset replay is SP 0x00648830. leaving: the client leaves the
    // snapshot (CG_SP_ReplayAnimCommandsLeft), and what is left of its commands up to its own time is due whatever
    // the clock says: those no later snapshot need carry again. One that starts after it (the server dates a command
    // stored once a snapshot of its frame was collected a frame on: G_StoreAnimCommand) comes again with the next
    // snapshot, among that one's own of the same time, and is left to it: replayed from here, the watermark, moved
    // past it after this list, would have the rest of them taken for replayed.
    int Replay(int localClientNum, const snapshot_s *snapshot, int entnum, bool leaving = false)
    {
        SnapshotCommands *batch = FindCommands(localClientNum, snapshot);
        if (!batch)
            return 0;
        ClientCommands &client = clients[localClientNum];
        cg_s *cg = CG_GetLocalClientGlobals(localClientNum);
        int applied = 0;
        for (int i = 0; i < batch->count; ++i)
        {
            const AnimCommand &command = batch->commands[i];
            if (entnum >= 0 ? command.entnum != entnum : batch->done[i])
                continue;
            // SP 0x008976F4: a command still to come lowers its entity's hold to its start time (never from 0)
            int &held = client.heldUntil[command.entnum];
            if (entnum < 0 && command.startTime < held)
                held = command.startTime;
            if (leaving ? command.startTime > snapshot->serverTime
                        : cg_defensive_anim_delay->current.enabled && command.startTime > cg->time)
                continue;
            if (entnum < 0 && command.startTime < client.lastTime)
            {
                batch->done[i] = true;
                continue;
            }
            DObj *obj = Com_GetClientDObj(command.entnum, localClientNum);
            if (!obj)
                continue;
            XAnimTree_s *tree = DObjGetTree(obj);
            if (!tree || command.anim >= tree->anims->size)
                continue;
            CG_ApplyAnimCommand(localClientNum, obj, tree, command);
            batch->done[i] = true;
            if (entnum < 0)
                client.lastTime = command.startTime;
            ++applied;
        }
        return applied;
    }

    // zombies: SP 0x00648830's result - the start time of the entity's first command in the snapshot, 0 with none
    // (or with no DObj or tree, which SP tests first).
    int FirstCommandTime(int localClientNum, const snapshot_s *snapshot, int entnum)
    {
        const SnapshotCommands *batch = FindCommands(localClientNum, snapshot);
        DObj *obj = Com_GetClientDObj(entnum, localClientNum);
        if (!batch || !obj || !DObjGetTree(obj))
            return 0;
        for (int i = 0; i < batch->count; ++i)
        {
            if (batch->commands[i].entnum == entnum)
                return batch->commands[i].startTime;
        }
        return 0;
    }

    // zombies: the commands a snapshot takes with it as its buffer goes to a later one (CL_SP_GetAnimCommands): those
    // it could have replayed and never did (their entity has a tree with the anim, and they are no older than the
    // last one replayed) and that neither the client's current snapshot nor the one coming in carries again, as the
    // server sends a command with every snapshot until the client has acknowledged one that had it
    // (game_sp/g_anim_commands_sp.cpp: SV_SP_WriteAnimCommands). There should be none: the client replays what is left
    // of a snapshot's commands before it leaves it (CG_SP_ReplayAnimCommandsLeft). Counted for the tests, which read
    // the count with bo1_player (animlost).
    int CountLost(int localClientNum, const SnapshotCommands &gone, const ParsedCommands &coming)
    {
        const ClientCommands &client = clients[localClientNum];
        const SnapshotCommands *current = FindCommands(localClientNum, CG_GetLocalClientGlobals(localClientNum)->snap);
        int lost = 0;
        for (int i = 0; i < gone.count; ++i)
        {
            const AnimCommand &command = gone.commands[i];
            if (gone.done[i] || command.startTime < client.lastTime)
                continue;
            DObj *obj = Com_GetClientDObj(command.entnum, localClientNum);
            const XAnimTree_s *tree = obj ? DObjGetTree(obj) : nullptr;
            if (!tree || command.anim >= tree->anims->size)
                continue;
            bool again = false;
            for (int j = 0; current && current != &gone && j < current->count && !again; ++j)
                again = !memcmp(&current->commands[j], &command, sizeof(command));
            for (int j = 0; j < coming.count && !again; ++j)
                again = !memcmp(&client.ring[(coming.first + j) & 4095], &command, sizeof(command));
            lost += !again;
        }
        return lost;
    }
}

void CL_SP_ResetAnimCommands(int localClientNum)
{
    memset(&clients[localClientNum], 0, sizeof(clients[localClientNum]));
    // zombies: SP 0x004A4E5D, default true, flags 0.
    cg_defensive_anim_delay = _Dvar_RegisterBool("cg_defensive_anim_delay", true, 0, "");
    bo1_animskipped = _Dvar_RegisterBool("bo1_animskipped", true, 0,
        "mod: replay the anim commands of a snapshot the client steps over within a frame (0: retail's, which loses them)");
    // L15 (TEST): client-side scripted anim trace, CG_SP_MeasureAnimScripted.
    _Dvar_RegisterBool("bo1_measure_animscripted_cl", false, 0, "L15 TEST: per-frame client scripted anim weights and placement");
}

// zombies: read snapshot command list into the 4096-command parse ring (SP 0x008851F0).
void CL_SP_ParseAnimCommands(int localClientNum, msg_t *msg, int messageNum, int serverTime)
{
    if (!zombiemode || !zombiemode->current.enabled)
        return;
    ClientCommands &client = clients[localClientNum];
    ParsedCommands &parsed = client.parsed[messageNum & 31];
    parsed = {messageNum, client.next, 0};
    AnimCommandMsgState state;
    AnimCommand command;
    while (MSG_ReadAnimCommand(msg, serverTime, &state, &command))
    {
        if (parsed.count == 1024)
        {
            msg->overflowed = 1;
            return;
        }
        client.ring[client.next++ & 4095] = command;
        ++parsed.count;
        ++client.readCount;
    }
}

// zombies: copy the parsed commands and clear applied flags (SP 0x005D7A30).
bool CL_SP_GetAnimCommands(int localClientNum, int messageNum, snapshot_s *snapshot)
{
    if (!zombiemode || !zombiemode->current.enabled)
        return true;
    ClientCommands &client = clients[localClientNum];
    const ParsedCommands &parsed = client.parsed[messageNum & 31];
    if (parsed.messageNum != messageNum || client.next - parsed.first > 4096)
        return false;
    SnapshotCommands *batch = FindCommands(localClientNum, snapshot);
    if (batch)
        client.lostCount += CountLost(localClientNum, *batch, parsed); // zombies: what the snapshot before takes with it
    if (!batch)
        for (SnapshotCommands &slot : client.snapshots)
            if (!slot.snapshot)
            {
                batch = &slot;
                break;
            }
    iassert(batch);
    batch->snapshot = snapshot;
    batch->count = parsed.count;
    for (int i = 0; i < parsed.count; ++i)
    {
        batch->commands[i] = client.ring[(parsed.first + i) & 4095];
        batch->done[i] = false;
    }
    return true;
}

// zombies: what is left of the current snapshot's commands, replayed as the client leaves it for the next one
// (CG_ProcessSnapshots, before each transition), the watermark moved on after the list as after any
// (CG_SP_ReplayAnimCommands). Not SP's, whose one replay a frame (below) is of the two snapshots the frame ends
// with, as the frame's replay here still is: a frame that goes through more than one transition (one longer than a
// snapshot's 50 ms: every frame below 20 a second, and any hitch) leaves the snapshots between for good, and their
// buffers to the snapshots read next. The commands a snapshot first carries start at its own time, so none of them
// is replayed before the client's clock has reached it: they went unreplayed with it, unless the server sent them
// again, which it does only until the client acknowledges a snapshot that had them, and a client has, of the one it
// is about to step over, by the frame before. A zombie whose command to walk was among them slid along in the pose
// it had, until its next command. bo1_animskipped 0 is SP's.
void CG_SP_ReplayAnimCommandsLeft(int localClientNum)
{
    if (!zombiemode || !zombiemode->current.enabled || !bo1_animskipped->current.enabled)
        return;
    if (Replay(localClientNum, CG_GetLocalClientGlobals(localClientNum)->snap, -1, true))
        ++clients[localClientNum].lastTime;
}

// zombies: the client starts over from this snapshot (CG_SetInitialSnapshot: its first, or the server's restart):
// the commands of the snapshots before it are of a game that is no more, replayed by no one and lost by no one.
void CG_SP_AnimCommandsStartOver(int localClientNum, const snapshot_s *snapshot)
{
    for (SnapshotCommands &batch : clients[localClientNum].snapshots)
        if (batch.snapshot != snapshot)
            batch.count = 0;
}

// zombies: the commands lost since the game began (CountLost), for bo1_player
int CG_SP_AnimCommandsLost(int localClientNum)
{
    return clients[localClientNum].lostCount;
}

// zombies: replay current then next snapshot; advance watermark only after each list
// (SP 0x004DEB90). This prevents equal-time commands being replayed in later snapshots.
void CG_SP_ReplayAnimCommands(int localClientNum)
{
    if (!zombiemode || !zombiemode->current.enabled)
        return;
    cg_s *cg = CG_GetLocalClientGlobals(localClientNum);
    ClientCommands &client = clients[localClientNum];
    if (Replay(localClientNum, cg->snap, -1))
        ++client.lastTime;
    if (Replay(localClientNum, cg->nextSnap, -1))
        ++client.lastTime;
    // L12 (TEST, bo1_measure_animents): the client DObj of every snapshot entity whose server twin has an anim
    // tree (listen server only) and is not a player / actor: does the client animate it, and which anims.
    static int s_nextAnimEntLog;
    if ( G_SP_MeasureAnimEntsOn() && cg->nextSnap && (cg->time >= s_nextAnimEntLog || cg->time < s_nextAnimEntLog - 1000) )
    {
        s_nextAnimEntLog = cg->time + 250;
        for (int num = 0; num < cg->nextSnap->numEntities; ++num)
        {
            const entityState_s *es = &cg->nextSnap->entities[num];
            if (es->eType == ET_PLAYER || es->eType == ET_ACTOR || es->number >= MAX_GENTITIES
                || !g_entities[es->number].r.inuse || !g_entities[es->number].pAnimTree)
                continue;
            DObj *obj = Com_GetClientDObj(es->number, localClientNum);
            const XAnimTree_s *tree = obj ? DObjGetTree(obj) : nullptr;
            char anims[512] = "-";
            if (tree)
                G_SP_FormatWeightedAnims(tree, anims, sizeof(anims));
            centity_s *cent = CG_GetEntity(localClientNum, es->number);
            Com_Printf(14, "bo1_animent_cl: time %d ent %d eType %d dobj %d tree %d centtree %d origin %.0f %.0f %.0f anims %s\n",
                cg->time, es->number, (int)es->eType, obj ? 1 : 0, tree ? 1 : 0, cent->tree ? 1 : 0,
                cent->pose.origin[0], cent->pose.origin[1], cent->pose.origin[2], anims);
        }
    }
    const dvar_s *measure = Dvar_FindVar("bo1_measure");
    if (measure && measure->current.enabled && cg->time - client.lastLogTime >= 10000)
    {
        client.lastLogTime = cg->time;
        Com_Printf(14, "bo1_anim_snapshot: time=%d read=%d replay=%d watermark=%d ai_useServerAnims=%d entity=%d\n",
            cg->time, client.readCount, client.replayCount, client.lastTime,
            ai_useServerAnims->current.enabled, ai_useServerAnimsEntity->current.integer);
    }
}

namespace
{
    // L15 (TEST): the client leaves with an effective weight >= 0.05, "name:weight,...".
    void MeasureEffectiveLeaves(const XAnimTree_s *tree, char *out, int size)
    {
        int used = 0;
        out[0] = 0;
        const XAnim_s *anims = XAnimGetAnims(tree);
        const unsigned int count = XAnimGetAnimTreeSize(anims);
        for (unsigned int anim = 1; anim < count && used < size - 1; ++anim)
        {
            if (XAnimGetNumChildren(anims, anim) || !XAnimGetInfoIndex(tree, anim))
                continue;
            const float weight = G_SP_MeasureEffectiveWeight(tree, anim);
            if (weight < 0.05f)
                continue;
            const int n = _snprintf_s(out + used, size - used, _TRUNCATE, "%s%s:%.2f", used ? "," : "",
                XAnimGetAnimName(anims, anim), weight);
            if (n < 0)
                break;
            used += n;
        }
        if (!used)
            strcpy_s(out, size, "-");
    }
}

// L15 (TEST, bo1_measure_animscripted_cl; listen server): every client frame, for each entity the server thread
// last saw in a scripted anim (its copy of the record, G_SP_MeasureScriptedPlacement; no server pointers are read
// here): the scripted anim's effective weight in the server tree (at its last think) and in the client tree, the
// client's weighted leaves, and the drawn origin / yaw against an SP-style placement (SP 0x00574990 places the entity
// from its client anim: the scripted anim's delta through the scripted axis, SP 0x007d4fb0) plus the server's last
// offset from its own anim placement. Called after the entity's per-type processing (CG_ProcessEntity).
void CG_SP_MeasureAnimScripted(int localClientNum, const centity_s *cent)
{
    static const dvar_s *enabled;
    if (!enabled)
        enabled = Dvar_FindVar("bo1_measure_animscripted_cl");
    if (!enabled || !enabled->current.enabled || !zombiemode || !zombiemode->current.enabled)
        return;
    const int number = cent->nextState.number;
    DObj *clientObj = Com_GetClientDObj(number, localClientNum);
    const XAnimTree_s *clientTree = clientObj ? DObjGetTree(clientObj) : nullptr;
    if (!clientTree)
        return;
    const cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    float ideal[3], idealYaw = 0.0f, serverWeight;
    unsigned int anim;
    int startTime;
    bool placed;
    if (!G_SP_MeasureScriptedPlacement(number, cgameGlob->time, clientObj, ideal, &idealYaw, &anim, &startTime,
            &serverWeight, &placed))
        return;
    char leaves[256];
    MeasureEffectiveLeaves(clientTree, leaves, sizeof(leaves));
    const float dpos = placed ? Vec3Distance(ideal, cent->pose.origin) : -1.0f;
    const float dyaw = placed ? (float)AngleDelta(cent->pose.angles[1], idealYaw) : 0.0f;
    Com_Printf(14, "bo1_asc: t %d ent %d eType %d anim %s since %d svw %.3f clw %.3f cltime %.3f dpos %.2f dyaw %.2f org %.1f %.1f %.1f leaves %s\n",
        cgameGlob->time, number, (int)cent->nextState.eType, XAnimGetAnimName(XAnimGetAnims(clientTree), anim),
        cgameGlob->time - startTime, serverWeight, G_SP_MeasureEffectiveWeight(clientTree, anim),
        (float)XAnimGetTime(clientTree, anim), dpos, dyaw,
        cent->pose.origin[0], cent->pose.origin[1], cent->pose.origin[2], leaves);
}

// zombies: rebuild an actor tree from both buffered snapshots (SP 0x004B8D80,
// called by CG_ResetActorEntity 0x0050E3D0 after updating its DObj).
void CG_SP_ReplayEntityAnimCommands(int localClientNum, int entnum)
{
    if (!zombiemode || !zombiemode->current.enabled)
        return;
    cg_s *cg = CG_GetLocalClientGlobals(localClientNum);
    const int snapTime = FirstCommandTime(localClientNum, cg->snap, entnum);
    Replay(localClientNum, cg->snap, entnum);
    const int nextTime = FirstCommandTime(localClientNum, cg->nextSnap, entnum);
    Replay(localClientNum, cg->nextSnap, entnum);
    // SP 0x004B8DFD: the entity is held until its earliest buffered command, the current snapshot's unless the next
    // one's is earlier (or the current one has none); not held at all when only the current one has any
    clients[localClientNum].heldUntil[entnum] = nextTime < snapTime || !snapTime ? nextTime : snapTime;
}

// zombies: SP CG_ResetEntity (0x00897B52) clears the hold before the new instance's tree is rebuilt.
void CG_SP_ClearAnimCommandHold(int localClientNum, int entnum)
{
    clients[localClientNum].heldUntil[entnum] = 0;
}

// zombies: SP CG_Actor (0x005813C3): an actor or corpse whose tree was rebuilt from buffered commands is not drawn
// (nor anything else of it done that frame) until cg->time reaches the first of them, so it doesn't show without its
// animation (in its base pose) first.
bool CG_SP_AnimCommandsHeld(int localClientNum, int entnum)
{
    const int held = clients[localClientNum].heldUntil[entnum];
    return held && held > CG_GetLocalClientGlobals(localClientNum)->time;
}
