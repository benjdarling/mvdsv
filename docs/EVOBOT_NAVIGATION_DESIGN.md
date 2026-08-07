# EvoBot navigation design

## Scope

This document records the investigation for the first EvoBot navigation milestone. It describes the current MVDSV collision and entity facilities, the navigation-relevant entity set produced by the PR1 single-player game code on E1M1, and a proposed portable representation and generation process.

This milestone does not implement navigation generation, path search, path following, movement, jumping, aiming, combat, or autonomous `usercmd` behavior. BSP leaves are explicitly not navigation regions.

The architectural boundary remains:

- `src/evobot/` owns portable plain-C navigation types and algorithms and includes no MVDSV headers.
- `src/evobot_qw_adapter.c` is the only place that may inspect MVDSV collision models, edicts, VM fields, server globals, or traces.
- No `edict_t`, `model_t`, `cmodel_t`, `mnode_t`, `mleaf_t`, `hull_t`, or `trace_t` pointer crosses into portable code.

## Investigation baseline

The live inventory below was captured at repository commit `1005719` from the current Debug executable with:

```text
K:\development\EvoBot\mvdsv\build\Debug\mvdsv.exe -basedir K:\development\EvoBot\server -game evosp -progtype 0 +set sv_progsname spprogs +set deathmatch 0 +set coop 1 +set skill 1 +sv_crypt_rcon 0 +rcon_password <local-password> +map e1m1
```

The loaded game code was `K:\development\EvoBot\server\evosp\spprogs.dat`. The map reported by `evobot_status` was `e1m1`, with `sv.map_checksum`/EvoBot checksum `523840258`. No client or EvoBot was spawned for the inventory. The server had 195 active edicts after QuakeC spawning.

Entity numbers are useful diagnostic identifiers only. They are not stable `.botnav` identifiers and must not be saved as such.

## MVDSV data and APIs

### Map identity and world bounds

`server_t` provides the current map name, both checksums, world collision model, inline collision models, and entity table in `src/server.h:85-99`:

- `sv.mapname`
- `sv.map_checksum`
- `sv.map_checksum2`
- `sv.worldmodel`
- `sv.models[]`
- `sv.num_edicts`
- `sv.edicts[]`

`CM_LoadMap` loads the BSP and returns the world `cmodel_t` (`src/cmodel.c:1399`; call site `src/sv_init.c:502`). A `cmodel_t` exposes `mins`, `maxs`, `origin`, and collision hulls (`src/cmodel.h:89-93`). Therefore the adapter can obtain world sampling bounds from `sv.worldmodel->mins/maxs`. `SV_ClearWorld` already uses those bounds to build the server area tree (`src/sv_world.c:203-208`).

`CM_CalcChecksum` deliberately excludes the entity lump while calculating `sv.map_checksum` (`src/cmodel.c:1331-1368`). It is a reliable collision-map identity, but it does not detect an external `.ent` override or a changed navigation-relevant entity setup. A persisted navigation file should eventually add a canonical entity/game-code signature rather than relying on the BSP checksum alone.

### BSP collision representation

The collision representation is a set of `hull_t` instances made from clipnodes and planes (`src/cmodel.h:45-51`). `CM_LoadSubmodels` installs the same plane/clipnode arrays and separate headnodes for the world and each inline model (`src/cmodel.c:618-675`). The available primitives are:

- `CM_HullForBox` for an expanded axis-aligned box hull (`src/cmodel.c:134`)
- `CM_HullPointContents` for a point-in-hull contents query (`src/cmodel.c:178`)
- `CM_HullTrace` for a point trace through an already expanded hull (`src/cmodel.c:341`)
- `CM_NumInlineModels` and `CM_InlineModel` for `*N` brush models (`src/cmodel.c:372`, `:1494`)
- `CM_EntityString` for the raw BSP entity text (`src/cmodel.c:377`)

The raw entity text is useful for diagnostics, but the live edict set is authoritative because game code applies skill/mode filtering, renames some classnames, creates helper triggers, links door groups, and may remove or replace entities.

### Player hull and movement constraints

The Quake player hull is:

```text
mins = {-16, -16, -24}
maxs = { 16,  16,  32}
```

These are `player_mins` and `player_maxs` in `src/pmove.c:38-39`. `SV_HullForEntity` selects hull 1 for this 32-unit-wide shape when clipping against a `SOLID_BSP` entity (`src/sv_world.c:56-99`). `PM_PlayerTrace` uses the same hull 1 and the same offset calculation (`src/pmovetst.c:145-203`). Navigation volumes must therefore describe valid **player-origin positions**, not unexpanded world-space voids.

Current grounded movement uses:

- `STEPSIZE 18` (`src/pmove.c:41`)
- minimum walkable floor normal `normal.z >= 0.7` (`src/pmove.h:103` and checks in `src/pmove.c`)
- a one-unit downward player trace to establish ground (`PM_CategorizePosition`, `src/pmove.c:587-640`)

These values form the first generation profile. The saved file should include the hull and relevant movement-profile values so incompatible data can be rejected or regenerated.

### Server traces and support detection

The adapter can use these server facilities without constructing global `pmove` state:

- `SV_Trace(start, mins, maxs, end, type, passedict)` clips a swept box against the world and linked solid entities (`src/sv_world.h:68`; implementation `src/sv_world.c:729`).
- `SV_ClipMoveToEntity` performs the same hull selection and offset work against one edict (`src/sv_world.c:481`). It is currently global in the C file but not declared in `sv_world.h`; using it for a world-only query would require a narrow header declaration during implementation.
- `SV_AreaEdicts` returns broad-phase solid or trigger edicts intersecting bounds (`src/sv_world.c:229`; declaration `src/sv_world.h:81`).
- `SV_LinkEdict` maintains `absmin/absmax` and the area lists whenever an entity moves (`src/sv_world.h:56-59`).

`MOVE_NOMONSTERS` in `SV_Trace` ignores all linked entities except `SOLID_BSP` (`src/sv_world.c:542`). This is appropriate for topology probes that must include doors/platforms but exclude monsters, players, and transient boxes. A world-only probe should use the world edict directly through the same hull-clipping path. A separate full-solid probe may use `MOVE_NORMAL` for validation or diagnostics.

Support classification should reproduce `PM_CategorizePosition`: trace the player hull one unit downward, require a hit and `plane.normal[2] >= MIN_STEP_NORMAL`, and retain the hit entity identity. A hit on entity zero is world support. A hit on a `SOLID_BSP` mover is dynamic support and must split the volume from adjacent world-supported space.

During actual player movement, `AddLinksToPmove` adds nearby `SOLID_BSP`, `SOLID_BBOX`, and `SOLID_SLIDEBOX` entities to the physical entity list (`src/sv_user.c:3455-3514`). `PM_PlayerMove` then moves against that list, and `SV_LinkEdict(..., true)` touches triggers after movement (`src/sv_user.c:3798-3865`). Generated data must model the same static and moving BSP obstructions, while treating monsters and other transient boxes as runtime obstacles rather than baked topology.

### Contents and liquids

World contents values are defined in `src/bspfile.h:144-149`: empty, solid, water, slime, lava, and sky. `SV_PointContents` queries hull 0 of the world only (`src/sv_world.c:442-444`). `PM_PointContents` does the same through the world physical entity (`src/pmovetst.c:61-65`), while `PM_PointContents_AllBSPs` can also inspect BSP physents (`src/pmovetst.c:75-98`).

`PM_CategorizePosition` determines water level at three player-relative heights (`src/pmove.c:646-665`):

- feet: `origin.z + player_mins.z + 1`
- middle: `origin.z + (player_mins.z + player_maxs.z) / 2`
- view region: `origin.z + 22`

The generator should use the same samples and split volumes where the primary contents or water-level classification changes. Water, slime, and lava must remain distinct. Slime and lava are occupiable for collision purposes but carry hazard semantics; they must never be merged into ordinary water or air.

### Static world, brush entities, and triggers

The world edict is installed as `SOLID_BSP`/`MOVETYPE_PUSH` in `src/sv_init.c:568-573`. Inline models are installed in `sv.models[]` from `CM_InlineModel` at `src/sv_init.c:545-550`. A live brush mover therefore provides:

- `classname`, `model`, `modelindex`, `origin`
- `mins/maxs` and maintained `absmin/absmax`
- `solid == SOLID_BSP`, normally `movetype == MOVETYPE_PUSH`
- exact collision through its inline `cmodel_t`

Triggers are linked separately with `solid == SOLID_TRIGGER`. Quake player movement touches them through the server area tree, so their maintained bounds are the correct first representation of activation space. Their bounds include the one-unit link padding; an extractor should keep both origin-relative bounds and absolute bounds and document which is serialized.

### Entity fields and relationships

The adapter can iterate integer entity numbers in `[0, sv.num_edicts)`, call `EDICT_NUM`, and skip `edict.e.free`. Common PR1 fields include `absmin`, `absmax`, `origin`, `movetype`, `solid`, `classname`, `model`, `target`, and `targetname` (`src/progdefs.h:85-148`). Strings must be resolved with `PR_GetEntityString`.

Optional game-code fields such as `map`, `killtarget`, `pos1`, `pos2`, `count`, or mod-specific fields must be obtained by name through `PR_GetEdictFieldValue`; the PR1 implementation is `PR1_GetEdictFieldValue` (`src/pr_edict.c:300`). The PR2 macro routes to the corresponding VM-aware implementation. Missing fields must be handled normally and must not abort extraction.

Target relationships are many-to-many string matches: an entity's `target` selects every live entity whose `targetname` is the same string. Navigation extraction must retain both the raw names and resolved map-local links. It must also retain navigation-relevant intermediary logic such as `trigger_counter`, rather than assuming a button directly opens a door.

## Live E1M1 inventory

### Spawn and exit

| Edict | Runtime classname | Bounds/origin | Relevant data |
|---:|---|---|---|
| 37 | `info_player_start` | origin `(480, -352, 88)` | Generic initial navigation seed |
| 175 | `trigger_changelevel` | abs bounds `(1287, 519, -281)` to `(1337, 569, -127)` | `map=e1m2`; `touch=changelevel_touch()` |

The level exit is identifiable generically by the live classname `trigger_changelevel`. Its optional `map` field supplies the destination. Neither E1M1 coordinates nor the target map name need to be hard-coded. The trigger bounds define the eventual goal contact region; reaching a nearby volume is not sufficient unless a normal player hull movement can actually touch the trigger.

### Doors and buttons

PR1 QuakeC changes both ordinary and secret door classnames to `door`. The live `touch`/`use` functions distinguish ordinary `door_touch`/`door_use` doors from `secret_touch`/`fd_secret_use` doors in this game code. These function names are diagnostic evidence, not a future portable/mod-independent classification API.

| Door edict(s) | Model(s) | Reference absolute bounds | Activation/link data |
|---:|---|---|---|
| 39, 40 | `*1`, `*2` | 39: `(207,511,-1)` to `(257,617,129)`; 40: `(207,537,-1)` to `(257,641,129)` | Automatically linked pair; common owner 39; generated proximity trigger 193 |
| 43 | `*3` | `(-65,511,-241)` to `(65,641,1)` | ordinary door, `targetname=t1`; buttons 44 and 79 target `t1` |
| 45, 46 | `*5`, `*6` | 45: `(63,1775,-209)` to `(169,1825,-79)`; 46: `(89,1775,-209)` to `(193,1825,-79)` | Automatically linked pair; common owner 45; generated proximity trigger 194 |
| 55 | `*8` | `(-1,2391,-97)` to `(321,2641,-79)` | ordinary door, `targetname=t2`; button 56 targets `t2` |
| 63 | `*10` | `(191,2303,15)` to `(209,2433,145)` | secret door, `targetname=t3`; triggers 64 and 67 target `t3` |
| 72 | `*13` | `(751,2431,-113)` to `(769,2529,1)` | shootable secret door; no target link |
| 73 | `*14` | `(511,2239,-145)` to `(577,2257,-47)` | shootable secret door; no target link |
| 74 | `*15` | `(511,1919,-267)` to `(577,2177,-191)` | ordinary door, `targetname=t4`; trigger 75 targets `t4` |
| 76 | `*17` | `(575,2431,-129)` to `(593,2545,1)` | ordinary door, `targetname=t5`; trigger 77 targets `t5` |
| 81 | `*21` | `(1103,991,-273)` to `(1121,1057,-159)` | secret door; no target link |
| 84 | `*23` | `(735,479,79)` to `(753,545,193)` | secret door, `targetname=t8`; trigger 85 targets `t8` |
| 96 | `*29` | `(751,1871,-433)` to `(897,1889,-319)` | ordinary door, `targetname=t10`; reached through counter 95 |
| 101 | `*34` | `(740,1988,-153)` to `(756,2004,-127)` | ordinary door, `targetname=t11`; trigger 97 targets `t11` |
| 102 | `*35` | `(1276,1988,-217)` to `(1292,2004,-191)` | ordinary door, `targetname=t12`; trigger 98 targets `t12` |
| 103 | `*36` | `(1276,2492,-281)` to `(1292,2508,-255)` | ordinary door, `targetname=t13`; trigger 99 targets `t13` |
| 104 | `*37` | `(772,2492,-345)` to `(788,2508,-319)` | ordinary door, `targetname=t14`; trigger 100 targets `t14` |
| 114 | `*38` | `(1087,959,-273)` to `(1105,1089,-159)` | ordinary door, `targetname=t15`; trigger 115 targets `t15` |
| 136 | `*41` | `(-321,2879,-81)` to `(-303,2945,1)` | secret door, `targetname=t18`; triggers 135 and 138 target `t18` |
| 163 | `*43` | `(655,47,47)` to `(721,65,113)` | shootable secret door; no target link |

The six live buttons are:

| Edict | Model | Bounds | Target |
|---:|---|---|---|
| 44 | `*4` | `(-69, 559, 31)` to `(-59, 593, 65)` | `t1` |
| 56 | `*9` | `(-65, 2655, -49)` to `(-31, 2665, -15)` | `t2` |
| 79 | `*19` | `(79, 703, -193)` to `(113, 713, -159)` | `t1` |
| 92 | `*25` | `(1287, 2031, -209)` to `(1297, 2065, -175)` | `t9` |
| 93 | `*26` | `(1215, 2503, -273)` to `(1249, 2513, -239)` | `t9` |
| 94 | `*27` | `(783, 1983, -145)` to `(817, 1993, -111)` | `t9` |

Buttons 92, 93, and 94 do not directly target door 96. They target live `trigger_counter` edict 95 (`targetname=t9`, `count=3`), which targets `t10`, which selects door 96. This is the strongest E1M1 example of why the saved representation must retain the target graph rather than encode a presumed door/button solution.

The target-driven door activation volumes observed at runtime were:

| Edict | Class | Absolute bounds | Target |
|---:|---|---|---|
| 64 | `trigger_once` | `(79,2319,47)` to `(145,2385,65)` | `t3` |
| 67 | `trigger_once` | `(239,2303,47)` to `(257,2433,65)` | `t3` |
| 75 | `trigger_multiple` | `(447,1999,-105)` to `(457,2033,-71)` | `t4` |
| 77 | `trigger_once` | `(767,2599,-81)` to `(897,2617,17)` | `t5` |
| 85 | `trigger_multiple` | `(751,479,-1)` to `(833,545,17)` | `t8` |
| 97 | `trigger_once` | `(783,2367,-105)` to `(897,2385,-23)` | `t11` |
| 98 | `trigger_once` | `(895,1983,-169)` to `(913,2113,-87)` | `t12` |
| 99 | `trigger_once` | `(1167,2111,-233)` to `(1297,2129,-151)` | `t13` |
| 100 | `trigger_once` | `(1151,2383,-297)` to `(1169,2513,-215)` | `t14` |
| 115 | `trigger_once` | `(1247,1119,-265)` to `(1385,1137,-151)` | `t15` |
| 135 | `trigger_multiple` | `(143,3055,-17)` to `(177,3065,17)` | `t18` |
| 138 | `trigger_multiple` | `(-385,2879,-49)` to `(-335,2945,-31)` | `t18` |

Generated proximity trigger 193 spans `(147,451,-9)` to `(317,701,137)` and owns door group 39/40. Generated trigger 194 spans `(3,1715,-217)` to `(253,1885,-71)` and owns door group 45/46.

Some target names also select non-navigation effects, such as the `t3` lights. The extractor should either preserve those nodes as generic logic/effect records or mark them as non-navigation target recipients; it must not silently rewrite a many-recipient target into a one-door relationship.

### Platforms and trains

There are two live `plat` edicts and no live `func_train`/`train` edict:

| Platform | Model | Current absolute bounds | Motion/helper data |
|---:|---|---|---|
| 53 | `*7` | `(-593, 2623, -281)` to `(-495, 2689, -119)` | `pos2=(0,0,-152)`; center trigger 54 bounds `(-568,2648,-121)` to `(-520,2664,41)`, `enemy=53` |
| 82 | `*22` | `(751, 479, -337)` to `(833, 545, -319)` | `pos2=(0,0,-400)`; center trigger 83 bounds `(776,504,-321)` to `(808,520,-311)`, `enemy=82` |

The platform top must be dynamic support, not merged with world-supported volumes at either stop. The representation should retain both stop contact regions, the swept volume, and the platform/helper relationship. No E1M1 train behavior can be validated in this milestone. Later `func_train` extraction should follow only path nodes reachable from that train's own target chain; E1M1's monster patrol `path_corner` entities are not platforms.

### Teleporter

| Edict | Class | Bounds/origin | Link |
|---:|---|---|---|
| 80 | `trigger_teleport` | abs bounds `(1271, 1079, -409)` to `(1353, 1097, -327)` | `target=t6` |
| 87 | `info_teleport_destination` | origin `(-32, 1800, -29)` | `targetname=t6` |

The trigger and destination are resolved through the same generic target/targetname graph. Their spatial proximity is irrelevant. Teleportation will eventually be a traversal, not physical adjacency; this milestone only stores the interactor relationship.

### Monsters

At medium skill (`skill 1`) with coop enabled, 22 live entities had both `FL_MONSTER` and nonzero blocking solidity. All 22 were `SOLID_SLIDEBOX`: 17 `monster_army` and 5 `monster_dog`.

Monsters must not be baked into static navigation volumes. `SV_Trace(..., MOVE_NOMONSTERS, ...)` already excludes them during topology generation while retaining solid BSP movers. For later runtime navigation testing:

1. Giving only the EvoBot `FL_NOTARGET` is a low-impact way to suppress normal monster targeting, but monsters can still physically block paths.
2. A separate deathmatch geometry test will normally cause standard Quake game code to remove monsters, but it is not a faithful single-player interaction test and may change other spawned entities.
3. If deterministic full-route tests require nonblocking monsters, an explicit, opt-in engine test mode could later suppress monster interaction for the EvoBot and restore normal state on disable/map clear. That requires careful physics work and should not be part of generation or normal gameplay.

No option requires a specially edited `spprogs.dat`; none is implemented here.

## Generation approach

### A: direct BSP free-space decomposition

The clipnode trees and planes can theoretically be traversed into convex half-space cells for a chosen collision hull. This has attractive properties: exact plane boundaries, no sampling aliasing, and potentially compact geometry.

It is not the recommended first implementation:

- Collision BSP splits encode collision construction, not navigation decisions.
- The hull represents solid/contents classification; it does not directly enumerate a clean set of bounded free convex cells.
- Support, step height, ledges, liquid level, hazards, doorway state, and moving support still require additional traces and semantic subdivision.
- Inline moving brush models require state-dependent Boolean operations against world free space.
- Coplanar/duplicate splits and unbounded intermediate cells make a robust C implementation substantially more complex.
- BSP hull 0 leaves are primarily tied to point contents and visibility organization. They do not correspond to player-hull configuration space and must not become navigation regions.

BSP planes and leaves may later be used as acceleration data or refinement hints, but never as the persisted region definition.

### B: temporary collision-query sampling and compact merging

The recommended method is a temporary, adaptive regular sample representation evaluated with the real player hull, followed by semantic classification and aggressive validated merging. The temporary grid is a generation workspace only. It is discarded before runtime use or serialization.

Proposed process:

1. Obtain world bounds, map identity, player hull, and movement profile through adapter-owned extraction.
2. Sample player-origin configuration space with stationary and swept player-hull queries. Classify samples as blocked or occupiable.
3. Run separate world-only and world-plus-`SOLID_BSP` probes so dynamic brush occupancy/support can be represented rather than confused with static world geometry.
4. For occupiable samples, reproduce grounded support and liquid-level queries. Retain support entity, support plane, contents, and hazard labels.
5. Refine around any change in occupancy, support, floor height/normal, contents, water level, dynamic obstruction, trigger overlap, or interaction area.
6. Detect boundary geometry between like and unlike classified cells before merging.
7. Merge only cells with compatible classification into large AABBs initially. Validate each proposed merged prism with collision probes so sparse samples cannot bridge thin walls or obstructions.
8. Optionally fit a convex plane set after the AABB implementation is proven. AABBs are a valid first compact volume form even if sloped areas require more boxes.
9. Emit compact volumes, explicit boundaries, and interactors; discard all sample cells.

This approach automatically follows the collision behavior of BSP29/BSP2 maps and current player physics. Its risks are sample resolution, narrow-feature aliasing, and generation cost. Adaptive refinement, conservative merging, exact swept validation, and reproducible generator parameters address those risks without saving a dense voxel map.

For E1M1, initial generation should cover grounded player-origin space and liquid space, plus localized unsupported regions needed to characterize ledges, falls, teleport contacts, and platform motion. It should not densely fill every high air pocket merely to anticipate future flying mods. Capability-specific open-air generation can be added later without changing the volume/boundary contract.

## Subdivision rules

Two neighboring samples may be merged only when all applicable properties are compatible:

- both are occupiable for the same player hull/profile;
- primary contents and water-level class match;
- hazard flags match;
- both have support or both are intentionally unsupported;
- supported samples have the same support category and support source;
- floor normal and height remain within a conservative merge tolerance;
- no intervening hull sweep is blocked;
- no ordinary-step threshold, ledge, gap, or headroom/capability change is crossed;
- no dynamic brush boundary, trigger contact boundary, teleporter, exit, or required interaction area is crossed.

Required splits include:

- supported ground to unsupported air;
- a height transition that cannot be validated as an ordinary 18-unit step;
- a material boundary between air, water, slime, and lava;
- world support to platform/door/other moving support;
- static clearance to a state-dependent dynamic obstruction;
- both faces of a doorway controlled by a moving brush;
- entry/exit surfaces of important triggers;
- button approach/use regions;
- a platform's boarding/contact region at each stop.

Physical contact between volumes creates a boundary record only. It does not create a walk, jump, drop, swim, teleport, lift, rocket-jump, or other traversal. Traversal capability and cost are intentionally deferred.

## Ledges, gaps, and steps

Ledge detection must operate on the supported side of a prospective boundary:

1. Validate the current sample as supported using the normal one-unit ground test.
2. Probe horizontally across the boundary with the full player hull.
3. Probe downward on the far side for support and retain the support height and plane.
4. If continuous collision-validated movement reaches compatible support within ordinary step handling, classify an ordinary portal/step boundary.
5. If support disappears or lies below the ordinary-step range, retain a directed high-side ledge boundary with its edge geometry, outward normal, and observed lower support/drop extent.
6. If supported regions face each other across unsupported space, retain separate facing ledges. Do not spatially merge them and do not infer a jump.

An unsupported sample adjacent to a floor cannot erase the ledge during volume merging. Later steering must be able to inset from the retained ledge edge and treat crossing it as an explicit decision. A lower volume may eventually receive a drop traversal from the high side, while the reverse direction may require a jump or rocket jump; those are not adjacency facts.

## Proposed portable structures

The following is a design sketch, not an API committed by this milestone. Names and layout can change when implemented. All fields are EvoBot-owned C scalars, fixed-width integers, arrays, or indices into EvoBot-owned arrays/string storage.

```c
typedef uint32_t evobot_nav_id_t;

typedef struct evobot_vec3_s {
    float v[3];
} evobot_vec3_t;

typedef struct evobot_plane_s {
    evobot_vec3_t normal;
    float distance;
} evobot_plane_t;

typedef struct evobot_bounds_s {
    evobot_vec3_t mins;
    evobot_vec3_t maxs;
} evobot_bounds_t;

typedef enum evobot_nav_contents_e {
    EVOBOT_NAV_CONTENTS_AIR,
    EVOBOT_NAV_CONTENTS_WATER,
    EVOBOT_NAV_CONTENTS_SLIME,
    EVOBOT_NAV_CONTENTS_LAVA,
    EVOBOT_NAV_CONTENTS_OTHER
} evobot_nav_contents_t;

typedef enum evobot_nav_support_kind_e {
    EVOBOT_NAV_SUPPORT_NONE,
    EVOBOT_NAV_SUPPORT_WORLD,
    EVOBOT_NAV_SUPPORT_INTERACTOR
} evobot_nav_support_kind_t;

typedef struct evobot_nav_support_s {
    evobot_nav_support_kind_t kind;
    evobot_nav_id_t interactor_id;
    evobot_plane_t plane;
} evobot_nav_support_t;

typedef enum evobot_nav_geometry_kind_e {
    EVOBOT_NAV_GEOMETRY_AABB,
    EVOBOT_NAV_GEOMETRY_CONVEX_PLANES
} evobot_nav_geometry_kind_t;

typedef struct evobot_nav_volume_s {
    evobot_nav_id_t id;
    evobot_nav_geometry_kind_t geometry_kind;
    evobot_bounds_t bounds;
    uint32_t first_plane;
    uint32_t plane_count;
    evobot_nav_contents_t contents;
    uint32_t water_level;
    uint32_t flags;
    evobot_nav_support_t support;
    uint32_t first_boundary;
    uint32_t boundary_count;
} evobot_nav_volume_t;
```

The volume is player-origin configuration space. `first_plane/plane_count` select an optional plane array; AABB volumes use only `bounds`. Flags can distinguish supported, unsupported/fall space, hazardous, dynamic, and other later capabilities without importing engine constants.

```c
typedef enum evobot_nav_boundary_kind_e {
    EVOBOT_NAV_BOUNDARY_PORTAL,
    EVOBOT_NAV_BOUNDARY_STEP,
    EVOBOT_NAV_BOUNDARY_LEDGE,
    EVOBOT_NAV_BOUNDARY_LIQUID,
    EVOBOT_NAV_BOUNDARY_DYNAMIC,
    EVOBOT_NAV_BOUNDARY_INTERACTION,
    EVOBOT_NAV_BOUNDARY_BLOCKED
} evobot_nav_boundary_kind_t;

typedef struct evobot_nav_boundary_s {
    evobot_nav_id_t id;
    evobot_nav_id_t volume_a;
    evobot_nav_id_t volume_b;
    evobot_nav_boundary_kind_t kind;
    evobot_plane_t plane;
    uint32_t first_vertex;
    uint32_t vertex_count;
    evobot_nav_id_t interactor_id;
    uint32_t flags;
} evobot_nav_boundary_t;
```

`volume_b` may be invalid for an exterior ledge/blocked boundary. Vertices define the actual portal face or ledge edge. Direction-specific facts such as the high side belong in flags or explicitly named later fields. This structure records spatial geometry and classification only, not reachability.

```c
typedef enum evobot_nav_interactor_kind_e {
    EVOBOT_NAV_INTERACTOR_DOOR,
    EVOBOT_NAV_INTERACTOR_BUTTON,
    EVOBOT_NAV_INTERACTOR_PLATFORM,
    EVOBOT_NAV_INTERACTOR_TRAIN,
    EVOBOT_NAV_INTERACTOR_TELEPORTER,
    EVOBOT_NAV_INTERACTOR_TELEPORT_DESTINATION,
    EVOBOT_NAV_INTERACTOR_LEVEL_EXIT,
    EVOBOT_NAV_INTERACTOR_LOGIC,
    EVOBOT_NAV_INTERACTOR_OTHER
} evobot_nav_interactor_kind_t;

typedef struct evobot_nav_interactor_s {
    evobot_nav_id_t id;
    uint64_t source_key;
    evobot_nav_interactor_kind_t kind;
    evobot_bounds_t bounds;
    evobot_vec3_t origin;
    uint32_t classname_string;
    uint32_t model_string;
    uint32_t target_string;
    uint32_t targetname_string;
    uint32_t first_target_link;
    uint32_t target_link_count;
    uint32_t first_group_member;
    uint32_t group_member_count;
    uint32_t flags;
} evobot_nav_interactor_t;
```

`source_key` should be derived from a canonical map-local descriptor such as classname, inline model identity, targetname, spawn/reference bounds, and a deterministic duplicate ordinal. A transient MVDSV entity number may be held by an adapter-private runtime lookup, but it is neither `source_key` nor serialized data. Strings and link/member lists use indices into portable owned arrays.

Doors and platforms also need portable motion metadata alongside the base interactor: reference pose, stop poses, swept bounds, wait/speed where available, and supported contact faces. Teleporters need resolved destination IDs. Exits need the destination map string. Logic interactors need fields such as counter count. These can be a tagged union or parallel type-specific arrays once the first extractor establishes the exact common subset.

## Dynamic interactor treatment

### Doors

A closed door divides static space with a dynamic boundary linked to the door interactor. Volumes on each side remain spatial records; whether the door currently connects them is state-dependent. Door groups formed through live owner/enemy relationships must be one logical interactor with multiple brush members. The current/reference pose, alternative pose(s), and swept obstruction must be retained.

Secret doors may follow multi-stage motion and may be touch-, use-, shoot-, or target-activated. The first implementation should extract and report them but must not infer an ordinary open-door traversal unless the motion states and activation are validated.

### Buttons and target logic

A button needs its brush bounds, usable face(s), and one or more nearby occupiable approach volumes. The target graph is data, not a solved action sequence. For E1M1, the representation must express both simple links such as button 44 to `t1` door 43 and the three-button `t9 -> trigger_counter -> t10 -> door 96` chain.

Later action planning can decide that a button must be touched, used, or shot. This milestone only preserves enough geometry and logic identity to make that decision possible.

### Platforms and lifts

World-supported volumes and platform-supported volumes must be separate even when coplanar at a stop. Each platform stop has a boarding/alighting contact boundary. The platform's swept space is a dynamic region, and the top surface carries moving-support identity. The generated center trigger is useful evidence of automatic activation but should be grouped under its owning platform rather than treated as an unrelated destination.

### Teleporters and exit

A teleporter trigger and matching destination remain separate interactors connected by target identity. The connection is not a physical boundary and will become a directed teleport traversal later.

Every live `trigger_changelevel` is a candidate level-exit interactor. The goal region is the trigger contact volume; the `map` field is metadata. E1M1 has exactly one such entity, edict 175, leading to `e1m2`.

## Expected `.botnav` structure

The saved format should be versioned, deterministic, and compact. JSON is the interchange format, but JSON parsing/writing is not part of this milestone and no dependency is selected yet.

```json
{
  "format": "evobot.botnav",
  "version": 1,
  "map": {
    "name": "e1m1",
    "checksum": 523840258,
    "entity_signature": "future-canonical-signature"
  },
  "generation": {
    "generator_version": "future-version",
    "player_hull": {
      "mins": [-16, -16, -24],
      "maxs": [16, 16, 32]
    },
    "step_size": 18,
    "minimum_floor_normal_z": 0.7,
    "settings": {}
  },
  "strings": [],
  "planes": [],
  "vertices": [],
  "volumes": [],
  "boundaries": [],
  "interactors": [],
  "target_links": [],
  "groups": []
}
```

The file contains no temporary occupancy cells. Large repeated strings/vertices/planes may be interned as shown. Traversals are intentionally absent from version 1 of this milestone design; adding them later should be a versioned schema extension rather than overloading physical boundaries.

## Unresolved technical questions

1. What initial and refinement spacing captures Quake stairs, thin ledges, narrow doors, and sloped floors conservatively without excessive generation time?
2. Should the first runtime geometry remain AABB-only or fit convex planes after conservative AABB merging is proven?
3. What exact validation samples/sweeps are sufficient to prove a merged prism does not bridge thin or diagonal collision geometry?
4. How should sloped supported space be vertically bounded while remaining player-origin volume rather than a surface mesh?
5. Which movement cvars must participate in the generation profile beyond hull, step size, and minimum floor normal?
6. How should dynamic door/platform states be sampled without causing irreversible QuakeC side effects during generation?
7. How should multi-stage secret-door motion be represented generically?
8. What canonical live-entity signature handles external `.ent` files, game-code changes, skill/mode filtering, and generated helper edicts?
9. Which stable source-key recipe handles multiple otherwise identical point entities without persisting edict numbers?
10. Do any supported maps/mods use entity-based liquid volumes that require more than `SV_PointContents` world contents?
11. How much unsupported air/fall space should E1M1 retain now, and how will later swimming/flying capability-specific volumes be generated without schema churn?
12. Which explicit opt-in monster test mode, if any, can be implemented without changing normal shared player physics or relying on modified game code?
13. How should non-navigation target recipients and mod-specific logic nodes be represented so target fanout is never silently lost?

## Proposed E1M1 implementation sequence

1. Add portable vector, bounds, trace-result, contents, volume, boundary, interactor, and map-container types under `src/evobot/`. Keep serialization and traversal out of the first type patch.
2. Add narrow host queries for world bounds, player hull/profile, world contents, world-only player-hull trace, world-plus-BSP player-hull trace, and navigation-relevant entity snapshots. Translate every MVDSV value in `evobot_qw_adapter.c`.
3. Add `evobot_nav_entities` as a concise diagnostic command backed by the snapshot extraction. Establish the E1M1 regression expectations from this document: 21 door edicts, 6 buttons, 2 platforms, no train, one teleporter pair, one exit, and 22 blocking monsters at the recorded settings.
4. Build the transitive target graph and logical brush groups. Verify simple door links, automatic door groups, both platform helpers, the teleport `t6` pair, and the `t9` three-button counter chain.
5. Implement temporary player-origin occupancy sampling for static world collision. Add adaptive refinement at blocked/free and contents boundaries.
6. Overlay `SOLID_BSP` movers separately. Split dynamic obstruction/support regions and compute platform stop contact regions without treating monsters as topology.
7. Reproduce support, floor normal, step, contents, and water-level classification. Emit explicit ledge and liquid boundaries before any merging.
8. Conservatively merge compatible samples into validated AABB volumes and discard the temporary grid. Add debug statistics and spatial probes before adding file output.
9. Mark teleporter and changelevel contacts and button approach regions in memory. Verify the single E1M1 exit is selected generically by classname and field data.
10. After the in-memory representation is stable, implement deterministic JSON serialization/loading for `<mapname>.botnav` without saving samples.
11. Validate volume coverage and boundaries on E1M1 with a non-moving diagnostic visualization or queries. Stop and correct decomposition errors before implementing any traversal or bot movement.

This sequence preserves PR1 and PR2/KTX fake-client behavior because it adds observation and portable data first. It does not change the neutral stationary command path or give either game code control over EvoBot navigation.
