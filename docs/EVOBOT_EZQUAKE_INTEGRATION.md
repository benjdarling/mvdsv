# EvoBot ezQuake integration investigation

## Scope and baseline

This document investigates ezQuake as a second EvoBot host. It does not implement the integration, move source files, add rendering, or add reachabilities, routing, movement, or AI.

The source inspection used these repository revisions:

- MVDSV `evobot-bootstrap`: `3e5da1ee046adcc6baf83ff9779d7862a5bd32c5`
- ezQuake `master`: `a86996a3d33dc1bc3fb15bfe7bcadd662b822557`

Function names and control flow below were verified against the current files. Line references identify this baseline only and should not be used instead of re-checking the source before an implementation edit.

The conclusion is **B: share the portable core and most QuakeWorld translation design, with a small host-specific ezQuake layer**. The current MVDSV adapter cannot be compiled unchanged in ezQuake. The server ancestry is close enough that most collision, entity, VM, filesystem, and neutral-command code can be ported directly, but ezQuake has a different process lifecycle and renderer, and it does not yet contain MVDSV's EvoBot-specific fake-client refactor.

## How the ezQuake local server operates

ezQuake is one executable containing a client and, unless built with `CLIENTONLY`, a QuakeWorld server. It is not an in-process game simulation that bypasses the network client path.

1. `Host_Init` initializes collision and models, calls `SV_Init`, then initializes the client (`src/host.c:773-781`).
2. The `map` command is deferred through `SV_Map`. For a client/server build, `SV_Map(true)` calls `CL_BeginLocalConnection` and then `SV_SpawnServer` (`src/sv_ccmds.c:406-466`). The playable client connects through the normal loopback network path.
3. `Host_Frame` calls `CL_Frame` (`src/host.c:544-552`). `CL_Frame` calls `CL_ServerFrame`, which saves the global client `pmove`, invokes `SV_Frame`, and restores `pmove` (`src/cl_main.c:2558-2569`). With independent physics disabled this happens once per client frame; with independent physics enabled it happens on physics frames (`src/cl_main.c:2698-2716`, `:2743-2757`). Zero-time server calls can also process loopback packets without advancing physics (`src/cl_main.c:2594-2601`, `:2788-2791`).
4. `SV_Frame` advances `sv.time`, reads packets, runs `SV_Physics`, runs PR2 bot clients through `SV_RunBots`, and sends client messages (`src/sv_main.c:3226-3287`).
5. Rendering occurs later in the same main thread. `R_RenderView` sets the 3D matrices, draws the world and entities, adds 3D effects/HUD preparation, and finally asks the selected renderer backend to flush the view (`src/r_rmain.c:869-901`).

The local server and renderer therefore share an address space and run sequentially on the main thread. A renderer may safely query an immutable EvoBot debug view after `SV_Frame`, provided it checks that a local server and matching map are still active and the query API invalidates views on map clear or navigation regeneration.

### Server initialization and shutdown

`SV_Init` initializes the progs subsystem and server-local commands/cvars (`src/sv_main.c:3962-3983`). The equivalent EvoBot initialization point is the end of `SV_Init`, as in MVDSV.

ezQuake shutdown differs materially from dedicated MVDSV:

- `SV_Shutdown` can be called by `Host_EndGame` and `Host_Error` while the ezQuake process continues (`src/host.c:466-500`). It unloads game code and clears `sv` and clients (`src/sv_main.c:224-276`).
- `Host_Shutdown` has its own recursion guard, calls `SV_Shutdown`, and then shuts down the client, networking, console, cvars, and filesystem (`src/host.c:858-884`).

For ezQuake, `SV_Shutdown` should clear only the current EvoBot map and fake clients. Full `EvoBot_Shutdown` belongs in `Host_Shutdown`, outside the `if (!sv.state) return` guard in `SV_Shutdown`. Otherwise ending a local game would permanently shut down EvoBot while the client process remains alive, and quitting without ever starting a server would miss EvoBot shutdown. Both adapter calls must remain idempotent.

### Map loading

`SV_SpawnServer` performs these relevant steps:

- saves current client spawn parameters;
- removes PR2 bot clients, calls `PR_GameShutDown`, and unloads the old progs (`src/sv_init.c:246-269`);
- clears the level state and loads PR1 or PR2 game code (`src/sv_init.c:333-361`);
- loads the BSP into `sv.worldmodel` and sets `sv.map_checksum`/`sv.map_checksum2` (`src/sv_init.c:470-495`);
- calls `SV_ClearWorld`, installs the world and inline models, loads live edicts, and changes `sv.state` from `ss_loading` to `ss_active` (`src/sv_init.c:498-614`);
- runs two settling physics frames, records movement variables and baselines, and publishes the map name (`src/sv_init.c:616-644`).

The current MVDSV hook positions remain correct in ezQuake:

- call adapter `MapCleared` near the start of `SV_SpawnServer`, before old bot clients, game code, edicts, or hunk-backed map data are destroyed;
- call adapter `MapLoaded` after `ss_active`, both settling frames, movement-variable capture, baseline creation, and serverinfo map publication;
- pass `sv.mapname` and the collision checksum `sv.map_checksum`.

The start hook must tolerate the first map, when no EvoBot map is loaded. The loaded hook must not fire if map loading aborts.

### PR1 and PR2 execution

ezQuake builds its server sources with `USE_PR2` and `WITH_NQPROGS` (`CMakeLists.txt:847-854`). `sv_progtype` selects PR1 DAT, native module, interpreted QVM, or JIT QVM (`src/pr2_exec.c:32-36`). `PR2_LoadProgs` tries `VM_Create` and falls back to `PR1_LoadProgs` when no VM is created (`src/pr2_exec.c:425-436`).

For PR1, `PR1_LoadProgs` tries `<sv_progsname>.dat`, then `qwprogs.dat`, `spprogs.dat`, and finally NetQuake `progs.dat` when enabled (`src/pr_edict.c:1138-1177`). The existing single-player EvoBot workflow therefore remains valid with the unmodified `evosp/spprogs.dat`; it does not require a new game-code format.

The `PR2_Game*` wrappers dispatch to the VM when `sv_vm` exists and otherwise to PR1 functions. This includes `GameClientConnect`, `GamePutClientInServer`, `GameClientDisconnect`, `GameClientPreThink`, and `GameClientPostThink` (`src/pr2_exec.c:220-285`). An engine-owned EvoBot fake client can consequently support both PR1 and PR2 through the same wrapper calls.

### Human and fake-client command paths

A normal human client is allocated by `SVC_DirectConnect`, completes `new`/`prespawn`/`spawn`/`begin`, and reaches game code through `PR_GameClientConnect` and `PR_GamePutClientInServer`. Move messages are parsed by `SV_ExecuteClientMessage`; `SV_ExecuteClientMove` replays dropped commands as needed and calls `SV_PreRunCmd`, `SV_RunCmd`, and `SV_PostRunCmd` (`src/sv_user.c:4122-4175`, `:4343-4634`).

The existing PR2 bot ABI uses `PF2_Add_Bot`, `PF2_Remove_Bot`, and `PF2_SetBotCMD` in `src/pr2_cmds.c`. It sets `client_t.isBot`, stores a `usercmd_t botcmd`, and lets game code set `msec`, angles, moves, buttons, and impulse. `SV_RunBots` calls the same `SV_RunCmd` player-physics path, copies the command to `lastcmd`, clears `botcmd`, and updates client timing (`src/sv_phys.c:1025-1109`).

`SV_RunCmd` copies game-code velocity into `pmove`, executes `PM_PlayerMove`, and copies the result back to the edict (`src/sv_user.c:3508-3720`). Consequently damage knockback remains ordinary game-code velocity followed by ordinary player physics; no EvoBot position or velocity correction belongs in either adapter.

The ezQuake bot timing distinction must be preserved:

- a dedicated `SERVERONLY` build schedules bot physics at `sv_maxfps` with remainder accumulation;
- the integrated client/server build runs bots when local server time advances and uses the current `sv_frametime` (`src/sv_phys.c:1030-1067`).

EvoBot's neutral command must be prepared inside `SV_RunBots`, after `SV_ProgStartFrame(true)` establishes the bot-frame timing and before the bot loop consumes and clears `botcmd`. It should use that `sv_frametime`, not client render time or a hard-coded millisecond value.

### Collision, movement, entities, and filesystem

The interfaces used by the current adapter are present in ezQuake and retain the same core semantics:

- `cmodel_t`, `hull_t`, clipnodes, planes, `CM_HullPointContents`, `CM_LoadMap`, and inline models are in `src/cmodel.c`/`cmodel.h`;
- `player_mins`, `player_maxs`, `PM_PlayerMove`, `MIN_STEP_NORMAL`, and the QuakeWorld movement state are in `src/pmove.c`, `pmove.h`, and `pmovetst.c`;
- `SV_HullForEntity`, `SV_PointContents`, `SV_ClipMoveToEntity`, and `SV_Trace` are in `src/sv_world.c` (`:56`, `:442`, `:481`, `:729` on this baseline);
- live server state is held in `sv`, `svs.clients`, `sv.edicts`, and `sv.num_edicts` from `src/server.h`;
- `EDICT_NUM`, VM-aware `PR_GetEntityString`, and `PR_GetEdictFieldValue` provide the same entity access model through `src/progs.h` and `src/pr2.h`;
- `FS_OpenVFS`, `FS_GAME_OS`, and the `VFS_*` operations have compatible signatures in `src/fs.h`.

The headers are not identical despite the common ancestry. ezQuake's `pmove_t` and renderer-facing model types contain client extensions, for example. Portable EvoBot must continue receiving copied plain-C values only.

`FS_GAME_OS` also has an important ezQuake behavior: reads search the user directory, home game directory, then basedir game directory; writes select the first writable form (`src/fs.c:983-1032`). The portable API should define paths as relative to the active game's writable data root. Its write contract should require creation of parent directories, because EvoBot currently writes paths such as `evobot/nav/e1m1.botnav` and a raw `FS_OpenVFS(..., "wb", FS_GAME_OS)` does not itself provide an unambiguous cross-host parent-directory guarantee.

## Exact integration-point comparison

| Concern | Current MVDSV point | ezQuake equivalent | Required difference |
|---|---|---|---|
| Core initialization | End of `SV_Init` in `src/sv_main.c`; `EvoBot_QW_Init` | End of `SV_Init` in `src/sv_main.c` | Register ezQuake commands here; initialize debug cvars/module separately. |
| Old map clear | Start of `SV_SpawnServer` in `src/sv_init.c` | Start of the same function | Same placement, before old game code/map memory is released. |
| Map usable | End of `SV_SpawnServer`, after settling/baseline | Same function and phase | Same map identity; optionally verify client/local map match before rendering. |
| Portable frame | In `SV_Frame`, after `SV_Physics` and before `SV_RunBots` | Same function | Keep the same ordering. Zero-time packet-service calls repeat the same server time, so core must not advance simulation twice for an unchanged time. |
| Neutral bot command | `SV_RunBots`, after `SV_ProgStartFrame(true)` | Same function | Use integrated-server `sv_frametime`; do not use `cls.frametime`. |
| Map/server stop | Current MVDSV calls full shutdown from `SV_Shutdown` | `SV_Shutdown` can return to a live ezQuake client | Call `MapCleared`, not full shutdown, here. |
| Process shutdown | Dedicated process ultimately exits | `Host_Shutdown` in `src/host.c` | Call full adapter/core shutdown here even if no server is active. |
| Fake-client allocation | `SV_AddBotClient(..., gamecode_bot)` added by EvoBot work | Only `PF2_Add_Bot` exists | Port the small engine helper/refactor and PR1 context setup. |
| Frogbot distinction | `client_t.gamecodeBot`; edict `isBot` only for game-code bots | Only `client_t.isBot`; edict `isBot` is always set | Add the distinction so EvoBot is engine-bot physics without becoming Frogbot-controlled game code. |
| World-only trace declaration | Declared for adapter use in MVDSV `sv_world.h` | Implementation exists but public declaration is absent | Add the narrow declaration or an ezQuake wrapper. |
| Renderer | None in dedicated MVDSV | Common renderer plus classic/modern backends | Add an ezQuake-only debug module and backend-aware primitive API. |

### Fake-client parity changes required in ezQuake

The current adapter cannot link against unmodified ezQuake because `SV_AddBotClient` does not exist there. ezQuake's `PF2_Add_Bot` also assumes a game-code-created bot and currently:

- sets engine `client_t.isBot = 1`;
- sets the optional game-code edict `isBot` field to `1`;
- invokes PR2 connect/put-in-server directly;
- is only exposed as a PR2 system call.

MVDSV has already separated the reusable engine operation into `SV_AddBotClient(..., qbool gamecode_bot)`. For PR1 it sets spawn parms and the temporary `sv_client`/`sv_player` context before using the same `PR2_Game*` dispatch wrappers. It records `client_t.gamecodeBot`, sets the game-code edict `isBot` field only for actual game-code bots, clears that flag on removal/map clear, and restricts the Frogbot blocked-command retry to `gamecodeBot` clients.

Those are functional requirements for the ezQuake port, not unrelated MVDSV changes. Porting them preserves existing KTX/Frogbot behavior while allowing EvoBot-owned PR1 and PR2 clients. `PF2_Add_Bot` must remain a wrapper that passes `gamecode_bot = true`; EvoBot passes `false`. No KTX or Frogbot code should be removed or made responsible for EvoBot.

### Adapter reuse decision

The following current adapter sections can be ported with only include/name adjustments after the parity changes:

- server and monotonic time;
- world/player bounds;
- world-only player-hull trace;
- point contents and clipnode-tree translation;
- live interactor extraction and optional VM fields;
- game-relative file I/O, after the storage contract is clarified;
- opaque EvoBot handle-to-client-slot bookkeeping;
- fake-client add/remove/validity checks;
- neutral `botcmd`, view-angle, and `fixangle` preparation;
- portable status/navigation command wrappers.

The host lifecycle and rendering code must not be shared verbatim. The recommended files are:

```text
MVDSV repository
    src/evobot_mvdsv_adapter.c
    src/evobot_mvdsv_adapter.h

ezQuake repository
    src/evobot_ezq_adapter.c
    src/evobot_ezq_adapter.h
    src/evobot_ezq_debug.c
    src/evobot_ezq_debug.h
```

Renaming the current MVDSV adapter is optional and should not be mixed into the first ezQuake implementation merely for symmetry. Each leaf adapter may include its engine's headers. Neither portable core nor its debug inspection header may include either engine's headers.

A second shared "QuakeWorld adapter framework" is not recommended yet. It would add a bridge beneath an API that is already a bridge. If the two leaf adapters later accumulate meaningful duplicated fixes, a small QuakeWorld support component operating only on EvoBot plain types can be extracted then.

## Portable source ownership

### Options

1. **Keep EvoBot under MVDSV and copy it into ezQuake:** simplest for one experiment, but violates the one-canonical-copy requirement immediately. Copied fixes and generated project files will drift.
2. **Standalone `benjdarling/EvoBot` repository used as a submodule:** one canonical history, deterministic pinned revisions in each host, offline builds after initialization, and host repositories keep only integration code.
3. **CMake `FetchContent`:** also canonical, but ordinary configure can unexpectedly require network access and credentials. Local editing needs an override path and is less transparent in GitHub Desktop.
4. **Git subtree/vendor synchronization:** host clones are self-contained, but they contain duplicated source snapshots and require an explicit pull/push synchronization discipline.

### Recommendation

Create `benjdarling/EvoBot` and consume it as a pinned git submodule in both hosts, for example at `third_party/evobot`. Give the standalone repository a small CMake target such as `evobot_core` with public headers and no third-party dependencies. The host executable links that target and compiles its leaf adapter separately.

This is now appropriate because there are two real consumers and the portable boundary already exists. A separate static-library target is justified here by ownership and compile-boundary enforcement, not by runtime modularity.

For normal use:

- clone each host with submodules or run one documented `git submodule update --init --recursive` step;
- open the host and nested EvoBot repository as separate repositories in GitHub Desktop when committing changes;
- use a VS Code multi-root workspace containing EvoBot, MVDSV, and ezQuake for cross-repository work;
- commit portable changes in EvoBot first, then update each host's gitlink in a separate host commit;
- keep a CMake source-directory override only as an optional developer convenience for testing a sibling EvoBot checkout. The pinned submodule remains the default build path.

Before extraction, decide whether the older `evobot_nav.c`/`evobot_nav.h` implementation should be archived or omitted. The active public lifecycle currently uses `evobot_nav_convex.c`; blindly carrying both implementations into the canonical repository would make the supported navigation path ambiguous.

## Host API review

### Reusable without semantic change

These callbacks use portable values and map directly to both hosts:

- `print`
- `monotonic_time`
- `create_bot_client`, `remove_bot_client`, and `is_bot_client_valid`
- `world_bounds` and `player_bounds`
- `trace_player_world`
- `point_contents`
- `collision_tree` and `collision_node`
- `interactor_count` and `get_interactor`
- game-relative storage operations

The opaque client handle is the correct boundary. A client slot, `client_t *`, or edict number must never become a portable handle.

### Small changes recommended before a second host depends on the API

1. Add `api_version`, `struct_size`, and capability bits to `evobot_host_api_t`. The core should validate required capabilities for generation and fake clients and tolerate absent optional callbacks. This avoids inferring support from a scattered set of null function pointers and permits compatible extension.
2. Remove `server_time` unless a concrete use appears. `EvoBot_Frame(double server_time)` already supplies simulation time, and the callback is currently unused. `monotonic_time` remains distinct and valid for profiling generation.
3. Rename `file_size`/`read_file`/`write_file` to `storage_size`/`storage_read`/`storage_write`, or at minimum document them as active-game-relative storage. Require path traversal rejection and parent-directory creation on write. Do not expose an OS path to the portable core.
4. Rename `EVOBOT_CREATE_BOT_UNSUPPORTED_GAMECODE` to a host/build capability result such as `EVOBOT_CREATE_BOT_UNSUPPORTED`. PR1 and PR2 are both supported in the current MVDSV integration, so the existing name and error text are now misleading.
5. Document callback lifetime: the core copies the table during `EvoBot_Init`; callbacks and any host context must remain valid until `EvoBot_Shutdown`.

Adding a `void *context` argument to every callback would make test hosts and non-singleton integrations cleaner, but EvoBot itself is currently a singleton. It is beneficial but not required for ezQuake and should be done only as a deliberate API-breaking cleanup during repository extraction, not piecemeal.

### QuakeWorld-shaped but acceptable as optional capabilities

`EVOBOT_COLLISION_TREE_POINT`/`PLAYER`, clipnode indices, Quake contents, fixed player bounds, and `target`/`targetname` interactors reflect QuakeWorld concepts. They do not leak MVDSV C types, and ezQuake can implement them directly, but a future unrelated engine may not.

Do not redesign them into an abstract universal engine model now. Mark the raw BSP tree and Quake-style interactor snapshot as optional navigation capabilities. A future host may generate equivalent convex source cells through a different callback or may omit target-graph support.

No renderer callbacks belong in `evobot_host_api_t`. Rendering consumes EvoBot state; it is not a service required by core simulation.

### Future command output

Autonomous commands are outside this investigation. Before movement is added, define a portable intent/command type keyed by `evobot_client_handle_t`. The host, not the core, should stamp engine timing and translate it to `usercmd_t`. That preserves the current rule that `msec`, `fixangle`, angle encoding, and `botcmd` lifetime are owned by the QuakeWorld adapter.

## Renderer-independent debug inspection API

The convex implementation currently keeps areas, faces, portals, vertices, and interactors private in `evobot_nav_convex.c`. Preserve that encapsulation. Add a small public `evobot_debug.h` containing plain copied records and index-based query functions.

A suitable first API shape is:

```c
typedef struct evobot_debug_nav_summary_s {
    uint64_t revision;
    uint32_t map_checksum;
    size_t area_count;
    size_t portal_count;
    size_t interactor_count;
} evobot_debug_nav_summary_t;

int EvoBot_DebugNavSummary(evobot_debug_nav_summary_t *summary);
int EvoBot_DebugArea(size_t index, evobot_debug_area_t *area);
int EvoBot_DebugAreaFace(size_t area_index, size_t face_index,
    evobot_debug_face_t *face);
int EvoBot_DebugAreaFaceVertex(size_t area_index, size_t face_index,
    size_t vertex_index, evobot_vec3_t *vertex);
int EvoBot_DebugPortal(size_t index, evobot_debug_portal_t *portal);
int EvoBot_DebugPortalVertex(size_t portal_index, size_t vertex_index,
    evobot_vec3_t *vertex);
int EvoBot_DebugInteractor(size_t index,
    evobot_debug_interactor_t *interactor);
int EvoBot_DebugFindArea(uint32_t area_id, size_t *index);
```

The copied records should expose stable IDs, bounds, contents, support/water flags, face kind, vertex count, portal endpoints, and interactor kind/bounds. A ledge is already a face semantic and need not be duplicated as a renderer primitive. The query API must not expose the internal arrays or allocation ownership.

`revision` increments on map clear, generation, load, and any later topology mutation. A renderer reads the summary, performs its queries, then verifies the revision has not changed. In the current single-threaded local path it will normally remain stable for the whole draw. If EvoBot later generates asynchronously, this can grow into an explicit begin/end snapshot without changing record semantics.

`evobot_nav_show_area <id>` is renderer selection state, not navigation state. The ezQuake debug module should store the requested ID and use `EvoBot_DebugFindArea`; the core need not acquire a mutable "selected area" merely for display. Later core-owned state such as a bot's current area, goal, route, reachability, or predicted trajectory can be added as new read-only record families.

Do not expose colors, line widths, triangle batches, OpenGL handles, or text layout. The host maps semantic fields to presentation.

## ezQuake debug rendering

### Existing mechanisms and limitations

ezQuake selects classic OpenGL with `vid_renderer 0` and modern OpenGL with `vid_renderer 1` when both are built (`src/r_local.h:50-74`). Both are enabled by default in CMake (`CMakeLists.txt:9-18`). Common rendering is dispatched through the `renderer` function table declared by `src/r_renderer.h` and `r_renderer_structure.h`.

The existing apparent helpers are not a ready 3D debug API:

- `GLM_Draw_Line3D` in `src/r_draw_line.c:30-35` is an empty stub.
- `R_Draw_LineRGB`/`Draw_AlphaLineRGB` operate in 2D HUD space.
- `R_Draw_Polygon` in `src/r_draw_polygon.c` is a 2D HUD polygon path.
- classic immediate `GL_LINES` calls exist in the HUD backend, but using them directly would fail the modern renderer requirement.

`R_Project3DCoordinates` is usable for labels (`src/r_matrix.c:381-412`). The auto-ID implementation demonstrates the correct two-stage pattern: project while the 3D matrices are current in `R_Render3DHud`, then draw strings during the 2D screen pass (`src/r_rmain.c:857-867`, `src/hud_autoid.c:99-178`, `src/cl_screen.c:734-805`). The EvoBot module must also reject points behind the camera and outside the desired viewport; projection alone checks only a zero homogeneous divisor.

### Recommended implementation boundary

`evobot_ezq_debug.c` should:

- own all `evobot_nav_show*` cvars and selected-area state;
- query only `evobot_debug.h` and never inspect private core arrays;
- map contents/face/interactor semantics to ezQuake colors;
- triangulate convex faces for optional fill and emit their edges for outlines;
- cache projected area-number labels for the later 2D pass;
- enforce a per-frame vertex/label budget and report truncation only in developer output;
- draw only while `com_serveractive`, `sv.state == ss_active`, the client is rendering the same local map, and the debug revision is valid.

Add a small common renderer primitive interface, implemented by both GLC and GLM, rather than calling OpenGL directly from the EvoBot module. The useful renderer-level operations are colored 3D line lists and colored 3D triangle lists. The records should be ezQuake renderer vertices, not EvoBot public types.

The renderer entry belongs in `src/r_renderer_structure.h`; the existing table-population macros in `src/glc_main.c` and `src/glm_main.c` will then require matching `GLC_*` and `GLM_*` implementations. The associated rendering state, VAO, buffer, and shader declarations belong with the renderer rather than in `evobot_ezq_debug.c`.

For classic OpenGL, the backend can use the existing immediate/vertex-array infrastructure. For modern OpenGL, add a dedicated dynamic VBO/VAO and a simple position/color shader or a narrowly generalized existing simple-3D path. Add rendering states with depth testing enabled, depth writes disabled, and alpha blending for fills. Wireframe should be implemented as line geometry rather than relying on polygon mode, so both backends behave consistently.

The clean common injection point is after `R_DrawEntities` and before `R_Render3DEffects`/`renderer.RenderView` in `R_RenderView`. At that point opaque and translucent scene geometry has been processed, the world matrices are valid, and backend queues have not had their final view flush. If translucent fill ordering proves distracting, ship outlines first and add fill as a separately gated follow-up.

Area numbers should be prepared alongside `SCR_SetupAutoID` in the 3D HUD preparation phase and drawn from `SCR_DrawElements`. Multiview needs per-view projected label caches, as auto-ID already does; the first implementation may explicitly disable EvoBot labels in multiview while retaining 3D geometry.

## Console and cvar architecture

ezQuake and MVDSV both use `Cmd_AddCommand`, so command names and portable command functions can remain consistent. Registration is host-specific:

- each adapter owns the tiny `Cmd_Argc`/`Cmd_Argv` wrappers and calls portable operations;
- the portable core does not include `cmd.h` or know about a console;
- ezQuake local commands check for an active local server and do not silently forward to a remote server;
- the ezQuake debug module registers cvars with `Cvar_SetCurrentGroup`, `Cvar_Register`, and `Cvar_ResetCurrentGroup`.

Keep the existing operational commands in both hosts:

```text
evobot_status
evobot_version
evobot_add <name>
evobot_remove <name>
evobot_nav_generate
evobot_nav_status
evobot_nav_save
evobot_nav_load [map]
evobot_nav_clear
evobot_nav_export_obj
```

Proposed ezQuake-only display cvars are:

```text
evobot_nav_show 0|1
evobot_nav_show_areas 0|1
evobot_nav_show_portals 0|1
evobot_nav_show_ledges 0|1
evobot_nav_show_liquids 0|1
evobot_nav_show_interactors 0|1
evobot_nav_show_numbers 0|1
evobot_nav_show_area <id>       // -1 means no single-area filter
```

The master cvar gates all querying and buffer work. Category cvars are renderer policy and must not be serialized into `.botnav`. Defaults should be off.

## Local development workflow

The ezQuake build is CMake-based. Its documented Windows path is:

```powershell
cd K:\development\EvoBot\ezquake
powershell -File bootstrap.ps1
cmake --preset msbuild-x64
cmake --build --preset msbuild-x64-debug
```

After the future integration, run the same original Quake data and `evosp/spprogs.dat` used by the dedicated-server navigation work:

```powershell
K:\development\EvoBot\ezquake\build-msbuild-x64\Debug\ezquake.exe `
  -basedir K:\development\EvoBot\server `
  -game evosp `
  -progtype 0 `
  +set sv_progsname spprogs `
  +set deathmatch 0 `
  +set coop 1 `
  +set skill 1 `
  +map e1m1
```

The exact executable directory follows the selected CMake preset; the path above is the documented `msbuild-x64` layout. No replacement `spprogs.dat` should be built or installed.

Suggested validation order for the future port is:

1. load E1M1 locally with PR1 `evosp/spprogs.dat` and verify map identity/status;
2. generate, save, clear, and reload version-2 `.botnav` and compare counts/checksum with MVDSV;
3. create/remove a neutral EvoBot and verify normal damage, knockback, death, respawn, map change, and client shutdown;
4. load PR2 KTX and verify an EvoBot and a normal KTX/Frogbot independently, especially the `gamecodeBot` distinction;
5. only then enable wireframe debug rendering in classic and modern OpenGL;
6. compare selected areas, faces, portals, ledges, liquids, numbers, and interactors against OBJ export.

## Remote dedicated-server visualization

Direct queries are valid only for ezQuake's local server. When ezQuake is connected to remote MVDSV there is no shared memory, and client collision/entity state is neither authoritative nor sufficient to reconstruct all EvoBot state.

A later opt-in protocol would need:

- a negotiated EvoBot debug protocol version and server capability advertisement;
- explicit server authorization, disabled by default for public servers;
- map name/checksum, `.botnav` format version, dataset hash, and debug revision;
- chunked, bounded transfer of static area/face/portal/interactor records, with resync and client caching;
- separate small dynamic updates for bot areas, selected routes, reachabilities, interactor state, and trajectories;
- endian-safe numeric encoding, maximum counts/sizes, rate limits, and malformed-data rejection;
- client rendering from decoded owned copies, never server pointers or assumed edict numbers;
- revision invalidation on map change or regeneration.

Static version-2 `.botnav` data could eventually be transferred or downloaded once and cached by checksum. Dynamic debug state needs its own messages. OBJ is not suitable as the protocol because it loses semantic IDs, contents, portal relationships, and interactors.

No part of that protocol is required for the local integration.

## Exact next implementation milestone

The next milestone should be **EZQ-1: canonical core extraction and headless local-server parity**. It should stop before debug rendering and before navigation reachabilities.

Its bounded scope is:

1. Create the standalone EvoBot repository from the active portable core and consume it as a pinned submodule from MVDSV and ezQuake.
2. Apply the small host-API cleanup above: version/size/capabilities, storage contract, and corrected unsupported status. Add the read-only debug inspection header/getters, but no renderer.
3. Keep MVDSV behavior unchanged and re-run its PR1/PR2 fake-client and E1M1 version-2 navigation regression tests.
4. Add `evobot_ezq_adapter.c/.h` with initialization, map load/clear, frame, process shutdown, commands, collision/entity/storage callbacks, and neutral fake-client commands.
5. Port only the required fake-client parity pieces: reusable `SV_AddBotClient`, PR1 context/spawn parms, `gamecodeBot`, correct edict `isBot`, and the blocked-retry guard. Preserve `PF2_Add_Bot` and existing Frogbot behavior.
6. Build ezQuake Debug x64 with both default OpenGL backends and runtime-test `evosp/spprogs.dat` on E1M1 plus PR2 KTX/Frogbot coexistence.

The following milestone can add `evobot_ezq_debug.c/.h` and wireframe areas/portals through the common renderer interface. Filled polygons and projected labels should follow only after the wireframe path works in both classic and modern OpenGL.

No reachability, routing, autonomous input, or AI belongs in either milestone.
