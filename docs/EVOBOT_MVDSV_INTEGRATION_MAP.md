# EvoBot / MVDSV integration map

## Scope and provenance

This document describes the current trees at:

- MVDSV: `K:\development\EvoBot\mvdsv`, commit `03d4824037ef4e5b71427e03737ed4f8c34e4ce5`
- KTX reference: `K:\development\EvoBot\ktx-reference`, commit `631584f7fac3c3891826224d36d873c99880d353`

Line numbers below are current at those commits. No implementation is implied by this document.

The intended boundary is strict: portable code under `src/evobot/` owns all structures visible to EvoBot and includes no MVDSV headers. Only `src/evobot_qw_adapter.c` may include MVDSV headers or touch MVDSV globals, edicts, clients, collision models, VM state, or `pmove` state.

## Executive summary

MVDSV already contains most of the engine-side mechanisms EvoBot needs:

- Engine lifecycle is centralized in `SV_Init`, `SV_SpawnServer`, `SV_Frame`, and `SV_Shutdown`.
- Human and existing fake clients converge on `SV_RunCmd`, `PM_PlayerMove`, and the same game-code pre/post-think callbacks.
- PR2 exposes existing fake-client syscalls: `G_Add_Bot`, `G_Remove_Bot`, `G_SetBotUserInfo`, and `G_SetBotCMD`.
- KTX/Frogbot creates bots through those syscalls and writes a `usercmd_t` into `client_t.botcmd`; `SV_RunBots` consumes that command and enters the normal movement path.
- World trace, point-contents, PVS, entity, player-edict, and client-state access already exist inside the server.

The smallest practical EvoBot integration is therefore a portable core plus one MVDSV adapter. The adapter should expose plain EvoBot-owned snapshots and requests, reuse or narrowly extract the existing fake-client allocation/removal code, translate EvoBot commands to `usercmd_t`, and drive actual movement through `SV_RunCmd`. It should not give portable code direct pointers to `client_t`, `edict_t`, `entvars_t`, `trace_t`, `cmodel_t`, `pmove`, or PR globals.

## Responsibility index

| Responsibility | Current source and function | Notes |
|---|---|---|
| Windows process entry and frame pump | `src/sv_sys_win.c:770` `main`; `src/sv_sys_win.c:823` `WinMain`; calls at `src/sv_sys_win.c:781`, `:815`, `:857`, `:911` | Both Windows entry variants call `Host_Init`, then repeatedly call `SV_Frame` with elapsed wall time. |
| Server initialization | `src/sv_main.c:3951` `Host_Init`; `src/sv_main.c:4044` `SV_Init` | `Host_Init` initializes filesystem/network/collision and calls `SV_Init` at `src/sv_main.c:3977`. `SV_Init` initializes progs, server locals, MVD, and login. |
| Server shutdown | `src/sv_ccmds.c:49` `SV_Quit`; `src/sv_main.c:235` `SV_Shutdown` | `SV_Shutdown` sends the final message, shuts down game code, unloads progs/VM, clears `sv`, and clears all client slots. Error shutdown also reaches it from `src/sv_sys_win.c:422`. |
| Map command staging and activation | `src/sv_ccmds.c:406` `SV_Map` | `SV_Map(false)` validates/stages the requested map at lines 471-497. A later `SV_Map(true)` calls `SV_SpawnServer` at line 458. `SV_Frame` invokes the activation phase at `src/sv_main.c:3327`. |
| Per-map teardown, loading, and activation | `src/sv_init.c:246` `SV_SpawnServer` | Saves transition parms, removes current PR2 bots, shuts down/unloads game code, clears memory, reloads progs and BSP, spawns entities, sets `ss_active`, and runs two settling physics frames. |
| Collision-map invalidation and server area tree reset | `src/sv_main.c:3920` `Host_ClearMemory`; `src/cmodel.c:1259` `CM_InvalidateMap`; `src/sv_world.c:203` `SV_ClearWorld` | `Host_ClearMemory` invalidates collision pointers before freeing hunk allocations. `SV_ClearWorld` creates a fresh world area tree after the new BSP is loaded. |
| Main server frame | `src/sv_main.c:3284` `SV_Frame` | Map activation, packet receipt, normal physics, bot physics, outgoing messages, demos, and heartbeat. |
| Finding a free human client slot | `src/sv_main.c:1157` `CountPlayersSpecsVips`; `src/sv_main.c:1237` `SVC_DirectConnect` | The first `cs_free` slot is returned at lines 1163-1169; `SVC_DirectConnect` initializes it at lines 1374-1433. |
| Freeing a human client slot | `src/sv_main.c:377` `SV_DropClient`; `src/sv_main.c:3079` `SV_CheckTimeouts` | A normal drop becomes `cs_zombie` at line 427. Timeout processing changes it to `cs_free` at lines 3111-3113. Reconnect may free it immediately at lines 1129-1147. |
| Allocating/freeing a current fake-client slot | `src/pr2_cmds.c:2144` `PF2_Add_Bot`; `src/pr2_cmds.c:2274` `RemoveBot`; `src/pr2_cmds.c:2296` `PF2_Remove_Bot` | `PF2_Add_Bot` finds a `cs_free` slot at lines 2181-2187 and makes it `cs_spawned`/`isBot` at lines 2210-2217. `RemoveBot` immediately makes it `cs_free` at line 2287. |
| Human sign-on and spawn | `src/sv_user.c:203` `Cmd_New_f`; `src/sv_user.c:672` `Cmd_PreSpawn_f`; `src/sv_user.c:824` `Cmd_Spawn_f`; `src/sv_user.c:960` `Cmd_Begin_f` | `Cmd_Begin_f` changes the slot to `cs_spawned` and invokes connect/put-in-server callbacks at lines 977-997. |
| Player/spectator transition | `src/sv_user.c:2669` `Cmd_Join_f`; `src/sv_user.c:2761` `Cmd_Observe_f` | These perform disconnect/new-parms/connect/put-in-server transitions without reallocating the client slot. |
| Game-controlled player respawn | Engine callback dispatch in `src/pr_exec.c:782` `PR1_GamePutClientInServer` and `src/pr2_exec.c:248` `PR2_GamePutClientInServer` | MVDSV does not own normal death-respawn policy. The loaded game code does. In KTX, `src/client.c:910` `k_respawn` calls `PutClientInServer` at line 931. |
| Client disconnect callback | `src/sv_main.c:377` `SV_DropClient` | For spawned humans, sets QC/VM `self` and calls `PR_GameClientDisconnect` at lines 400-405. Fake clients are redirected to `RemoveBot` at lines 390-395. |
| Network packet receive | `src/sv_main.c:2968` `SV_ReadPackets` | `NET_GetPacket` is called at line 2995; matched client packets reach `SV_ExecuteClientMessage` at line 3061. |
| User-command decode | `src/sv_user.c:4478` `SV_ExecuteClientMessage`; `src/common.c:474` `MSG_ReadDeltaUsercmd` | `clc_move` begins at `src/sv_user.c:4686`; three commands are decoded at lines 4710-4716. The wire/POD layout is `src/qwprot/src/protocol.h:500` `usercmd_t`. |
| User-command storage | `src/server.h:213` `client_t.lastcmd`; `src/server.h:223` `client_t.botcmd` | Human `newcmd` becomes `lastcmd` at `src/sv_user.c:4766`. PR2 bot submission writes `botcmd` at `src/pr2_cmds.c:2388-2396`; `SV_RunBots` copies it to `lastcmd` and clears it at `src/sv_phys.c:1097-1100`. |
| User-command execution | `src/sv_user.c:4261` `SV_ExecuteClientMove`; `src/sv_user.c:3635` `SV_RunCmd`; `src/sv_user.c:4168` `SV_PostRunCmd` | Packet-loss replay and current command execution converge on `SV_RunCmd`; post-think is called once after the command group. |
| Normal QuakeWorld player physics | `src/sv_user.c:3635` `SV_RunCmd`; `src/pmove.c:886` `PM_PlayerMove` | `SV_RunCmd` copies client/edict state to global `pmove` at lines 3775-3810, calls `PM_PlayerMove` at line 3813, then commits results and touches at lines 3842-3879. |
| World/entity trace | `src/sv_world.c:729` `SV_Trace`; `src/sv_world.c:481` `SV_ClipMoveToEntity`; `src/cmodel.c:341` `CM_HullTrace` | `SV_Trace` clips against world first, then linked solid entities, including optional antilag behavior. |
| Point contents | `src/sv_world.c:442` `SV_PointContents`; `src/cmodel.c:178` `CM_HullPointContents` | `SV_PointContents` queries hull 0 of `sv.worldmodel`. |
| PVS query primitives | `src/cmodel.c:397` `CM_PointInLeaf`; `src/cmodel.c:421` `CM_LeafPVS`; `src/cmodel.c:502` `CM_FatPVS`; `src/cmodel.c:382` `CM_Leafnum` | `CM_FatPVS` includes all leaf PVS sets within eight units of a point. Returned storage is engine-owned/static and must not cross the EvoBot boundary. |
| Entity PVS testing | `src/sv_ents.c:812` `SV_EntityVisibleToClient`; `src/pr2_cmds.c:2527` `PF2_VisibleTo` | Both test an edict's cached `leafnums` against a PVS bitset. `PF2_VisibleTo` is the clearest bulk precedent. |
| Entity iteration | `src/sv_phys.c:986-999`; `src/sv_ents.c:954-956`; `src/pr2_cmds.c:2541-2553` | Iterate integer IDs `[0, sv.num_edicts)`, obtain `EDICT_NUM(id)`, and skip `e.free`. Use `NEXT_EDICT` only inside the adapter. |
| Player edict/client access | `src/server.h:190` `client_t`; `src/server.h:220` `client_t.edict`; `src/sv_init.c:478-489` | Client slot `i` is reserved edict `i + 1`. `client_t.edict` points at that edict. `svs.clients` is the slot array; `sv.edicts`/`sv.num_edicts` are the entity table/count. |
| PR1 program loading | `src/pr_edict.c:1138` `PR1_LoadProgs` | Loads the selected `*.dat`, validates version/CRC, maps function/string/definition/global lumps, calculates `pr_edict_size`, and initializes builtins. |
| PR1 fields and globals | `src/progdefs.h:26` `globalvars_t`; `src/progdefs.h:82` `entvars_t`; `src/progs.h:85-95`; `src/progs.h:109-120`; `src/progs.h:149-170` | `pr_global_struct`/`pr_globals` expose globals. `edict_t.v` exposes generated common fields. `ED_FindGlobal`, `ED_FindField`, and field-offset/value helpers handle named access. |
| PR1 execution | `src/pr_exec.c:359` `PR_ExecuteProgram`; wrappers at `src/pr_exec.c:752-819` | Directly interprets PR1 statements and calls exported QC function offsets such as `ClientConnect`, `PlayerPreThink`, and `PlayerPostThink`. |
| PR2/native/QVM loading and execution | `src/pr2_exec.c:425` `PR2_LoadProgs`; `src/vm.c:1235` `VM_Create`; `src/vm.c:1409` `VM_Call`; `src/pr2_exec.c:571` `PR2_InitProg` | Attempts the selected VM type, falls back to PR1 if no VM loads, imports `gameData_t`, and uses numbered `GAME_*` calls and `G_*` syscalls. |
| Existing bot frame | `src/sv_phys.c:1026` `SV_RunBots` | Calls a bot-specific game-code start frame and then runs every `client_t.isBot` through the normal command path. |
| Existing bot syscalls | `src/g_public.h:157-160`; dispatcher `src/pr2_cmds.c:2804-2813` | Add, remove, userinfo update, and command submission. There is no MVDSV `Cmd_AddCommand` console command named `addbot`; creation is exposed to PR2 game code as syscalls. |
| Existing bot visibility support | `src/sv_ents.c:1081` `SV_SetVisibleEntitiesForBot`; call at `src/sv_send.c:1159-1160` | Writes the mod's optional `visclients` field for bots before game-code observation. |

## Server and map lifecycle

### Process/server initialization

The Windows entry point calls `Host_Init` (`src/sv_sys_win.c:781` for the console build, `src/sv_sys_win.c:857` for the GUI build). `Host_Init` initializes the filesystem, network, system layer, and collision model at `src/sv_main.c:3969-3976`, then calls `SV_Init` at line 3977.

`SV_Init` (`src/sv_main.c:4044`) is the correct one-time EvoBot initialization neighborhood. It calls `PR_Init` at line 4049, then initializes server-local commands/state, MVD, and login at lines 4054-4057. A future adapter hook should be once-only and should fail cleanly without leaving partially created fake clients.

### Map request, clear, load, and activation

Map changes are deliberately two-phase:

1. `SV_Map(false)` records and validates a requested level (`src/sv_ccmds.c:471-497`).
2. The next `SV_Frame` calls `SV_Map(true)` (`src/sv_main.c:3327`).
3. `SV_Map(true)` invokes `SV_SpawnServer` (`src/sv_ccmds.c:415-458`).

`SV_SpawnServer` (`src/sv_init.c:246`) performs this order:

1. `SV_SaveSpawnparms` preserves human transition state (`src/sv_init.c:271`, implementation at `:156-179`). Spawned clients become `cs_connected` so they sign on to the new level.
2. Current PR2 bots are immediately freed at `src/sv_init.c:274-290`.
3. Current game code receives shutdown and is unloaded at `src/sv_init.c:292-294`.
4. Server state becomes dead and `Host_ClearMemory` runs at `src/sv_init.c:296-305`. `Host_ClearMemory` calls `CM_InvalidateMap` before releasing hunk memory (`src/sv_main.c:3920-3928`); `CM_InvalidateMap` nulls collision/PVS/entity-string pointers (`src/cmodel.c:1259-1274`).
5. The complete per-level `sv` structure is cleared at `src/sv_init.c:358-361`.
6. Game code is loaded and edict storage is initialized at `src/sv_init.c:383-398`.
7. Client edicts 1 through `MAX_CLIENTS` are reserved and rebound to slots at `src/sv_init.c:478-489`.
8. The BSP is loaded by `CM_LoadMap` at `src/sv_init.c:499` (with old-map fallback at lines 501-513).
9. `SV_ClearWorld` builds the new area tree at `src/sv_init.c:527`; its implementation is `src/sv_world.c:203-208`.
10. World/global map data is installed at `src/sv_init.c:565-600`.
11. Entity text is obtained and passed to `PR_LoadEnts` at `src/sv_init.c:602-635`.
12. The map becomes active at `src/sv_init.c:643`, then two settling `SV_Physics` calls run at lines 645-650. Baselines and serverinfo are completed at lines 652-659.

The safest future “map loaded” notification point is immediately after line 659, when entity spawning and the two settling frames are complete. The adapter should increment an EvoBot-owned map generation and invalidate all old entity/client handles before publishing the new snapshot.

**Runtime verification required:** confirm whether EvoBot bots should persist conceptually across a map change or be removed and recreated. Current PR2 bots are removed, not carried, by `src/sv_init.c:274-290`. The smallest first implementation should match that behavior.

### Main frame and shutdown

`SV_Frame` begins at `src/sv_main.c:3284`. Its important order is:

1. advance `realtime` and `sv.time` (`:3295-3300`);
2. process commands and pending map change (`:3318-3329`);
3. receive client packets (`:3331-3332`);
4. run normal game/entity physics (`:3334-3337`);
5. run existing bot physics (`:3337-3339`);
6. send client/demo/master traffic (`:3344-3357`).

`SV_Shutdown` (`src/sv_main.c:235`) calls `PR_GameShutDown` and `PR_UnLoadProgs` at lines 265-267, clears `sv` at lines 269-270, and clears `svs.clients` at line 284. A future EvoBot shutdown hook belongs before game/VM teardown so adapter-owned fake clients and any map-local references can be released while MVDSV state is still valid. It must be idempotent because shutdown can be reached through normal quit and error paths.

## Client-slot and player lifecycle

### State and identity

The slot-state enum is `src/server.h:146-154`:

`cs_free -> cs_preconnected -> cs_connected -> cs_spawned -> cs_zombie -> cs_free`

`client_t` starts at `src/server.h:190`. Important members are state (`:192`), userinfo (`:206-208`), `lastcmd` (`:213`), player edict (`:220`), and—under `USE_PR2`—`isBot`/`botcmd` (`:221-224`). The reserved relationship is slot `i` to edict `i + 1`, established in `SV_SpawnServer` at `src/sv_init.c:478-489`.

Use a generation-tagged EvoBot handle such as `{ map_generation, slot_index }`; never expose `client_t *` or `edict_t *`. A slot index alone becomes stale when clients disconnect or the map changes.

### Human connect and initial spawn

`SVC_DirectConnect` (`src/sv_main.c:1237`) validates protocol, challenge, userinfo, passwords, and reconnect state. `CountPlayersSpecsVips` supplies the first free slot (`src/sv_main.c:1157-1194`). The slot is zeroed and initialized at `src/sv_main.c:1374-1433`, including its reserved edict.

Sign-on string commands then execute:

- `Cmd_New_f` (`src/sv_user.c:203`) settles real-IP/login state and moves the slot to `cs_connected` at line 298.
- `Cmd_PreSpawn_f` (`src/sv_user.c:672`) streams sign-on data.
- `Cmd_Spawn_f` (`src/sv_user.c:824`) initializes the client edict at lines 881-897 and sends final sign-on state.
- `Cmd_Begin_f` (`src/sv_user.c:960`) changes the slot to `cs_spawned` at line 977, restores spawn parms, and calls `PR_GameClientConnect` and `PR_GamePutClientInServer` at lines 988-997.

### Respawn

MVDSV has no standalone engine `RespawnClient` routine. After initial connection, death/respawn policy is game-code behavior driven by usercmd buttons and game callbacks. For KTX, `k_respawn` is `ktx-reference/src/client.c:910`; it prepares respawn parms and calls KTX `PutClientInServer` at lines 929-931. Frogbot requests respawn by setting jump in its next command: `BotRequestRespawn` is `ktx-reference/src/bot_movement.c:71-76`, and `BotSetCommand` turns it into button bit 2 at `ktx-reference/src/bot_movement.c:539-562`.

Therefore EvoBot should request a normal respawn with the same input/button route; it should not directly rewrite health, origin, deadflag, or spawn fields.

### Disconnect/free

`SV_DropClient` (`src/sv_main.c:377`) sends the normal game callback for spawned humans at lines 400-405 and marks the slot `cs_zombie` at line 427. `SV_CheckTimeouts` later makes zombies reusable at `src/sv_main.c:3111-3113`.

Existing bots bypass the zombie delay: `SV_DropClient` redirects `isBot` clients to `RemoveBot` at `src/sv_main.c:390-395`; `RemoveBot` sends the PR2 disconnect callback when a VM is present and marks the slot `cs_free` at `src/pr2_cmds.c:2280-2293`.

## Human network usercmd flow

For a normal spawned client, the exact flow is:

1. Windows calls `SV_Frame` (`src/sv_sys_win.c:815` or `:911`).
2. `SV_Frame` calls `SV_ReadPackets` (`src/sv_main.c:3332`).
3. `SV_ReadPackets` obtains a datagram with `NET_GetPacket`, identifies the slot, and calls `SV_ExecuteClientMessage` (`src/sv_main.c:2995-3062`). Delayed packets use the same destination at lines 2985-2990.
4. `SV_ExecuteClientMessage` validates the netchannel and sets `sv_client`/`sv_player` (`src/sv_user.c:4478-4608`).
5. On `clc_move` (`src/sv_user.c:4686`), it reads checksum/loss and decodes `oldest`, `oldcmd`, and `newcmd` using `MSG_ReadDeltaUsercmd` (`src/sv_user.c:4692-4716`; decoder `src/common.c:474-507`).
6. After checksum validation, it calls `SV_ExecuteClientMove` (`src/sv_user.c:4734-4764`).
7. `SV_ExecuteClientMove` calls `SV_PreRunCmd`, replays `lastcmd`/old commands for dropped packets, runs `newcmd`, then calls `SV_PostRunCmd` (`src/sv_user.c:4261-4305`).
8. Each `SV_RunCmd` copies buttons, impulse, movement, and angles into the player edict (`src/sv_user.c:3704-3737`). Commands longer than 50 ms are split recursively at lines 3691-3701.
9. For a player, `PR_GameClientPreThink(0)` runs before movement (`src/sv_user.c:3741-3757`). PR1 dispatch is `src/pr_exec.c:796-805`; PR2 dispatch is `src/pr2_exec.c:270-275`.
10. MVDSV copies the edict/client state and command into global `pmove`, builds the nearby physent list, fills movement variables, and calls `PM_PlayerMove` (`src/sv_user.c:3775-3813`).
11. `PM_PlayerMove` categorizes position, handles jumping/friction, and selects water/fly/air movement (`src/pmove.c:886-949`).
12. `SV_RunCmd` copies `pmove` results back to the edict, relinks it, and invokes touch callbacks (`src/sv_user.c:3842-3879`).
13. After the packet's command group, `SV_PostRunCmd` calls `PR_GameClientPostThink(0)` (`src/sv_user.c:4168-4206`). PR1 dispatch is `src/pr_exec.c:810-819`; PR2 dispatch is `src/pr2_exec.c:281-286`.
14. `SV_ExecuteClientMessage` stores `newcmd` as `cl->lastcmd` and clears its buttons to avoid repeated fire (`src/sv_user.c:4766-4767`).

Important behavior: pre-think is run once per actual/replayed `SV_RunCmd`, whereas post-think is run once after `SV_ExecuteClientMove` finishes its group.

## PR1 versus PR2/QVM

This build defines both `SERVERONLY` and `USE_PR2` at `CMakeLists.txt:181-182`. `src/pr2.h:31-91` aliases generic `PR_*` calls to PR2 wrappers. Those wrappers use `sv_vm` when a PR2 module loads and fall back to PR1 when it does not.

### PR1

- `PR1_LoadProgs` (`src/pr_edict.c:1138`) tries the configured `<sv_progsname>.dat`, then `qwprogs.dat`, `spprogs.dat`, and optionally `progs.dat` at lines 1151-1177.
- It maps the program lumps and global storage at `src/pr_edict.c:1198-1209` and endian-fixes them at lines 1211-1247.
- `PR1_InitProg` allocates variable-sized game edicts at `src/pr_edict.c:1252-1256`.
- Common generated globals and fields are `globalvars_t`/`entvars_t` in `src/progdefs.h:26-160`. `pr_global_struct`, `pr_globals`, and `pr_edict_size` are declared at `src/progs.h:85-95`.
- `PR_GLOBAL`, `EDICT_NUM`, `NUM_FOR_EDICT`, `EDICT_TO_PROG`, `PROG_TO_EDICT`, and typed field/global macros are at `src/progs.h:109-170`.
- Named lookup is `ED_FindGlobal` (`src/pr_edict.c:242`), `ED_FindField` (`:222`), `ED1_FindFieldOffset` (`:261`), and `PR1_GetEdictFieldValue` (`:300-328`).
- Game callbacks execute QC function offsets through `PR_ExecuteProgram` (`src/pr_exec.c:359`) and wrappers at `src/pr_exec.c:752-819`.
- PR1 has no real game-shutdown callback: `PR1_GameShutDown` is an empty macro at `src/progs.h:227`.

### PR2/QVM/native module

- VM modes are `VMI_NONE`, `VMI_NATIVE`, `VMI_BYTECODE`, and `VMI_COMPILED` (`src/vm.h:20-25`).
- `PR2_LoadProgs` calls `VM_Create` and falls back to `PR1_LoadProgs` if it returns null (`src/pr2_exec.c:425-436`).
- `VM_Create` attempts native loading when selected, otherwise loads QVM bytecode and compiles or prepares the interpreter (`src/vm.c:1235-1329`).
- `PR2_InitProg` calls `GAME_INIT`, imports `gameData_t`, validates the API, and maps module edicts/globals/fields into the common server view (`src/pr2_exec.c:571-619`).
- Lifecycle/client callbacks use numbered `GAME_*` calls through `VM_Call`; see `src/pr2_exec.c:222-286` and `:398-418`.
- Module-to-engine operations use `G_*` syscalls dispatched by `PR2_GameSystemCalls` (`src/pr2_cmds.c:2563`).
- `PR2_GetEdictFieldValue` and `ED2_FindFieldOffset` use the PR2 field table but fall back to PR1 when `sv_vm` is absent (`src/pr2_edict.c:30-57`).

Adapter consequence: direct access to the common `edict_t.v`/`pr_global_struct` view works only inside the adapter and only while the current map/game module is live. Optional named fields must be resolved per map/module load. No field offset, string pointer, edict pointer, or global pointer may be cached across `SV_SpawnServer`.

## Existing MVDSV fake-client and bot support

### Engine-side API

The public PR2 syscall numbers are `G_Add_Bot`, `G_Remove_Bot`, `G_SetBotUserInfo`, and `G_SetBotCMD` at `src/g_public.h:157-160`. `PR2_GameSystemCalls` dispatches them at `src/pr2_cmds.c:2804-2813`.

`PF2_Add_Bot` (`src/pr2_cmds.c:2144`) performs all current fake-client creation:

- count player/spectator use and enforce `maxclients` (`:2156-2180`);
- find and clear a free `client_t` (`:2181-2197`);
- create bot userinfo (`:2199-2208`);
- set `cs_spawned`, `isBot`, timing, speed/gravity, and edict data (`:2210-2237`);
- broadcast the client update (`:2261`);
- call `PR2_GameClientConnect` and `PR2_GamePutClientInServer` (`:2263-2268`).

`PF2_SetBotCMD` (`src/pr2_cmds.c:2372`) validates the slot and writes msec, angles, three movement axes, buttons, and impulse to `client_t.botcmd` at lines 2388-2396. A pending `fixangle` is translated at lines 2398-2403.

`SV_RunBots` (`src/sv_phys.c:1026`) runs after normal `SV_Physics` in `SV_Frame`. It rate-limits to the selected physics FPS, calls `SV_ProgStartFrame(true)` at line 1074, then for each `isBot` slot calls `SV_PreRunCmd`, `SV_RunCmd(&cl->botcmd, ...)`, and `SV_PostRunCmd` at lines 1079-1095. It stores/clears the command at lines 1097-1100.

The bot-specific game start frame is PR2 API-dependent: `PR2_GameStartFrame` refuses a bot frame without a VM or with API `< 15` (`src/pr2_exec.c:222-229`). The non-PR2 macro explicitly suppresses bot start frames (`src/progs.h:276`).

### Commands

MVDSV itself registers no `addbot`/`removebot` server-console command. The engine capability is the four PR2 syscalls above. KTX exposes a client game command named `botcmd` (`ktx-reference/src/commands.c:1045-1048`), including `addbot` and `removebot` subcommands (`ktx-reference/src/bot_commands.c:2320-2326`). Thus `/botcmd addbot` is a KTX command, not an MVDSV console command.

## KTX/Frogbot bot creation flow

The command path is:

1. A client's `clc_stringcmd` reaches `SV_ExecuteUserCommand` (`src/sv_user.c:4780-4783`).
2. Engine commands are checked first; then `SV_ExecutePRCommand` calls `PR_ClientCmd` (`src/sv_user.c:3398-3435`).
3. `PR2_ClientCmd` calls `VM_Call(... GAME_CLIENT_COMMAND ...)` (`src/pr2_exec.c:292-295`).
4. KTX's VM entry dispatches `GAME_CLIENT_COMMAND` to `ClientCommand` (`ktx-reference/src/g_main.c:372-375`).
5. KTX `ClientCommand` resolves the named command (`ktx-reference/src/g_cmd.c:43-90`); the table entry for `botcmd` is `ktx-reference/src/commands.c:1047`.
6. `FrogbotsCommand` (`ktx-reference/src/bot_commands.c:2388`) selects the `addbot` handler from the table at lines 2320-2325 and dispatches it after validation.
7. `FrogbotsAddbot_f` (`ktx-reference/src/bot_commands.c:367`) parses skill/team and calls `FrogbotsAddbot` at line 397.
8. `FrogbotsAddbot` (`ktx-reference/src/bot_commands.c:275`) calls `trap_AddBot` at line 333 and then sets team/name/skill through `trap_SetBotUserInfo` at lines 351-355.
9. KTX syscall wrappers are `ktx-reference/src/g_syscalls.c:409-429`.
10. MVDSV dispatches `G_Add_Bot` to `PF2_Add_Bot` (`src/pr2_cmds.c:2804-2805`).
11. `PF2_Add_Bot` creates the fake client and calls connect/put-in-server (`src/pr2_cmds.c:2144-2271`).
12. KTX receives `GAME_CLIENT_CONNECT` and calls `ClientConnect` (`ktx-reference/src/g_main.c:194-243`), then receives `GAME_PUT_CLIENT_IN_SERVER` and calls `k_respawn` (`ktx-reference/src/g_main.c:245-263`).

Removal reverses the path: KTX `FrogbotsRemoveBot` calls `trap_RemoveBot` (`ktx-reference/src/bot_commands.c:400-409`), MVDSV dispatches to `PF2_Remove_Bot` (`src/pr2_cmds.c:2806-2808`), and `RemoveBot` invokes disconnect and frees the slot (`src/pr2_cmds.c:2274-2293`).

## KTX/Frogbot movement-command flow

The complete existing bot path is:

1. `SV_Frame` runs normal `SV_Physics`, then `SV_RunBots` (`src/sv_main.c:3334-3339`).
2. `SV_RunBots` calls `SV_ProgStartFrame(true)` (`src/sv_phys.c:1073-1075`).
3. `SV_ProgStartFrame` calls `PR_GameStartFrame(true)` (`src/sv_phys.c:838-845`).
4. `PR2_GameStartFrame` calls `VM_Call(GAME_START_FRAME, time, isBotFrame)` (`src/pr2_exec.c:222-231`).
5. KTX's `GAME_START_FRAME` branch calls `BotStartFrame` for the bot frame (`ktx-reference/src/g_main.c:167-179`).
6. `BotStartFrame` (`ktx-reference/src/bot_commands.c:2674`) initializes bot support on early bot frames, then iterates players and calls `BotSetCommand` for every `self->isBot` at lines 2691-2750.
7. `BotSetCommand` (`ktx-reference/src/bot_movement.c:425`) computes command msec, desired view angles, forward/side/up movement, buttons, and impulse, then calls `trap_SetBotCMD` at lines 559-562.
8. The wrapper emits syscall `G_SetBotCMD` (`ktx-reference/src/g_syscalls.c:424-429`).
9. `PR2_GameSystemCalls` calls `PF2_SetBotCMD` (`src/pr2_cmds.c:2812-2814`), which stores the values in `client_t.botcmd` (`src/pr2_cmds.c:2388-2396`).
10. Control returns to `SV_RunBots`, which calls `SV_RunCmd(&cl->botcmd, false, false)` (`src/sv_phys.c:1079-1095`).
11. From this point the flow is the normal path: game pre-think, `PM_PlayerMove`, edict commit/touches, and game post-think (`src/sv_user.c:3635-3881`, `:4168-4213`).

KTX receives those callbacks through `GAME_CLIENT_PRETHINK` and `GAME_CLIENT_POSTTHINK` (`ktx-reference/src/g_main.c:294-345`). Its `PlayerPreThink` is `ktx-reference/src/client.c:3712`; it calls `BotPreThink` at line 3744. `PlayerPostThink` is `ktx-reference/src/client.c:4528`.

## World-query and observation services for the adapter

### Trace

Use `SV_Trace` (`src/sv_world.c:729-774`) inside the adapter. It includes world BSP and linked solid entities, respects movement type flags, excludes the passed edict, and may apply antilag for a human passed edict. Return an EvoBot-owned trace result containing at least:

- `all_solid`, `start_solid`, `fraction`;
- end position and plane normal/distance;
- hit entity ID (0 for world, a sentinel for none);
- `in_open` and `in_water` if needed by movement code.

Translate an EvoBot-owned trace mask/mode enum to MVDSV `MOVE_*` flags in the adapter. Do not publish raw `trace_t` or an `edict_t *`.

**Runtime verification required:** decide whether AI sight/weapon traces should request human-style antilag. The safe initial behavior is current-world traces without `MOVE_LAGGED`; EvoBot decisions should normally observe authoritative current state.

### Point contents

Wrap `SV_PointContents` (`src/sv_world.c:442-445`) and translate MVDSV `CONTENTS_*` values to an EvoBot-owned enum. Do not make portable code depend on Quake header numeric constants.

### PVS

For point-to-point testing, follow the existing `PF2_checkclient` pattern: find the source leaf/PVS with `CM_PointInLeaf`/`CM_LeafPVS`, find the destination leaf number with `CM_Leafnum`, then test its bit (`src/pr2_cmds.c:640-674`). For a tolerant eye-volume or entity query, use `CM_FatPVS` and test cached entity leafs as `PF2_VisibleTo` does (`src/pr2_cmds.c:2527-2555`).

The adapter should offer plain calls such as `in_pvs(from, to)` and `entity_in_pvs(view_origin, entity_id)`. It must consume/copy PVS results immediately because the engine owns and reuses the returned buffers.

PVS means potentially visible, not line-of-sight. EvoBot should combine it with a trace when actual visibility is required.

### Entity and player observation

Iterate integer entity IDs from 0 to `sv.num_edicts - 1`; use `EDICT_NUM`, skip `e.free`, and copy selected values from `edict_t.v` into an EvoBot-owned snapshot. The core layout is:

- server entity wrapper `edict_t`: `src/progs.h:76-81`;
- common entity fields `entvars_t`: `src/progdefs.h:82-160`;
- entity count/table: `src/server.h:96-100`.

The initial snapshot should remain small: stable entity ID plus map generation, classname/model strings copied into bounded buffers, origin, velocity, angles, mins/maxs, movetype, solid, flags, health/deadflag, view offset, team, items/weapon/ammo, and whether it is a connected player/bot. Do not expose `think`, `touch`, `owner`, `enemy`, string offsets, or raw pointers unless a later concrete requirement justifies a translated ID/value.

For players, iterate `svs.clients[0..MAX_CLIENTS)`, inspect slot state, then copy from `client_t` and `client_t.edict`. Use `cs_spawned` to distinguish active players. Keep transport/private fields such as netchannel, real IP, login, buffers, and filesystem handles out of EvoBot.

Optional mod fields should be found by name after each map/module load using `ED_FindFieldOffset`/`PR_GetEdictFieldValue`, copied to EvoBot-owned values, and treated as absent when lookup returns zero/null. Do not build mod profiles yet.

## Smallest practical EvoBot integration

### Boundary shape

Use a portable host-vtable declared by EvoBot, containing callbacks that accept and return only EvoBot types. The MVDSV adapter constructs that table.

Suggested portable types (names preliminary):

- `evobot_vec3_t`
- `evobot_client_handle_t { uint32_t map_generation; uint16_t slot; }`
- `evobot_entity_handle_t { uint32_t map_generation; uint16_t entity; }`
- `evobot_usercmd_t`
- `evobot_trace_request_t` / `evobot_trace_result_t`
- `evobot_entity_snapshot_t`
- `evobot_player_snapshot_t`
- `evobot_frame_t { double server_time; double dt; bool paused; uint32_t map_generation; }`
- `evobot_host_api_t` with fake-client, command, trace, contents, PVS, observation, and movement-simulation callbacks

`src/evobot_qw_adapter.h` should include only EvoBot-owned headers and expose lifecycle entry points to MVDSV. `src/evobot_qw_adapter.c` alone includes `qwsvdef.h`, `server.h`, `sv_world.h`, `pmove.h`, or other MVDSV headers.

### Lifecycle hooks

1. **Initialization:** future `EvoBot_QW_Init` call in `SV_Init`, after existing core server initialization (`src/sv_main.c:4044-4057`). It constructs the adapter vtable and calls portable `EvoBot_Init` once.
2. **Map loaded:** future `EvoBot_QW_MapLoaded` call in `SV_SpawnServer` after map serverinfo/baseline completion (`src/sv_init.c:652-659`). It increments the map generation, resolves per-map optional fields, and publishes copied map identity/bounds. It must invalidate previous handles first.
3. **One call per server frame:** future `EvoBot_QW_Frame` call directly from `SV_Frame`, not from `SV_Physics` or `SV_RunBots`, so it occurs exactly once for every server frame. The preferred point is after normal `SV_Physics` and before fake-client command consumption. Pass `paused`; do not queue movement while paused.
4. **Shutdown:** future `EvoBot_QW_Shutdown` call near the start of `SV_Shutdown`, after the `sv.state` guard but before `PR_GameShutDown`/`PR_UnLoadProgs` (`src/sv_main.c:235-267`). It removes owned fake clients and then calls portable `EvoBot_Shutdown`. Make it idempotent.

To place the frame hook cleanly, the later implementation will need a small reorder of the current block at `src/sv_main.c:3334-3342`: run/skip normal physics, call EvoBot exactly once with paused state, and then consume EvoBot bot commands only when unpaused. This is a targeted integration change, not a physics rewrite.

### Fake-client creation/removal

The smallest initial implementation should reuse the behavior of `PF2_Add_Bot`/`RemoveBot` rather than invent a second client lifecycle. Two implementation options exist:

1. **Smallest patch:** adapter calls narrowly exposed engine helpers based on the current `PF2_Add_Bot` and `RemoveBot` implementations.
2. **Cleaner follow-up:** extract neutral `SV_AddBotClient`, `SV_RemoveBotClient`, `SV_SetBotUserInfo`, and `SV_SetBotCommand` helpers, then make both PR2 syscalls and the EvoBot adapter call them.

Option 2 avoids making EvoBot depend on a `PF2_*` symbol but touches more existing code. For the first integration, keep any extraction mechanical and behavior-preserving.

Creation must return a generation-tagged handle, set `isBot`, reserve the matching player edict, publish userinfo, and invoke normal game connect/put-in-server callbacks. Removal must invoke game disconnect and free only a slot owned by EvoBot.

**Runtime verification required — ownership conflict:** MVDSV has only one `client_t.isBot` flag, and KTX `BotStartFrame` sends commands to every game edict whose `isBot` field is true (`ktx-reference/src/bot_commands.c:2712-2750`). If Frogbot and EvoBot are enabled simultaneously, KTX can overwrite EvoBot's `botcmd` before `SV_RunBots` consumes it. The smallest first version should explicitly require Frogbot bot control to be disabled while EvoBot owns bots. Coexistence later requires an engine-side bot-owner discriminator and a corresponding KTX policy; do not silently race two controllers.

### Usercmd submission

Translate `evobot_usercmd_t` into MVDSV `usercmd_t` exactly once per owned bot/frame and store it through the shared bot-command helper. Preserve MVDSV's current fixangle handling (`src/pr2_cmds.c:2398-2403`). `SV_RunBots` should remain the commit point into `SV_RunCmd`.

Validate and clamp in the adapter:

- slot ownership and generation;
- `msec` to the representable byte range and the intended frame duration;
- movement axes to signed 16-bit range;
- buttons/impulse to bytes;
- finite angles and movement values.

The current storage is a single `client_t.botcmd`, not a queue. Multiple submissions before consumption overwrite each other. The portable API should document one final command per bot per frame.

### World traces, contents, PVS, and observation

Implement these as synchronous adapter callbacks using the current authoritative server state:

- trace -> `SV_Trace`;
- point contents -> `SV_PointContents`;
- PVS -> `CM_PointInLeaf`/`CM_LeafPVS` or `CM_FatPVS` plus leaf-bit tests;
- entity/player observation -> copy from `sv.edicts`, `sv.num_edicts`, `svs.clients`, and `client_t.edict`.

All outputs are copied. No MVDSV pointer or transient string/PVS storage survives the callback.

### Movement simulation

Actual bot movement should continue through `SV_RunCmd`; do not directly call `PM_PlayerMove` to move a real bot because `SV_RunCmd` supplies game pre/post-think, command splitting, physent construction, result commit, relinking, and touch callbacks.

For read-only predictive simulation, add a separate adapter operation that:

1. accepts an EvoBot-owned player state and command;
2. saves the global `pmove` and `movevars` state;
3. populates a temporary `pmove`, including the world and nearby solid physents using the same logic as `AddLinksToPmove` (`src/sv_user.c:3455-3524`);
4. calls `PM_PlayerMove` (`src/pmove.c:886`);
5. copies the predicted state/touches to EvoBot-owned output;
6. restores all engine globals and performs no edict writes, relinks, touches, or game callbacks.

**Runtime verification required — non-reentrancy:** `pmove` and `movevars` are global (`src/pmove.h:91-92`; `src/pmove.c:29`), and `AddLinksToPmove` is currently static to `sv_user.c`. Predictive simulation must run only on the server thread outside an active `SV_RunCmd`. The implementation should first extract a side-effect-free physent builder or a scoped engine helper; duplicating collision-list logic in portable EvoBot is not acceptable.

## Preliminary folder layout

```text
src/
  evobot/
    evobot.h              # Portable lifecycle API; includes only EvoBot headers.
    evobot.c              # Init, map notification, one-frame coordinator, shutdown.
    evobot_types.h        # POD vectors, handles, commands, traces, snapshots.
    evobot_host.h         # EvoBot-owned host callback/vtable declarations.
    evobot_world.c        # Portable snapshot/query orchestration only; no MVDSV access.
    evobot_world.h
  evobot_qw_adapter.c     # The only MVDSV-aware bridge; includes MVDSV headers.
  evobot_qw_adapter.h     # Lifecycle bridge declarations using EvoBot-owned types only.
```

Only the files actually needed for the first vertical slice should be added. `CMakeLists.txt:33-81` is the future source-list integration point. Do not add AI, navigation, Dijkstra, JSON, mod profiles, or unrelated support layers yet.

## Recommended first vertical slice

1. Portable init/map/frame/shutdown counters and logging through an adapter callback.
2. One EvoBot-owned fake client using the shared fake-client lifecycle.
3. One plain `evobot_usercmd_t` submitted per frame and consumed by existing `SV_RunBots`/`SV_RunCmd`.
4. Read-only player/entity snapshots.
5. Trace, point-contents, and PVS callbacks.
6. Predictive movement only after actual movement is verified and the global `pmove` save/restore boundary has a focused test.

This slice proves lifecycle, ownership, game callbacks, movement parity, and world access without introducing policy, navigation, serialization, or mod-specific behavior.
