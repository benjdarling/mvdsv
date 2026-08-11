#include "qwsvdef.h"

#include "evobot_qw_adapter.h"

#include <float.h>
#include <inttypes.h>

static const char *EvoBot_QW_TravelTypeName(evobot_nav_travel_type_t type);
static int EvoBot_QW_FindClientSlot(evobot_client_handle_t handle);

static evobot_host_api_t evobot_qw_host;
static int evobot_qw_initialized;
static int evobot_qw_map_loaded;
static evobot_client_handle_t evobot_qw_client_handles[MAX_CLIENTS];
static evobot_client_handle_t evobot_qw_next_client_handle = 1;
static double evobot_qw_command_msec_remainder;
static int evobot_qw_exec_debug;

#define EVOBOT_QW_ENTITY_CACHE_MAX 1024
typedef struct evobot_qw_entity_cache_s
{
	const evobot_host_interactor_t *interactor;
	int entity_number;
} evobot_qw_entity_cache_t;
static evobot_qw_entity_cache_t
	evobot_qw_entity_cache[EVOBOT_QW_ENTITY_CACHE_MAX];
static size_t evobot_qw_entity_cache_count;

static void EvoBot_QW_Print(const char *message)
{
	Con_Printf("%s", message);
}

static double EvoBot_QW_ServerTime(void)
{
	return sv.time;
}

static double EvoBot_QW_MonotonicTime(void)
{
	return Sys_DoubleTime();
}

static void EvoBot_QW_CopyVector(const vec3_t source, evobot_vec3_t *destination)
{
	destination->v[0] = source[0];
	destination->v[1] = source[1];
	destination->v[2] = source[2];
}

static int EvoBot_QW_WorldBounds(evobot_bounds_t *bounds)
{
	if (!bounds || sv.state != ss_active || !sv.worldmodel)
		return 0;

	EvoBot_QW_CopyVector(sv.worldmodel->mins, &bounds->mins);
	EvoBot_QW_CopyVector(sv.worldmodel->maxs, &bounds->maxs);
	return 1;
}

static int EvoBot_QW_PlayerBounds(evobot_bounds_t *bounds)
{
	extern vec3_t player_mins;
	extern vec3_t player_maxs;

	if (!bounds)
		return 0;
	EvoBot_QW_CopyVector(player_mins, &bounds->mins);
	EvoBot_QW_CopyVector(player_maxs, &bounds->maxs);
	return 1;
}

static int EvoBot_QW_TracePlayerWorld(const evobot_vec3_t *start,
	const evobot_vec3_t *end, evobot_trace_t *result)
{
	extern vec3_t player_mins;
	extern vec3_t player_maxs;
	vec3_t trace_start;
	vec3_t trace_end;
	trace_t trace;

	if (!start || !end || !result || sv.state != ss_active || !sv.worldmodel)
		return 0;

	VectorSet(trace_start, start->v[0], start->v[1], start->v[2]);
	VectorSet(trace_end, end->v[0], end->v[1], end->v[2]);
	trace = SV_ClipMoveToEntity(EDICT_NUM(0), NULL, trace_start,
		player_mins, player_maxs, trace_end);
	memset(result, 0, sizeof(*result));
	result->all_solid = trace.allsolid;
	result->start_solid = trace.startsolid;
	result->fraction = trace.fraction;
	EvoBot_QW_CopyVector(trace.endpos, &result->end);
	result->normal.v[0] = trace.plane.normal[0];
	result->normal.v[1] = trace.plane.normal[1];
	result->normal.v[2] = trace.plane.normal[2];
	result->hit_dynamic = 0;
	return 1;
}

static int EvoBot_QW_TracePlayerSolids(evobot_client_handle_t handle,
	const evobot_vec3_t *start, const evobot_vec3_t *end,
	evobot_trace_t *result)
{
	extern vec3_t player_mins;
	extern vec3_t player_maxs;
	int slot = EvoBot_QW_FindClientSlot(handle);
	vec3_t trace_start;
	vec3_t trace_end;
	trace_t trace;

	if (slot < 0 || !start || !end || !result || sv.state != ss_active ||
		!sv.worldmodel)
		return 0;
	VectorSet(trace_start, start->v[0], start->v[1], start->v[2]);
	VectorSet(trace_end, end->v[0], end->v[1], end->v[2]);
	trace = SV_Trace(trace_start, player_mins, player_maxs, trace_end,
		MOVE_NORMAL, svs.clients[slot].edict);
	memset(result, 0, sizeof(*result));
	result->all_solid = trace.allsolid;
	result->start_solid = trace.startsolid;
	result->fraction = trace.fraction;
	EvoBot_QW_CopyVector(trace.endpos, &result->end);
	result->normal.v[0] = trace.plane.normal[0];
	result->normal.v[1] = trace.plane.normal[1];
	result->normal.v[2] = trace.plane.normal[2];
	result->hit_dynamic = trace.e.ent && trace.e.ent != EDICT_NUM(0) &&
		trace.e.ent->v->solid != SOLID_BSP;
	return 1;
}

static evobot_contents_t EvoBot_QW_PointContents(const evobot_vec3_t *point)
{
	vec3_t position;
	int contents;

	if (!point || sv.state != ss_active || !sv.worldmodel)
		return EVOBOT_CONTENTS_OTHER;
	VectorSet(position, point->v[0], point->v[1], point->v[2]);
	contents = SV_PointContents(position);
	switch (contents)
	{
	case CONTENTS_EMPTY:
		return EVOBOT_CONTENTS_AIR;
	case CONTENTS_SOLID:
		return EVOBOT_CONTENTS_SOLID;
	case CONTENTS_WATER:
		return EVOBOT_CONTENTS_WATER;
	case CONTENTS_SLIME:
		return EVOBOT_CONTENTS_SLIME;
	case CONTENTS_LAVA:
		return EVOBOT_CONTENTS_LAVA;
	default:
		return EVOBOT_CONTENTS_OTHER;
	}
}

static evobot_contents_t EvoBot_QW_MoveContents(int contents)
{
	switch (contents)
	{
	case CONTENTS_EMPTY:
		return EVOBOT_CONTENTS_AIR;
	case CONTENTS_SOLID:
		return EVOBOT_CONTENTS_SOLID;
	case CONTENTS_WATER:
		return EVOBOT_CONTENTS_WATER;
	case CONTENTS_SLIME:
		return EVOBOT_CONTENTS_SLIME;
	case CONTENTS_LAVA:
		return EVOBOT_CONTENTS_LAVA;
	default:
		return EVOBOT_CONTENTS_OTHER;
	}
}

static void EvoBot_QW_SetMoveVars(void)
{
	extern cvar_t pm_airstep;
	extern cvar_t pm_bunnyspeedcap;
	extern cvar_t pm_ktjump;
	extern cvar_t pm_pground;
	extern cvar_t pm_rampjump;
	extern cvar_t pm_slidefix;

	SV_SetMoveVars();
	movevars.bunnyspeedcap = pm_bunnyspeedcap.value;
	movevars.ktjump = pm_ktjump.value;
	movevars.slidefix = pm_slidefix.value != 0;
	movevars.airstep = pm_airstep.value != 0;
	movevars.pground = pm_pground.value != 0;
	movevars.rampjump = (int)pm_rampjump.value;
}

static int EvoBot_QW_PlayerPhysics(evobot_player_physics_t *physics)
{
	if (!physics || sv.state != ss_active || !sv.worldmodel)
		return 0;
	EvoBot_QW_SetMoveVars();
	physics->step_height = PM_STEP_SIZE;
	physics->minimum_ground_normal = MIN_STEP_NORMAL;
	physics->gravity = movevars.gravity;
	physics->maximum_speed = movevars.maxspeed;
	return 1;
}

static int EvoBot_QW_SimulatePlayerMove(
	const evobot_player_move_state_t *state,
	const evobot_player_move_command_t *command,
	evobot_player_move_result_t *result)
{
	playermove_t saved_pmove;
	movevars_t saved_movevars;
	int blocked;

	if (!state || !command || !result || sv.state != ss_active || !sv.worldmodel ||
		command->msec > 255)
		return 0;
	saved_pmove = pmove;
	saved_movevars = movevars;
	memset(&pmove, 0, sizeof(pmove));
	VectorSet(pmove.origin, state->origin.v[0], state->origin.v[1], state->origin.v[2]);
	VectorSet(pmove.velocity, state->velocity.v[0], state->velocity.v[1],
		state->velocity.v[2]);
	VectorSet(pmove.angles, state->angles.v[0], state->angles.v[1],
		state->angles.v[2]);
	pmove.waterjumptime = state->water_jump_time;
	pmove.onground = state->on_ground;
	pmove.waterlevel = state->water_level;
	pmove.jump_held = state->jump_held;
	pmove.jump_msec = state->jump_msec;
	pmove.pm_type = PM_NORMAL;
	pmove.numphysent = 1;
	pmove.physents[0].model = sv.worldmodel;
	pmove.cmd.msec = (byte)command->msec;
	pmove.cmd.forwardmove = command->forward_move;
	pmove.cmd.sidemove = command->side_move;
	pmove.cmd.upmove = command->up_move;
	pmove.cmd.buttons = (byte)((command->buttons & EVOBOT_PLAYER_BUTTON_JUMP) ?
		BUTTON_JUMP : 0);
	if (command->buttons & EVOBOT_PLAYER_BUTTON_ATTACK)
		pmove.cmd.buttons |= BUTTON_ATTACK;
	pmove.cmd.impulse = (byte)command->impulse;
	VectorCopy(pmove.angles, pmove.cmd.angles);
	EvoBot_QW_SetMoveVars();
	blocked = PM_PlayerMove();
	memset(result, 0, sizeof(*result));
	EvoBot_QW_CopyVector(pmove.origin, &result->state.origin);
	EvoBot_QW_CopyVector(pmove.velocity, &result->state.velocity);
	EvoBot_QW_CopyVector(pmove.angles, &result->state.angles);
	result->state.water_jump_time = pmove.waterjumptime;
	result->state.on_ground = pmove.onground;
	result->state.water_level = pmove.waterlevel;
	result->state.jump_held = pmove.jump_held;
	result->state.jump_msec = pmove.jump_msec;
	result->contents = EvoBot_QW_MoveContents(pmove.watertype);
	result->blocked = blocked;
	pmove = saved_pmove;
	movevars = saved_movevars;
	return 1;
}

static int EvoBot_QW_CollisionLeaf(int contents)
{
	switch (contents)
	{
	case CONTENTS_SOLID:
		return EVOBOT_COLLISION_LEAF_SOLID;
	case CONTENTS_EMPTY:
		return EVOBOT_COLLISION_LEAF_AIR;
	case CONTENTS_WATER:
		return EVOBOT_COLLISION_LEAF_WATER;
	case CONTENTS_SLIME:
		return EVOBOT_COLLISION_LEAF_SLIME;
	case CONTENTS_LAVA:
		return EVOBOT_COLLISION_LEAF_LAVA;
	default:
		return EVOBOT_COLLISION_LEAF_OTHER;
	}
}

static hull_t *EvoBot_QW_CollisionHull(evobot_collision_tree_kind_t kind)
{
	if (sv.state != ss_active || !sv.worldmodel)
		return NULL;
	return &sv.worldmodel->hulls[kind == EVOBOT_COLLISION_TREE_PLAYER ? 1 : 0];
}

static int EvoBot_QW_CollisionTree(evobot_collision_tree_kind_t kind,
	evobot_collision_tree_t *tree)
{
	hull_t *hull = EvoBot_QW_CollisionHull(kind);

	if (!hull || !tree)
		return 0;
	tree->root_node = hull->firstclipnode;
	tree->first_node = hull->firstclipnode;
	tree->last_node = hull->lastclipnode;
	return 1;
}

static int EvoBot_QW_CollisionNode(evobot_collision_tree_kind_t kind,
	int node_index, evobot_collision_node_t *result)
{
	hull_t *hull = EvoBot_QW_CollisionHull(kind);
	mclipnode_t *node;
	mplane_t *plane;
	int side;

	if (!hull || !result || node_index < hull->firstclipnode ||
		node_index > hull->lastclipnode)
		return 0;
	node = &hull->clipnodes[node_index];
	plane = &hull->planes[node->planenum];
	EvoBot_QW_CopyVector(plane->normal, &result->normal);
	result->distance = plane->dist;
	for (side = 0; side < 2; side++)
	{
		result->children[side] = node->children[side] < 0 ?
			EvoBot_QW_CollisionLeaf(node->children[side]) : node->children[side];
	}
	return 1;
}

static evobot_interactor_kind_t EvoBot_QW_InteractorKind(const char *classname)
{
	if (!strcmp(classname, "door") || !strcmp(classname, "func_door") ||
		!strcmp(classname, "func_door_secret"))
		return EVOBOT_INTERACTOR_DOOR;
	if (!strcmp(classname, "func_button"))
		return EVOBOT_INTERACTOR_BUTTON;
	if (!strcmp(classname, "plat") || !strcmp(classname, "func_plat"))
		return EVOBOT_INTERACTOR_PLATFORM;
	if (!strcmp(classname, "train") || !strcmp(classname, "func_train"))
		return EVOBOT_INTERACTOR_TRAIN;
	if (!strcmp(classname, "trigger_teleport"))
		return EVOBOT_INTERACTOR_TELEPORTER;
	if (!strcmp(classname, "info_teleport_destination"))
		return EVOBOT_INTERACTOR_TELEPORT_DESTINATION;
	if (!strcmp(classname, "trigger_changelevel"))
		return EVOBOT_INTERACTOR_LEVEL_EXIT;
	if (!strcmp(classname, "trigger_once") || !strcmp(classname, "trigger_multiple") ||
		!strcmp(classname, "trigger_push") || !strcmp(classname, "item_sigil"))
		return EVOBOT_INTERACTOR_TRIGGER;
	if (!strcmp(classname, "trigger_counter") || !strcmp(classname, "trigger_relay") ||
		!strcmp(classname, "event_lightning") ||
		!strcmp(classname, "monster_boss"))
		return EVOBOT_INTERACTOR_LOGIC;
	if (!strcmp(classname, "item_key1") || !strcmp(classname, "item_key2"))
		return EVOBOT_INTERACTOR_ITEM;
	return EVOBOT_INTERACTOR_OTHER;
}

static int EvoBot_QW_InteractorEntityMatches(edict_t *entity,
	const evobot_host_interactor_t *interactor)
{
	const char *classname;
	const char *model;

	if (!entity || entity->e.free || !entity->v->classname || !interactor)
		return 0;
	classname = PR_GetEntityString(entity->v->classname);
	model = PR_GetEntityString(entity->v->model);
	return EvoBot_QW_InteractorKind(classname) == interactor->kind &&
		interactor->model[0] && !strcmp(model, interactor->model);
}

static edict_t *EvoBot_QW_FindInteractorEntity(
	const evobot_host_interactor_t *interactor)
{
	size_t cache_index;
	int i;

	for (cache_index = 0; cache_index < evobot_qw_entity_cache_count;
		cache_index++)
		if (evobot_qw_entity_cache[cache_index].interactor == interactor)
		{
			i = evobot_qw_entity_cache[cache_index].entity_number;
			if (i >= 0 && i < sv.num_edicts &&
				EvoBot_QW_InteractorEntityMatches(EDICT_NUM(i), interactor))
				return EDICT_NUM(i);
			break;
		}
	for (i = 0; i < sv.num_edicts; i++)
		if (EvoBot_QW_InteractorEntityMatches(EDICT_NUM(i), interactor))
		{
			if (cache_index == evobot_qw_entity_cache_count &&
				evobot_qw_entity_cache_count < EVOBOT_QW_ENTITY_CACHE_MAX)
				evobot_qw_entity_cache_count++;
			if (cache_index < EVOBOT_QW_ENTITY_CACHE_MAX)
			{
				evobot_qw_entity_cache[cache_index].interactor = interactor;
				evobot_qw_entity_cache[cache_index].entity_number = i;
			}
			return EDICT_NUM(i);
		}
	return NULL;
}

static int EvoBot_QW_IsNavigationInteractor(edict_t *entity,
	evobot_interactor_kind_t *kind)
{
	const char *classname;

	if (!entity || entity->e.free || !entity->v->classname)
		return 0;
	classname = PR_GetEntityString(entity->v->classname);
	*kind = EvoBot_QW_InteractorKind(classname);
	/* Vanilla Quake's NOTOUCH spawnflag turns trigger_once/trigger_multiple
	 * brushes into externally fired relays.  They are state-graph nodes, not
	 * places the player can activate by walking into their usually tiny model. */
	if (*kind == EVOBOT_INTERACTOR_TRIGGER &&
		(!strcmp(classname, "trigger_once") ||
		 !strcmp(classname, "trigger_multiple")) &&
		((int)entity->v->spawnflags & 1))
		*kind = EVOBOT_INTERACTOR_LOGIC;
	if (*kind == EVOBOT_INTERACTOR_OTHER)
		return 0;
	if (*kind == EVOBOT_INTERACTOR_LOGIC &&
		!PR_GetEntityString(entity->v->target)[0] &&
		!PR_GetEntityString(entity->v->targetname)[0])
		return 0;
	return 1;
}

static int EvoBot_QW_InteractorCount(void)
{
	int count = 0;
	int i;

	if (sv.state != ss_active)
		return 0;
	for (i = 0; i < sv.num_edicts; i++)
	{
		evobot_interactor_kind_t kind;

		if (EvoBot_QW_IsNavigationInteractor(EDICT_NUM(i), &kind))
			count++;
	}
	return count;
}

static void EvoBot_QW_ExpandSweptBounds(evobot_host_interactor_t *interactor,
	const float *position)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float delta = position[axis] - interactor->origin.v[axis];
		float minimum = interactor->bounds.mins.v[axis] + delta;
		float maximum = interactor->bounds.maxs.v[axis] + delta;

		if (minimum < interactor->swept_bounds.mins.v[axis])
			interactor->swept_bounds.mins.v[axis] = minimum;
		if (maximum > interactor->swept_bounds.maxs.v[axis])
			interactor->swept_bounds.maxs.v[axis] = maximum;
	}
}

static void EvoBot_QW_CopyOptionalString(edict_t *entity, const char *field,
	char *destination, size_t destination_size)
{
	eval_t *value = PR_GetEdictFieldValue(entity, (char *)field);

	if (!value)
	{
		destination[0] = '\0';
		return;
	}
	strlcpy(destination, PR_GetEntityString(value->string), destination_size);
}

static float EvoBot_QW_OptionalFloat(edict_t *entity, const char *field,
	float fallback)
{
	eval_t *value = PR_GetEdictFieldValue(entity, (char *)field);

	return value ? value->_float : fallback;
}

static edict_t *EvoBot_QW_FindPathCorner(const char *targetname)
{
	int i;

	if (!targetname || !targetname[0])
		return NULL;
	for (i = 0; i < sv.num_edicts; i++)
	{
		edict_t *entity = EDICT_NUM(i);
		const char *classname;

		if (!entity || entity->e.free || !entity->v->classname)
			continue;
		classname = PR_GetEntityString(entity->v->classname);
		if (strcmp(classname, "path_corner") && strcmp(classname, "path_corner_train"))
			continue;
		if (!strcmp(PR_GetEntityString(entity->v->targetname), targetname))
			return entity;
	}
	return NULL;
}

static void EvoBot_QW_AddMoverStop(evobot_host_interactor_t *interactor,
	const evobot_bounds_t *bounds)
{
	int axis;

	if (interactor->movement_stop_count >= EVOBOT_NAV_MOVER_STOPS_MAX)
		return;
	interactor->movement_stop_bounds[interactor->movement_stop_count++] = *bounds;
	for (axis = 0; axis < 3; axis++)
	{
		if (bounds->mins.v[axis] < interactor->swept_bounds.mins.v[axis])
			interactor->swept_bounds.mins.v[axis] = bounds->mins.v[axis];
		if (bounds->maxs.v[axis] > interactor->swept_bounds.maxs.v[axis])
			interactor->swept_bounds.maxs.v[axis] = bounds->maxs.v[axis];
	}
}

/* Vanilla func_door_secret renames itself to "door" during spawn and does not
 * populate pos1/pos2 until it is used.  Reconstruct its two-stage movement from
 * the same mangle/size/spawnflag formula as QuakeC so navigation generation sees
 * the real swept path before the first shot or external activation. */
static int EvoBot_QW_SecretDoorStops(edict_t *entity,
	evobot_host_interactor_t *interactor)
{
	eval_t *mangle;
	vec3_t forward;
	vec3_t right;
	vec3_t up;
	vec3_t destination1;
	vec3_t destination2;
	vec3_t width_axis;
	evobot_bounds_t intermediate_bounds;
	float endpoint_distance = 0;
	float width;
	float length;
	float direction;
	int axis;

	if (!entity || !interactor || interactor->kind != EVOBOT_INTERACTOR_DOOR ||
		fabsf(interactor->speed - 50.0f) > 0.1f)
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		float delta = interactor->endpoint_b.v[axis] -
			interactor->endpoint_a.v[axis];
		endpoint_distance += delta * delta;
	}
	if (endpoint_distance > 1.0f)
		return 0;
	mangle = PR_GetEdictFieldValue(entity, "mangle");
	if (!mangle)
		return 0;
	AngleVectors(mangle->vector, forward, right, up);
	width = EvoBot_QW_OptionalFloat(entity, "t_width", 0);
	length = EvoBot_QW_OptionalFloat(entity, "t_length", 0);
	if (width <= 0)
	{
		if (interactor->spawnflags & 4)
			VectorCopy(up, width_axis);
		else
			VectorCopy(right, width_axis);
		width = fabsf(DotProduct(width_axis, entity->v->size));
	}
	if (length <= 0)
		length = fabsf(DotProduct(forward, entity->v->size));
	if (width <= 0 || length <= 0)
		return 0;
	direction = 1.0f - (float)(interactor->spawnflags & 2);
	for (axis = 0; axis < 3; axis++)
	{
		destination1[axis] = interactor->origin.v[axis] +
			((interactor->spawnflags & 4) ? -up[axis] * width :
			 right[axis] * width * direction);
		destination2[axis] = destination1[axis] + forward[axis] * length;
		interactor->endpoint_a.v[axis] = interactor->origin.v[axis];
		interactor->endpoint_b.v[axis] = destination2[axis];
		intermediate_bounds.mins.v[axis] = interactor->bounds.mins.v[axis] +
			destination1[axis] - interactor->origin.v[axis];
		intermediate_bounds.maxs.v[axis] = interactor->bounds.maxs.v[axis] +
			destination1[axis] - interactor->origin.v[axis];
		interactor->endpoint_a_bounds.mins.v[axis] =
			interactor->bounds.mins.v[axis];
		interactor->endpoint_a_bounds.maxs.v[axis] =
			interactor->bounds.maxs.v[axis];
		interactor->endpoint_b_bounds.mins.v[axis] =
			interactor->bounds.mins.v[axis] + destination2[axis] -
			interactor->origin.v[axis];
		interactor->endpoint_b_bounds.maxs.v[axis] =
			interactor->bounds.maxs.v[axis] + destination2[axis] -
			interactor->origin.v[axis];
	}
	interactor->swept_bounds = interactor->bounds;
	interactor->movement_stop_count = 0;
	EvoBot_QW_AddMoverStop(interactor, &interactor->endpoint_a_bounds);
	EvoBot_QW_AddMoverStop(interactor, &intermediate_bounds);
	EvoBot_QW_AddMoverStop(interactor, &interactor->endpoint_b_bounds);
	interactor->has_movement = 1;
	interactor->travel_time = (width + length) / interactor->speed + 1.0f;
	return 1;
}

static void EvoBot_QW_TrainStops(edict_t *train,
	evobot_host_interactor_t *interactor)
{
	char next_target[EVOBOT_NAV_TARGET_MAX];
	edict_t *visited[EVOBOT_NAV_MOVER_STOPS_MAX];
	int visited_count = 0;
	int guard;

	EvoBot_QW_AddMoverStop(interactor, &interactor->bounds);
	strlcpy(next_target, interactor->target, sizeof(next_target));
	for (guard = 0; guard < EVOBOT_NAV_MOVER_STOPS_MAX - 1 && next_target[0]; guard++)
	{
		edict_t *corner = EvoBot_QW_FindPathCorner(next_target);
		evobot_bounds_t stop;
		int repeated = 0;
		int axis;
		int previous;

		if (!corner)
			break;
		for (previous = 0; previous < visited_count; previous++)
			if (visited[previous] == corner)
			{
				repeated = 1;
				break;
			}
		if (!repeated)
			visited[visited_count++] = corner;
		stop = interactor->bounds;
		for (axis = 0; axis < 3; axis++)
		{
			float delta = corner->v->origin[axis] - interactor->bounds.mins.v[axis];
			stop.mins.v[axis] += delta;
			stop.maxs.v[axis] += delta;
		}
		EvoBot_QW_AddMoverStop(interactor, &stop);
		strlcpy(next_target, PR_GetEntityString(corner->v->target),
			sizeof(next_target));
		if (repeated)
			break;
	}
	interactor->has_movement = interactor->movement_stop_count > 1;
	if (interactor->has_movement)
	{
		interactor->endpoint_a_bounds = interactor->movement_stop_bounds[0];
		interactor->endpoint_b_bounds =
			interactor->movement_stop_bounds[interactor->movement_stop_count - 1];
	}
	(void)train;
}

static int EvoBot_QW_GetInteractor(int index, evobot_host_interactor_t *interactor)
{
	int found = 0;
	int i;

	if (index < 0 || !interactor || sv.state != ss_active)
		return 0;
	for (i = 0; i < sv.num_edicts; i++)
	{
		edict_t *entity = EDICT_NUM(i);
		evobot_interactor_kind_t kind;
		const char *classname;

		if (!EvoBot_QW_IsNavigationInteractor(entity, &kind))
			continue;
		if (found++ != index)
			continue;

		memset(interactor, 0, sizeof(*interactor));
		interactor->kind = kind;
		interactor->dynamic_brush = (kind == EVOBOT_INTERACTOR_DOOR ||
			kind == EVOBOT_INTERACTOR_BUTTON || kind == EVOBOT_INTERACTOR_PLATFORM ||
			kind == EVOBOT_INTERACTOR_TRAIN) && entity->v->solid == SOLID_BSP;
		EvoBot_QW_CopyVector(entity->v->absmin, &interactor->bounds.mins);
		EvoBot_QW_CopyVector(entity->v->absmax, &interactor->bounds.maxs);
		interactor->swept_bounds = interactor->bounds;
		EvoBot_QW_CopyVector(entity->v->origin, &interactor->origin);
		if (kind == EVOBOT_INTERACTOR_TELEPORT_DESTINATION)
		{
			eval_t *mangle = PR_GetEdictFieldValue(entity, "mangle");
			int axis;

			if (mangle)
			{
				EvoBot_QW_CopyVector(mangle->vector, &interactor->angles);
				interactor->has_angles = 1;
			}
			else
			{
				EvoBot_QW_CopyVector(entity->v->angles, &interactor->angles);
				for (axis = 0; axis < 3; axis++)
					if (entity->v->angles[axis] != 0)
						interactor->has_angles = 1;
			}
			EvoBot_QW_CopyVector(entity->v->velocity, &interactor->velocity);
			for (axis = 0; axis < 3; axis++)
				if (entity->v->velocity[axis] != 0)
					interactor->has_velocity = 1;
		}
		classname = PR_GetEntityString(entity->v->classname);
		strlcpy(interactor->classname, classname, sizeof(interactor->classname));
		strlcpy(interactor->model, PR_GetEntityString(entity->v->model),
			sizeof(interactor->model));
		strlcpy(interactor->target, PR_GetEntityString(entity->v->target),
			sizeof(interactor->target));
		strlcpy(interactor->targetname, PR_GetEntityString(entity->v->targetname),
			sizeof(interactor->targetname));
		if (!strcmp(classname, "event_lightning"))
		{
			int boss_index;
			/* Vanilla lightning_use locates monster_boss by classname rather
			 * than by a target edge.  Export that deterministic QC relationship
			 * using the boss's real targetname so the portable activation graph
			 * can represent it without a map/entity-id exception. */
			for (boss_index = 0; boss_index < sv.num_edicts; boss_index++)
			{
				edict_t *boss = EDICT_NUM(boss_index);
				if (!boss->e.free && boss->v->classname &&
					!strcmp(PR_GetEntityString(boss->v->classname), "monster_boss"))
				{
					strlcpy(interactor->target,
						PR_GetEntityString(boss->v->targetname),
						sizeof(interactor->target));
					break;
				}
			}
		}
		EvoBot_QW_CopyOptionalString(entity, "killtarget", interactor->killtarget,
			sizeof(interactor->killtarget));
		EvoBot_QW_CopyOptionalString(entity, "map", interactor->destination_map,
			sizeof(interactor->destination_map));
		interactor->spawnflags = (int)entity->v->spawnflags;
		interactor->health = entity->v->health;
		/* Quake assigns ordinary, non-shootable doors a sentinel health of
		 * 10000 while leaving takedamage disabled.  Positive health (including
		 * max_health in some gamecode) therefore does not mean damage is an
		 * activation mechanism; the engine's live takedamage flag does. */
		{
			int damage_activates = entity->v->takedamage != DAMAGE_NO;
		interactor->speed = EvoBot_QW_OptionalFloat(entity, "speed", 0);
		interactor->wait = EvoBot_QW_OptionalFloat(entity, "wait", 0);
		interactor->activation_required_count =
			(int)EvoBot_QW_OptionalFloat(entity, "count", 0);
		if (kind == EVOBOT_INTERACTOR_ITEM)
		{
			interactor->inventory_grants = (uint32_t)entity->v->items &
				(uint32_t)(IT_KEY1 | IT_KEY2);
			interactor->pickup_persistent = coop.value != 0;
		}
		else if (kind == EVOBOT_INTERACTOR_DOOR)
		{
			interactor->inventory_requires = (uint32_t)entity->v->items &
				(uint32_t)(IT_KEY1 | IT_KEY2);
			interactor->inventory_consumes = interactor->inventory_requires;
		}
		if (!strcmp(classname, "trigger_push"))
		{
			eval_t *movedir = PR_GetEdictFieldValue(entity, "movedir");
			int axis;

			if (movedir)
			{
				for (axis = 0; axis < 3; axis++)
					interactor->velocity.v[axis] = movedir->vector[axis] *
						interactor->speed * 10.0f;
				interactor->has_velocity = 1;
			}
		}
		if (!strcmp(classname, "trigger_counter") &&
			interactor->activation_required_count <= 0)
			interactor->activation_required_count = 2;
		if (!strcmp(classname, "monster_boss"))
			interactor->activation_required_count = skill.value == 0 ? 1 : 3;
		interactor->endpoint_a = interactor->origin;
		interactor->endpoint_b = interactor->origin;
		interactor->endpoint_a_bounds = interactor->bounds;
		interactor->endpoint_b_bounds = interactor->bounds;
		if (kind == EVOBOT_INTERACTOR_DOOR)
			interactor->activation = interactor->inventory_requires ?
				EVOBOT_ACTIVATION_TOUCH : damage_activates ?
				EVOBOT_ACTIVATION_SHOOT : interactor->targetname[0] ?
				EVOBOT_ACTIVATION_EXTERNAL : EVOBOT_ACTIVATION_APPROACH;
		else if (kind == EVOBOT_INTERACTOR_BUTTON)
			interactor->activation = damage_activates ?
				EVOBOT_ACTIVATION_SHOOT : EVOBOT_ACTIVATION_TOUCH;
		else if (kind == EVOBOT_INTERACTOR_TRIGGER)
			/* Quake maps also use damageable trigger brushes as recessed shoot
			 * switches.  Their health is the same engine-level activation contract as
			 * a shootable button or door; treating every SOLID_TRIGGER as touch-only
			 * sends navigation toward an intentionally unreachable brush volume. */
			interactor->activation = damage_activates ?
				EVOBOT_ACTIVATION_SHOOT : EVOBOT_ACTIVATION_TOUCH;
		else if (kind == EVOBOT_INTERACTOR_PLATFORM)
			interactor->activation = interactor->targetname[0] ?
				EVOBOT_ACTIVATION_EXTERNAL : EVOBOT_ACTIVATION_TOUCH;
		else if (kind == EVOBOT_INTERACTOR_TRAIN)
			interactor->activation = interactor->targetname[0] ?
				EVOBOT_ACTIVATION_EXTERNAL : EVOBOT_ACTIVATION_APPROACH;
		else if (kind == EVOBOT_INTERACTOR_LOGIC)
			interactor->activation = EVOBOT_ACTIVATION_EXTERNAL;
		else if (kind == EVOBOT_INTERACTOR_ITEM)
			interactor->activation = EVOBOT_ACTIVATION_TOUCH;
		else
			interactor->activation = EVOBOT_ACTIVATION_NONE;
		}
		interactor->lifetime = !strcmp(classname, "trigger_once") ||
			!strcmp(classname, "item_sigil") ||
			(kind == EVOBOT_INTERACTOR_DOOR &&
			 (interactor->spawnflags & 32)) ||
			interactor->wait < 0 ? EVOBOT_INTERACTOR_PERSISTENT :
			EVOBOT_INTERACTOR_TEMPORARY;

		if (interactor->dynamic_brush)
		{
			eval_t *pos1 = PR_GetEdictFieldValue(entity, "pos1");
			eval_t *pos2 = PR_GetEdictFieldValue(entity, "pos2");

			if (pos1)
			{
				EvoBot_QW_CopyVector(pos1->vector, &interactor->endpoint_a);
				EvoBot_QW_ExpandSweptBounds(interactor, pos1->vector);
			}
			if (pos2)
			{
				EvoBot_QW_CopyVector(pos2->vector, &interactor->endpoint_b);
				EvoBot_QW_ExpandSweptBounds(interactor, pos2->vector);
			}
			interactor->has_movement = pos1 && pos2;
			if (interactor->has_movement)
			{
				float distance = 0;
				int axis;

				for (axis = 0; axis < 3; axis++)
				{
					float a_delta = interactor->endpoint_a.v[axis] -
						interactor->origin.v[axis];
					float b_delta = interactor->endpoint_b.v[axis] -
						interactor->origin.v[axis];
					float movement = interactor->endpoint_b.v[axis] -
						interactor->endpoint_a.v[axis];
					interactor->endpoint_a_bounds.mins.v[axis] += a_delta;
					interactor->endpoint_a_bounds.maxs.v[axis] += a_delta;
					interactor->endpoint_b_bounds.mins.v[axis] += b_delta;
					interactor->endpoint_b_bounds.maxs.v[axis] += b_delta;
					distance += movement * movement;
				}
				interactor->travel_time = interactor->speed > 0 ?
					sqrtf(distance) / interactor->speed : 0;
			}
			EvoBot_QW_SecretDoorStops(entity, interactor);
		}
		if (kind == EVOBOT_INTERACTOR_PLATFORM && interactor->has_movement)
		{
			EvoBot_QW_AddMoverStop(interactor, &interactor->endpoint_a_bounds);
			EvoBot_QW_AddMoverStop(interactor, &interactor->endpoint_b_bounds);
		}
		else if (kind == EVOBOT_INTERACTOR_TRAIN)
			EvoBot_QW_TrainStops(entity, interactor);
		return 1;
	}

	return 0;
}

static int EvoBot_QW_BoundsIntersect(const evobot_bounds_t *a,
	const evobot_bounds_t *b)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (a->maxs.v[axis] < b->mins.v[axis] ||
			a->mins.v[axis] > b->maxs.v[axis])
			return 0;
	}
	return 1;
}

static evobot_dynamic_blocker_state_t EvoBot_QW_DynamicBlockerState(
	const evobot_host_interactor_t *interactor,
	const evobot_bounds_t *crossing_bounds)
{
	extern vec3_t player_mins;
	extern vec3_t player_maxs;
	edict_t *entity;

	if (!interactor || !crossing_bounds || sv.state != ss_active ||
		!interactor->dynamic_brush)
		return EVOBOT_DYNAMIC_BLOCKER_UNKNOWN;
	/* Doors, plats, and trains all block only at their live brush transform.
	 * Navigation attribution uses their complete swept paths; compare the
	 * crossing against the matched edict's current abs bounds so vacated parts
	 * of that path remain traversable before the mover is activated. */
	entity = EvoBot_QW_FindInteractorEntity(interactor);
	if (entity)
	{
		evobot_bounds_t blocking_bounds;
		int axis;

		if (entity->v->solid != SOLID_BSP)
			return EVOBOT_DYNAMIC_BLOCKER_CLEAR;
		for (axis = 0; axis < 3; axis++)
		{
			blocking_bounds.mins.v[axis] = entity->v->absmin[axis] -
				player_maxs[axis];
			blocking_bounds.maxs.v[axis] = entity->v->absmax[axis] -
				player_mins[axis];
		}
		return EvoBot_QW_BoundsIntersect(&blocking_bounds, crossing_bounds) ?
			EVOBOT_DYNAMIC_BLOCKER_BLOCKED : EVOBOT_DYNAMIC_BLOCKER_CLEAR;
	}
	return EVOBOT_DYNAMIC_BLOCKER_UNKNOWN;
}

static int EvoBot_QW_InteractorState(const evobot_host_interactor_t *interactor,
	evobot_host_interactor_state_t *state)
{
	edict_t *entity;

	if (!interactor || !state || sv.state != ss_active)
		return 0;
	entity = EvoBot_QW_FindInteractorEntity(interactor);
	if (entity)
	{
		int moving = 0;
		int at_a = 1;
		int at_b = 1;
		int axis;

		memset(state, 0, sizeof(*state));
		EvoBot_QW_CopyVector(entity->v->origin, &state->origin);
		EvoBot_QW_CopyVector(entity->v->absmin, &state->bounds.mins);
		EvoBot_QW_CopyVector(entity->v->absmax, &state->bounds.maxs);
		for (axis = 0; axis < 3; axis++)
		{
			if (fabsf(entity->v->velocity[axis]) > 0.1f) moving = 1;
			if (fabsf(entity->v->origin[axis] - interactor->endpoint_a.v[axis]) > 1.0f)
				at_a = 0;
			if (fabsf(entity->v->origin[axis] - interactor->endpoint_b.v[axis]) > 1.0f)
				at_b = 0;
		}
		state->state = (entity->v->solid == SOLID_NOT ||
			((interactor->kind == EVOBOT_INTERACTOR_TRIGGER ||
			  interactor->kind == EVOBOT_INTERACTOR_ITEM) &&
			 entity->v->solid != SOLID_TRIGGER)) ?
			EVOBOT_INTERACTOR_STATE_DISABLED :
			moving ? EVOBOT_INTERACTOR_STATE_MOVING :
			at_a ? EVOBOT_INTERACTOR_STATE_AT_ENDPOINT_A :
			at_b ? EVOBOT_INTERACTOR_STATE_AT_ENDPOINT_B :
			EVOBOT_INTERACTOR_STATE_UNKNOWN;
		return 1;
	}
	/* Brush-model identities are unique within a BSP.  If a captured brush
	 * interactor no longer has a live edict, QC consumed or destroyed it (for
	 * example trigger_once after use); expose that persistent fact instead of
	 * reverting it to UNKNOWN on every plan rebuild. */
	if (interactor->model[0] == '*')
	{
		memset(state, 0, sizeof(*state));
		state->state = EVOBOT_INTERACTOR_STATE_DISABLED;
		state->origin = interactor->origin;
		state->bounds = interactor->bounds;
		return 1;
	}
	return 0;
}

static int EvoBot_QW_FileSize(const char *path, size_t *size)
{
	vfsfile_t *file;
	unsigned long length;

	if (!path || !size)
		return 0;
	file = FS_OpenVFS(path, "rb", FS_GAME_OS);
	if (!file)
		return 0;
	length = VFS_GETLEN(file);
	VFS_CLOSE(file);
	*size = (size_t)length;
	return 1;
}

static int EvoBot_QW_ReadFile(const char *path, void *data, size_t size)
{
	vfsfile_t *file;
	byte *destination = (byte *)data;
	size_t total = 0;

	if (!path || (!data && size))
		return 0;
	file = FS_OpenVFS(path, "rb", FS_GAME_OS);
	if (!file)
		return 0;
	while (total < size)
	{
		int request = (int)min(size - total, (size_t)0x7fffffff);
		int received = VFS_READ(file, destination + total, request, NULL);

		if (received <= 0)
			break;
		total += (size_t)received;
	}
	VFS_CLOSE(file);
	return total == size;
}

static int EvoBot_QW_WriteFile(const char *path, const void *data, size_t size)
{
	vfsfile_t *file;
	const byte *source = (const byte *)data;
	size_t total = 0;

	if (!path || (!data && size))
		return 0;
	file = FS_OpenVFS(path, "wb", FS_GAME_OS);
	if (!file)
		return 0;
	while (total < size)
	{
		int request = (int)min(size - total, (size_t)0x7fffffff);
		int written = VFS_WRITE(file, source + total, request);

		if (written <= 0)
			break;
		total += (size_t)written;
	}
	VFS_CLOSE(file);
	return total == size;
}

static int EvoBot_QW_FindClientSlot(evobot_client_handle_t handle)
{
	int i;

	if (handle == EVOBOT_CLIENT_HANDLE_INVALID)
		return -1;

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		if (evobot_qw_client_handles[i] == handle)
			return i;
	}

	return -1;
}

static evobot_client_handle_t EvoBot_QW_NewClientHandle(void)
{
	evobot_client_handle_t handle;

	do
	{
		handle = evobot_qw_next_client_handle++;
		if (evobot_qw_next_client_handle == EVOBOT_CLIENT_HANDLE_INVALID)
			evobot_qw_next_client_handle++;
	}
	while (handle == EVOBOT_CLIENT_HANDLE_INVALID || EvoBot_QW_FindClientSlot(handle) >= 0);

	return handle;
}

static int EvoBot_QW_IsBotClientValid(evobot_client_handle_t handle)
{
#ifdef USE_PR2
	client_t *client;
	int slot = EvoBot_QW_FindClientSlot(handle);

	if (slot < 0)
		return 0;

	client = &svs.clients[slot];
	if (client->state == cs_free || !client->isBot || client->edict != EDICT_NUM(slot + 1))
	{
		evobot_qw_client_handles[slot] = EVOBOT_CLIENT_HANDLE_INVALID;
		return 0;
	}

	return 1;
#else
	(void)handle;
	return 0;
#endif
}

static int EvoBot_QW_SetTestIsolation(evobot_client_handle_t handle, int enabled)
{
#ifdef USE_PR2
	int slot = EvoBot_QW_FindClientSlot(handle);
	client_t *client;
	if (slot < 0 || !EvoBot_QW_IsBotClientValid(handle))
		return 0;
	client = &svs.clients[slot];
	if (enabled)
		client->edict->v->flags = (float)((int)client->edict->v->flags | FL_NOTARGET);
	else
		client->edict->v->flags = (float)((int)client->edict->v->flags & ~FL_NOTARGET);
	return 1;
#else
	(void)handle;
	(void)enabled;
	return 0;
#endif
}

static evobot_create_bot_result_t EvoBot_QW_CreateBotClient(const char *name)
{
	evobot_create_bot_result_t result;
	int edictnum;
	int slot;

	result.status = EVOBOT_CREATE_BOT_UNAVAILABLE;
	result.handle = EVOBOT_CLIENT_HANDLE_INVALID;
	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return result;

#ifdef USE_PR2
	edictnum = SV_AddBotClient(name, 0, 0, "base", false);
	if (!edictnum)
	{
		result.status = EVOBOT_CREATE_BOT_NO_FREE_SLOT;
		return result;
	}

	slot = edictnum - 1;
	if (slot < 0 || slot >= MAX_CLIENTS || evobot_qw_client_handles[slot] != EVOBOT_CLIENT_HANDLE_INVALID)
	{
		if (slot >= 0 && slot < MAX_CLIENTS)
		{
			int old_self = pr_global_struct->self;

			RemoveBot(&svs.clients[slot]);
			pr_global_struct->self = old_self;
		}
		return result;
	}

	result.handle = EvoBot_QW_NewClientHandle();
	evobot_qw_client_handles[slot] = result.handle;
	result.status = EVOBOT_CREATE_BOT_OK;
#else
	(void)name;
	(void)edictnum;
	(void)slot;
	result.status = EVOBOT_CREATE_BOT_UNSUPPORTED_GAMECODE;
#endif

	return result;
}

static int EvoBot_QW_RemoveBotClient(evobot_client_handle_t handle)
{
#ifdef USE_PR2
	int old_self;
	int slot = EvoBot_QW_FindClientSlot(handle);

	if (slot < 0 || !EvoBot_QW_IsBotClientValid(handle))
		return 0;

	old_self = pr_global_struct->self;
	RemoveBot(&svs.clients[slot]);
	pr_global_struct->self = old_self;
	evobot_qw_client_handles[slot] = EVOBOT_CLIENT_HANDLE_INVALID;
	return 1;
#else
	(void)handle;
	return 0;
#endif
}

static void EvoBot_QW_Status_f(void)
{
	EvoBot_PrintStatus();
}

static void EvoBot_QW_Version_f(void)
{
	EvoBot_PrintVersion();
}

static void EvoBot_QW_Add_f(void)
{
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_add <name>\n");
		return;
	}

	EvoBot_AddBot(Cmd_Argv(1));
}

static void EvoBot_QW_Remove_f(void)
{
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_remove <name>\n");
		return;
	}

	EvoBot_RemoveBot(Cmd_Argv(1));
}

static void EvoBot_QW_ExecPrintSnapshot(const evobot_exec_snapshot_t *s,
	const char *prefix)
{
	const char *near_trigger_class = "";
	vec3_t near_trigger_mins = {0, 0, 0};
	vec3_t near_trigger_maxs = {0, 0, 0};
	float near_trigger_distance = 99999.0f;
	int client_slot = EvoBot_QW_FindClientSlot(s->handle);
	float weapon = client_slot >= 0 ? svs.clients[client_slot].edict->v->weapon : 0;
	const char *ground_class = "";
	const char *ground_model = "";
	int ground_entity = 0;
	const char *blocked_class = "";
	const char *blocked_model = "";
	int blocked_entity = 0;
	float blocked_fraction = 1.0f;
	int entity_index;
	if (client_slot >= 0)
	{
		edict_t *client_entity = svs.clients[client_slot].edict;
		edict_t *ground = PROG_TO_EDICT(client_entity->v->groundentity);
		vec3_t probe_end;
		trace_t probe;
		if (ground && !ground->e.free)
		{
			ground_entity = ground->e.entnum;
			ground_class = ground->v->classname ?
				PR_GetEntityString(ground->v->classname) : "";
			ground_model = ground->v->model ?
				PR_GetEntityString(ground->v->model) : "";
		}
		probe_end[0] = s->player.origin.v[0] + s->desired_direction.v[0] * 24.0f;
		probe_end[1] = s->player.origin.v[1] + s->desired_direction.v[1] * 24.0f;
		probe_end[2] = s->player.origin.v[2];
		probe = SV_Trace(client_entity->v->origin, client_entity->v->mins,
			client_entity->v->maxs, probe_end, MOVE_NORMAL, client_entity);
		blocked_fraction = probe.fraction;
		if (probe.e.ent && !probe.e.ent->e.free && probe.fraction < 1.0f)
		{
			blocked_entity = probe.e.ent->e.entnum;
			blocked_class = probe.e.ent->v->classname ?
				PR_GetEntityString(probe.e.ent->v->classname) : "";
			blocked_model = probe.e.ent->v->model ?
				PR_GetEntityString(probe.e.ent->v->model) : "";
		}
	}
	for (entity_index = MAX_CLIENTS + 1; entity_index < sv.num_edicts;
		entity_index++)
	{
		edict_t *entity = EDICT_NUM(entity_index);
		float squared = 0;
		int axis;
		if (!entity || entity->e.free || entity->v->solid != SOLID_TRIGGER)
			continue;
		for (axis = 0; axis < 3; axis++)
		{
			float delta = s->player.origin.v[axis] < entity->v->absmin[axis] ?
				entity->v->absmin[axis] - s->player.origin.v[axis] :
				s->player.origin.v[axis] > entity->v->absmax[axis] ?
				s->player.origin.v[axis] - entity->v->absmax[axis] : 0;
			squared += delta * delta;
		}
		if (squared < near_trigger_distance * near_trigger_distance)
		{
			near_trigger_distance = sqrtf(squared);
			near_trigger_class = entity->v->classname ?
				PR_GetEntityString(entity->v->classname) : "";
			VectorCopy(entity->v->absmin, near_trigger_mins);
			VectorCopy(entity->v->absmax, near_trigger_maxs);
		}
	}
	Con_Printf("%s{\"map\":\"%s\",\"time\":%.6f,\"bot\":\"%s\","
		"\"handle\":%u,\"state\":\"%s\",\"event\":\"%s\","
		"\"event_sequence\":%" PRIu64 ",\"origin\":[%.3f,%.3f,%.3f],"
		"\"velocity\":[%.3f,%.3f,%.3f],\"view\":[%.3f,%.3f],"
		"\"area\":%u,\"route_length\":%zu,\"route_index\":%zu,"
		"\"plan_count\":%zu,\"plan_index\":%zu,\"subgoal_active\":%d,"
		"\"subgoal_action\":%d,\"subgoal_area\":%u,"
		"\"subgoal_activator\":%u,\"subgoal_affected\":%u,"
		"\"reachability_id\":%u,\"travel_type\":\"%s\","
		"\"source_area\":%u,\"destination_area\":%u,"
		"\"reachability_flags\":%u,"
		"\"mover_interactor\":%u,\"mover_state\":%d,"
		"\"mover_origin\":[%.3f,%.3f,%.3f],"
		"\"mover_bounds\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
		"\"ground_entity\":%d,\"ground_class\":\"%s\",\"ground_model\":\"%s\","
		"\"blocked_entity\":%d,\"blocked_class\":\"%s\","
		"\"blocked_model\":\"%s\",\"blocked_fraction\":%.3f,"
		"\"near_trigger\":\"%s\",\"near_trigger_distance\":%.3f,"
		"\"near_trigger_bounds\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
		"\"steering_target\":[%.3f,%.3f,%.3f],\"target_distance\":%.3f,"
		"\"desired_direction\":[%.4f,%.4f,%.4f],\"keys\":%u,"
		"\"forwardmove\":%.1f,\"sidemove\":%.1f,\"upmove\":%.1f,"
		"\"buttons\":%u,\"impulse\":%u,\"weapon\":%.0f,"
		"\"jump\":%d,\"onground\":%d,\"waterlevel\":%d,"
		"\"movement_disabled\":%d,"
		"\"step_elapsed\":%.3f,\"total_elapsed\":%.3f,"
		"\"recent_progress\":%.3f,\"yaw_error\":%.3f,\"traversal_phase\":%d,"
		"\"replans\":%u,\"stuck_detections\":%u,\"traversal_retries\":%u,"
		"\"jumps_attempted\":%u,\"jumps_succeeded\":%u,"
		"\"reachabilities_completed\":%u,\"invalid_area_frames\":%u,"
		"\"test_notarget\":%d}\n",
		prefix, sv.mapname, s->server_time, s->name, s->handle,
		EvoBot_ExecStateName(s->state), EvoBot_ExecEventName(s->event),
		s->event_sequence,
		s->player.origin.v[0], s->player.origin.v[1], s->player.origin.v[2],
		s->player.velocity.v[0], s->player.velocity.v[1], s->player.velocity.v[2],
		s->player.view_angles.v[1], s->player.view_angles.v[0],
		s->current_area, s->route_length, s->route_index,
		s->plan_count, s->plan_index, s->subgoal_active,
		(int)s->subgoal_action, s->subgoal_area, s->subgoal_activator,
		s->subgoal_affected,
		s->step.reachability_id, EvoBot_QW_TravelTypeName(s->step.travel_type),
		s->step.source_area, s->step.destination_area,
		s->reachability_flags,
		s->mover_interactor, (int)s->mover_state,
		s->mover_origin.v[0], s->mover_origin.v[1], s->mover_origin.v[2],
		s->mover_bounds.mins.v[0], s->mover_bounds.mins.v[1],
		s->mover_bounds.mins.v[2], s->mover_bounds.maxs.v[0],
		s->mover_bounds.maxs.v[1], s->mover_bounds.maxs.v[2],
		ground_entity, ground_class, ground_model,
		blocked_entity, blocked_class, blocked_model, blocked_fraction,
		near_trigger_class, near_trigger_distance,
		near_trigger_mins[0], near_trigger_mins[1], near_trigger_mins[2],
		near_trigger_maxs[0], near_trigger_maxs[1], near_trigger_maxs[2],
		s->steering_target.v[0], s->steering_target.v[1], s->steering_target.v[2],
		s->target_distance, s->desired_direction.v[0],
		s->desired_direction.v[1], s->desired_direction.v[2], s->input.keys,
		s->input.forward_move, s->input.side_move, s->input.up_move,
		s->input.buttons, s->input.impulse, weapon,
		(s->input.buttons & EVOBOT_PLAYER_BUTTON_JUMP) != 0,
		s->player.on_ground, s->player.water_level,
		s->player.movement_disabled, s->step_elapsed,
		s->total_elapsed, s->recent_progress, s->yaw_error, s->traversal_phase,
		s->replans,
		s->stuck_detections, s->traversal_retries, s->jumps_attempted,
		s->jumps_succeeded, s->reachabilities_completed,
		s->invalid_area_frames,
		client_slot >= 0 &&
		((int)svs.clients[client_slot].edict->v->flags &
		 FL_NOTARGET) != 0);
}

static void EvoBot_QW_ExecStart_f(void)
{
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_exec_start <bot name>\n");
		return;
	}
	if (!EvoBot_ExecStart(Cmd_Argv(1)))
		Con_Printf("EvoBot execution: bot '%s' not found\n", Cmd_Argv(1));
	else
		Con_Printf("EvoBot execution started: %s (test isolation: notarget)\n",
			Cmd_Argv(1));
}

static void EvoBot_QW_ExecStop_f(void)
{
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_exec_stop <bot name>\n");
		return;
	}
	if (!EvoBot_ExecStop(Cmd_Argv(1)))
		Con_Printf("EvoBot execution: bot '%s' not found\n", Cmd_Argv(1));
	else
		Con_Printf("EvoBot execution stopped: %s\n", Cmd_Argv(1));
}

static void EvoBot_QW_ExecStatus_f(void)
{
	evobot_exec_snapshot_t snapshot;
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_exec_status <bot name>\n");
		return;
	}
	if (!EvoBot_ExecSnapshot(Cmd_Argv(1), &snapshot))
	{
		Con_Printf("EvoBot execution: bot '%s' not found\n", Cmd_Argv(1));
		return;
	}
	EvoBot_QW_ExecPrintSnapshot(&snapshot, "EVOBOT_EXEC_JSON ");
}

static void EvoBot_QW_ExecDebug_f(void)
{
	if (Cmd_Argc() != 2 || (strcmp(Cmd_Argv(1), "0") && strcmp(Cmd_Argv(1), "1")))
	{
		Con_Printf("usage: evobot_exec_debug 0|1\n");
		return;
	}
	evobot_qw_exec_debug = atoi(Cmd_Argv(1));
	Con_Printf("EvoBot execution debug: %d\n", evobot_qw_exec_debug);
}

static void EvoBot_QW_NavLipProbe_f(void)
{
	extern vec3_t player_mins;
	extern vec3_t player_maxs;
	evobot_vec3_t start;
	evobot_vec3_t end;
	evobot_trace_t trace;
	float z;
	for (z = 104.0f; z <= 208.0f; z += 8.0f)
	{
		trace_t server_trace;
		edict_t *hit;
		const char *classname;
		const char *model;
		start.v[0] = 719.5f;
		start.v[1] = 512.0f;
		start.v[2] = z;
		end.v[0] = 823.0f;
		end.v[1] = 512.0f;
		end.v[2] = z;
		memset(&trace, 0, sizeof(trace));
		if (EvoBot_QW_TracePlayerWorld(&start, &end, &trace))
			Con_Printf("lip probe z %.1f fraction %.4f end [%.2f %.2f %.2f] "
				"startsolid %d allsolid %d\n", z, trace.fraction,
				trace.end.v[0], trace.end.v[1], trace.end.v[2],
				trace.start_solid, trace.all_solid);
		server_trace = SV_Trace((float *)start.v, player_mins, player_maxs,
			(float *)end.v, MOVE_NORMAL, NULL);
		hit = server_trace.e.ent;
		classname = hit && hit->v->classname ?
			PR_GetEntityString(hit->v->classname) : "";
		model = hit && hit->v->model ? PR_GetEntityString(hit->v->model) : "";
		Con_Printf("lip live z %.1f fraction %.4f end [%.2f %.2f %.2f] "
			"hit %s model %s bounds [%.1f %.1f %.1f]-[%.1f %.1f %.1f]\n",
			z, server_trace.fraction, server_trace.endpos[0],
			server_trace.endpos[1], server_trace.endpos[2], classname, model,
			hit ? hit->v->absmin[0] : 0, hit ? hit->v->absmin[1] : 0,
			hit ? hit->v->absmin[2] : 0, hit ? hit->v->absmax[0] : 0,
			hit ? hit->v->absmax[1] : 0, hit ? hit->v->absmax[2] : 0);
	}
}

static void EvoBot_QW_ExecHistory_f(void)
{
	size_t count;
	size_t index;
	evobot_exec_snapshot_t snapshot;
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_exec_history <bot name>\n");
		return;
	}
	count = EvoBot_ExecHistoryCount(Cmd_Argv(1));
	for (index = 0; index < count; index++)
		if (EvoBot_ExecHistory(Cmd_Argv(1), index, &snapshot))
			EvoBot_QW_ExecPrintSnapshot(&snapshot, "EVOBOT_EXEC_HISTORY_JSON ");
}

static void EvoBot_QW_TestE1M1_f(void)
{
	if (strcmp(sv.mapname, "e1m1"))
	{
		Con_Printf("evobot_test_e1m1 requires map e1m1\n");
		return;
	}
	EvoBot_AddBot("e1m1bot");
	if (!EvoBot_ExecStart("e1m1bot"))
		Con_Printf("EvoBot E1M1 test failed to start\n");
	else
		Con_Printf("EvoBot E1M1 execution test started\n");
}

static void EvoBot_QW_ExecMap_f(void)
{
	Con_Printf("EVOBOT_MAP %s\n", sv.mapname);
}

static void EvoBot_QW_NavGenerate_f(void)
{
	EvoBot_NavConvexGenerate();
}

static void EvoBot_QW_NavStatus_f(void)
{
	EvoBot_NavConvexPrintStatus();
}

static void EvoBot_QW_NavReachStatus_f(void)
{
	EvoBot_NavReachPrintStatus();
}

static void EvoBot_QW_NavReachValidate_f(void)
{
	EvoBot_NavReachValidate();
}

static void EvoBot_QW_NavSave_f(void)
{
	EvoBot_NavConvexSave();
}

static void EvoBot_QW_NavLoad_f(void)
{
	if (Cmd_Argc() > 2)
	{
		Con_Printf("usage: evobot_nav_load [mapname]\n");
		return;
	}
	EvoBot_NavConvexLoad(Cmd_Argc() == 2 ? Cmd_Argv(1) : NULL);
}

static void EvoBot_QW_NavClear_f(void)
{
	EvoBot_NavConvexClear();
}

static void EvoBot_QW_NavExportObj_f(void)
{
	EvoBot_NavConvexExportObj();
}

static int EvoBot_QW_NavParseArea(int argument, uint32_t *area)
{
	char *end;
	unsigned long value;

	if (!area || argument >= Cmd_Argc() || !Cmd_Argv(argument)[0])
		return 0;
	value = strtoul(Cmd_Argv(argument), &end, 10);
	if (*end || !value || value > UINT32_MAX)
		return 0;
	*area = (uint32_t)value;
	return 1;
}

static int EvoBot_QW_NavRouteSource(uint32_t *area)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		evobot_vec3_t origin;

		if (evobot_qw_client_handles[i] == EVOBOT_CLIENT_HANDLE_INVALID ||
			!EvoBot_QW_IsBotClientValid(evobot_qw_client_handles[i]))
			continue;
		EvoBot_QW_CopyVector(svs.clients[i].edict->v->origin, &origin);
		if (EvoBot_NavDebugFindArea(&origin, area))
			return 1;
		Con_Printf("EvoBot routing: bot %d origin [%.2f %.2f %.2f] is outside "
			"all navigation areas\n", i, origin.v[0], origin.v[1], origin.v[2]);
		{
			evobot_nav_debug_summary_t summary;
			evobot_nav_debug_area_t nearest;
			float nearest_distance = FLT_MAX;
			size_t index;

			memset(&nearest, 0, sizeof(nearest));
			if (EvoBot_NavDebugSummary(&summary))
			{
				for (index = 0; index < summary.area_count; index++)
				{
					evobot_nav_debug_area_t candidate;
					float distance_squared = 0;
					int axis;

					if (!EvoBot_NavDebugArea(index, &candidate))
						continue;
					for (axis = 0; axis < 3; axis++)
					{
						float delta = origin.v[axis] < candidate.bounds.mins.v[axis] ?
							candidate.bounds.mins.v[axis] - origin.v[axis] :
							origin.v[axis] > candidate.bounds.maxs.v[axis] ?
							origin.v[axis] - candidate.bounds.maxs.v[axis] : 0;

						distance_squared += delta * delta;
					}
					if (distance_squared < nearest_distance)
					{
						nearest_distance = distance_squared;
						nearest = candidate;
					}
				}
				if (nearest.id)
					Con_Printf("EvoBot routing: nearest area %u is %.3f units away "
						"(supported %d, bounds [%.1f %.1f %.1f]-[%.1f %.1f %.1f])\n",
						nearest.id, sqrtf(nearest_distance), nearest.supported,
						nearest.bounds.mins.v[0], nearest.bounds.mins.v[1],
						nearest.bounds.mins.v[2], nearest.bounds.maxs.v[0],
						nearest.bounds.maxs.v[1], nearest.bounds.maxs.v[2]);
			}
		}
	}
	Con_Printf("EvoBot routing: add an EvoBot or provide an explicit source area\n");
	return 0;
}

static void EvoBot_QW_NavRouteExit_f(void)
{
	uint32_t source = 0;

	if (Cmd_Argc() > 2 ||
		(Cmd_Argc() == 2 && !EvoBot_QW_NavParseArea(1, &source)))
	{
		Con_Printf("usage: evobot_nav_route_exit [source area]\n");
		return;
	}
	if (Cmd_Argc() == 1 && !EvoBot_QW_NavRouteSource(&source))
		return;
	if (!EvoBot_NavRouteBuild(source, NULL) || !EvoBot_NavRouteSelectExit())
		return;
	Con_Printf("EvoBot route to exit\n");
	EvoBot_NavRoutePrintStatus();
}

static const char *EvoBot_QW_TravelTypeName(evobot_nav_travel_type_t type)
{
	switch (type)
	{
	case EVOBOT_NAV_TRAVEL_WALK: return "WALK";
	case EVOBOT_NAV_TRAVEL_DROP: return "DROP";
	case EVOBOT_NAV_TRAVEL_SWIM: return "SWIM";
	case EVOBOT_NAV_TRAVEL_WATER_ENTRY: return "WATER_ENTRY";
	case EVOBOT_NAV_TRAVEL_WATER_EXIT: return "WATER_EXIT";
	case EVOBOT_NAV_TRAVEL_WATER_JUMP: return "WATER_JUMP";
	case EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP: return "UNRESOLVED_WATER_JUMP";
	case EVOBOT_NAV_TRAVEL_JUMP: return "JUMP";
	case EVOBOT_NAV_TRAVEL_TELEPORT: return "TELEPORT";
	case EVOBOT_NAV_TRAVEL_PLATFORM: return "PLATFORM";
	default: return "UNKNOWN";
	}
}

static void EvoBot_QW_NavRouteDump_f(void)
{
	evobot_nav_route_summary_t summary;
	size_t index;

	if (!EvoBot_NavRouteDebugSummary(&summary) || !summary.route_length)
	{
		Con_Printf("EvoBot route dump: no selected route\n");
		return;
	}
	Con_Printf("EvoBot route dump: %zu steps, area %u -> %u\n",
		summary.route_length, summary.source_area, summary.destination_area);
	for (index = 0; index < summary.route_length; index++)
	{
		evobot_nav_route_step_t step;
		evobot_nav_reachability_t reach;
		if (!EvoBot_NavRouteDebugStep(index, &step, &reach))
			continue;
		Con_Printf("step %zu reach %u type %s area %u -> %u "
			"start [%.2f %.2f %.2f]-[%.2f %.2f %.2f] "
			"dest [%.2f %.2f %.2f]-[%.2f %.2f %.2f] "
			"height %.2f distance %.2f flags %u blocker %d mover %u\n",
			index + 1, reach.id, EvoBot_QW_TravelTypeName(reach.travel_type),
			reach.source_area, reach.destination_area,
			reach.start.start.v[0], reach.start.start.v[1], reach.start.start.v[2],
			reach.start.end.v[0], reach.start.end.v[1], reach.start.end.v[2],
			reach.destination.start.v[0], reach.destination.start.v[1],
			reach.destination.start.v[2], reach.destination.end.v[0],
			reach.destination.end.v[1], reach.destination.end.v[2],
			reach.height_delta, reach.travel_distance, reach.flags,
			reach.dynamic_interactor, reach.mover_interactor);
	}
}

static void EvoBot_QW_NavInteractor_f(void)
{
	evobot_nav_debug_interactor_t interactor;
	unsigned long id;

	if (Cmd_Argc() != 2 ||
		(id = strtoul(Cmd_Argv(1), NULL, 10)) == 0 ||
		!EvoBot_NavDebugInteractor((size_t)id - 1, &interactor))
	{
		Con_Printf("usage: evobot_nav_interactor <id>\n");
		return;
	}
	Con_Printf("EvoBot interactor %u: %s model %s kind %d activation %d "
		"state %d movement %d health %.1f wait %.2f travel %.2f\n",
		interactor.id, interactor.classname, interactor.model,
		(int)interactor.kind, (int)interactor.activation,
		(int)interactor.current_state, interactor.has_movement,
		interactor.health, interactor.wait, interactor.travel_time);
	Con_Printf("bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f] "
		"current [%.2f %.2f %.2f]-[%.2f %.2f %.2f]\n",
		interactor.bounds.mins.v[0], interactor.bounds.mins.v[1],
		interactor.bounds.mins.v[2], interactor.bounds.maxs.v[0],
		interactor.bounds.maxs.v[1], interactor.bounds.maxs.v[2],
		interactor.current_bounds.mins.v[0], interactor.current_bounds.mins.v[1],
		interactor.current_bounds.mins.v[2], interactor.current_bounds.maxs.v[0],
		interactor.current_bounds.maxs.v[1], interactor.current_bounds.maxs.v[2]);
	Con_Printf("endpoint A [%.2f %.2f %.2f]-[%.2f %.2f %.2f] "
		"B [%.2f %.2f %.2f]-[%.2f %.2f %.2f] target '%s' name '%s'\n",
		interactor.endpoint_a_bounds.mins.v[0],
		interactor.endpoint_a_bounds.mins.v[1],
		interactor.endpoint_a_bounds.mins.v[2],
		interactor.endpoint_a_bounds.maxs.v[0],
		interactor.endpoint_a_bounds.maxs.v[1],
		interactor.endpoint_a_bounds.maxs.v[2],
		interactor.endpoint_b_bounds.mins.v[0],
		interactor.endpoint_b_bounds.mins.v[1],
		interactor.endpoint_b_bounds.mins.v[2],
		interactor.endpoint_b_bounds.maxs.v[0],
		interactor.endpoint_b_bounds.maxs.v[1],
		interactor.endpoint_b_bounds.maxs.v[2], interactor.target,
		interactor.targetname);
}

static void EvoBot_QW_NavPlanExit_f(void)
{
	uint32_t source = 0;

	if (Cmd_Argc() > 2 || (Cmd_Argc() == 2 &&
		(source = (uint32_t)strtoul(Cmd_Argv(1), NULL, 10)) == 0))
	{
		Con_Printf("usage: evobot_nav_plan_exit [source area]\n");
		return;
	}
	if (Cmd_Argc() == 1 && !EvoBot_QW_NavRouteSource(&source))
		return;
	Con_Printf("EvoBot plan to exit\n");
	EvoBot_NavPlanExit(source);
	EvoBot_NavPlanPrintStatus();
}

static void EvoBot_QW_NavPlanStatus_f(void) { EvoBot_NavPlanPrintStatus(); }
static void EvoBot_QW_NavPlanDump_f(void) { EvoBot_NavPlanPrintDump(); }
static void EvoBot_QW_NavPlanClear_f(void) { EvoBot_NavPlanClear(); }

static void EvoBot_QW_NavRouteArea_f(void)
{
	uint32_t source = 0;
	uint32_t destination;

	if ((Cmd_Argc() != 2 && Cmd_Argc() != 3) ||
		!EvoBot_QW_NavParseArea(1, &destination) ||
		(Cmd_Argc() == 3 && !EvoBot_QW_NavParseArea(2, &source)))
	{
		Con_Printf("usage: evobot_nav_route_area <area id> [source area]\n");
		return;
	}
	if ((Cmd_Argc() == 2 && !EvoBot_QW_NavRouteSource(&source)) ||
		!EvoBot_NavRouteBuild(source, NULL) ||
		!EvoBot_NavRouteSelectArea(destination))
		return;
	EvoBot_NavRoutePrintStatus();
}

static void EvoBot_QW_NavRouteClear_f(void)
{
	EvoBot_NavRouteClear();
	Con_Printf("EvoBot route cleared\n");
}

static void EvoBot_QW_NavRouteStatus_f(void)
{
	EvoBot_NavRoutePrintStatus();
}

static void EvoBot_QW_NavCost_f(void)
{
	uint32_t area;

	if (Cmd_Argc() != 2 || !EvoBot_QW_NavParseArea(1, &area))
	{
		Con_Printf("usage: evobot_nav_cost <area id>\n");
		return;
	}
	EvoBot_NavRoutePrintCost(area);
}

static void EvoBot_QW_NavRouteValidate_f(void)
{
	EvoBot_NavRouteValidate();
}

static void EvoBot_QW_NavProblemReport_f(void)
{
	size_t index = 0;
	size_t count = EvoBot_NavRouteDebugProblemCount();

	if (Cmd_Argc() > 2)
	{
		Con_Printf("usage: evobot_nav_problem_report [1-based index]\n");
		return;
	}
	if (Cmd_Argc() == 2)
	{
		char *end;
		unsigned long value = strtoul(Cmd_Argv(1), &end, 10);

		if (*end || !value || value > count)
		{
			Con_Printf("evobot_nav_problem_report: index must be 1..%zu\n",
				count);
			return;
		}
		index = (size_t)value - 1;
	}
	else
	{
		evobot_nav_route_summary_t summary;
		size_t i;

		if (EvoBot_NavRouteDebugSummary(&summary) && summary.frontier_portal)
		{
			for (i = 0; i < count; i++)
			{
				evobot_nav_route_problem_t problem;

				if (EvoBot_NavRouteDebugProblem(i, &problem) &&
					problem.portal_id == summary.frontier_portal)
				{
					index = i;
					break;
				}
			}
		}
	}
	EvoBot_NavRoutePrintProblem(index);
}

static void EvoBot_QW_NavFrontierReport_f(void)
{
	if (Cmd_Argc() != 1)
	{
		Con_Printf("usage: evobot_nav_frontier_report\n");
		return;
	}
	EvoBot_NavRoutePrintFrontierAudit();
}

static void EvoBot_QW_NavWaterJump_f(void)
{
	evobot_nav_debug_air_candidate_t candidate;
	evobot_nav_debug_air_kind_t kind;
	const char *label;
	unsigned long portal;
	unsigned long source;
	unsigned long destination;
	char *end;

	kind = !strcmp(Cmd_Argv(0), "evobot_nav_water_exit") ?
		EVOBOT_NAV_DEBUG_AIR_WATER_EXIT :
		(!strcmp(Cmd_Argv(0), "evobot_nav_jump_candidate") ?
		 EVOBOT_NAV_DEBUG_AIR_JUMP_UP : EVOBOT_NAV_DEBUG_AIR_WATER_JUMP);
	label = kind == EVOBOT_NAV_DEBUG_AIR_WATER_EXIT ? "WATER_EXIT" :
		(kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP ? "JUMP_UP" : "WATER_JUMP");
	if (Cmd_Argc() != 4)
	{
		Con_Printf("usage: %s <portal> <source> <destination>\n", Cmd_Argv(0));
		return;
	}
	portal = strtoul(Cmd_Argv(1), &end, 10);
	if (*end || !portal)
		goto invalid;
	source = strtoul(Cmd_Argv(2), &end, 10);
	if (*end || !source)
		goto invalid;
	destination = strtoul(Cmd_Argv(3), &end, 10);
	if (*end || !destination)
		goto invalid;
	if (!EvoBot_NavDebugAirCandidate((uint32_t)portal, (uint32_t)source,
		(uint32_t)destination, kind, &candidate))
	{
		Con_Printf("water-jump candidate unavailable\n");
		return;
	}
	Con_Printf("%s diagnostic: portal %u source %u destination %u present %d valid %d rejection %s attempts %u\n",
		label,
		candidate.portal_id, candidate.source_area,
		candidate.requested_destination_area, candidate.present, candidate.valid,
		EvoBot_NavDebugAirRejectionName(candidate.rejection), candidate.attempts);
	Con_Printf("%s direction [%.3f %.3f %.3f] edge %u width %.2f speed %.1f approach %u steps %u blocked %u left %d entered %d activated %d landed %d hull-clear %d area %u\n",
		label,
		candidate.direction.v[0], candidate.direction.v[1], candidate.direction.v[2],
		candidate.edge_index, candidate.portal_width, candidate.command_speed,
		candidate.approach_frames,
		candidate.move_steps, candidate.blocked_steps, candidate.left_source,
		candidate.entered_destination, candidate.airborne, candidate.landed,
		candidate.landing_hull_clear, candidate.landing_area);
	Con_Printf("%s points: start [%.2f %.2f %.2f] launch [%.2f %.2f %.2f] landing [%.2f %.2f %.2f] final [%.2f %.2f %.2f] time %.3f rise %.2f horizontal %.2f\n",
		label,
		candidate.start_origin.v[0], candidate.start_origin.v[1], candidate.start_origin.v[2],
		candidate.launch_origin.v[0], candidate.launch_origin.v[1], candidate.launch_origin.v[2],
		candidate.landing_origin.v[0], candidate.landing_origin.v[1], candidate.landing_origin.v[2],
		candidate.final_origin.v[0], candidate.final_origin.v[1], candidate.final_origin.v[2],
		candidate.elapsed, candidate.maximum_rise,
		candidate.horizontal_displacement);
	return;

invalid:
	Con_Printf("evobot_nav_water_jump: all ids must be positive integers\n");
}

void EvoBot_QW_Init(void)
{
	if (evobot_qw_initialized)
		return;

	evobot_qw_host.print = EvoBot_QW_Print;
	evobot_qw_host.server_time = EvoBot_QW_ServerTime;
	evobot_qw_host.create_bot_client = EvoBot_QW_CreateBotClient;
	evobot_qw_host.remove_bot_client = EvoBot_QW_RemoveBotClient;
	evobot_qw_host.is_bot_client_valid = EvoBot_QW_IsBotClientValid;
	evobot_qw_host.set_test_isolation = EvoBot_QW_SetTestIsolation;
	evobot_qw_host.monotonic_time = EvoBot_QW_MonotonicTime;
	evobot_qw_host.world_bounds = EvoBot_QW_WorldBounds;
	evobot_qw_host.player_bounds = EvoBot_QW_PlayerBounds;
	evobot_qw_host.trace_player_world = EvoBot_QW_TracePlayerWorld;
	evobot_qw_host.trace_player_solids = EvoBot_QW_TracePlayerSolids;
	evobot_qw_host.point_contents = EvoBot_QW_PointContents;
	evobot_qw_host.player_physics = EvoBot_QW_PlayerPhysics;
	evobot_qw_host.simulate_player_move = EvoBot_QW_SimulatePlayerMove;
	evobot_qw_host.collision_tree = EvoBot_QW_CollisionTree;
	evobot_qw_host.collision_node = EvoBot_QW_CollisionNode;
	evobot_qw_host.interactor_count = EvoBot_QW_InteractorCount;
	evobot_qw_host.get_interactor = EvoBot_QW_GetInteractor;
	evobot_qw_host.dynamic_blocker_state = EvoBot_QW_DynamicBlockerState;
	evobot_qw_host.interactor_state = EvoBot_QW_InteractorState;
	evobot_qw_host.file_size = EvoBot_QW_FileSize;
	evobot_qw_host.read_file = EvoBot_QW_ReadFile;
	evobot_qw_host.write_file = EvoBot_QW_WriteFile;
	EvoBot_Init(&evobot_qw_host);

	Cmd_AddCommand("evobot_status", EvoBot_QW_Status_f);
	Cmd_AddCommand("evobot_version", EvoBot_QW_Version_f);
	Cmd_AddCommand("evobot_add", EvoBot_QW_Add_f);
	Cmd_AddCommand("evobot_remove", EvoBot_QW_Remove_f);
	Cmd_AddCommand("evobot_exec_start", EvoBot_QW_ExecStart_f);
	Cmd_AddCommand("evobot_exec_stop", EvoBot_QW_ExecStop_f);
	Cmd_AddCommand("evobot_exec_status", EvoBot_QW_ExecStatus_f);
	Cmd_AddCommand("evobot_exec_debug", EvoBot_QW_ExecDebug_f);
	Cmd_AddCommand("evobot_exec_history", EvoBot_QW_ExecHistory_f);
	Cmd_AddCommand("evobot_test_e1m1", EvoBot_QW_TestE1M1_f);
	Cmd_AddCommand("evobot_exec_map", EvoBot_QW_ExecMap_f);
	Cmd_AddCommand("evobot_nav_generate", EvoBot_QW_NavGenerate_f);
	Cmd_AddCommand("evobot_nav_status", EvoBot_QW_NavStatus_f);
	Cmd_AddCommand("evobot_nav_reach_status", EvoBot_QW_NavReachStatus_f);
	Cmd_AddCommand("evobot_nav_reach_validate", EvoBot_QW_NavReachValidate_f);
	Cmd_AddCommand("evobot_nav_save", EvoBot_QW_NavSave_f);
	Cmd_AddCommand("evobot_nav_load", EvoBot_QW_NavLoad_f);
	Cmd_AddCommand("evobot_nav_clear", EvoBot_QW_NavClear_f);
	Cmd_AddCommand("evobot_nav_export_obj", EvoBot_QW_NavExportObj_f);
	Cmd_AddCommand("evobot_nav_route_exit", EvoBot_QW_NavRouteExit_f);
	Cmd_AddCommand("evobot_nav_route_dump", EvoBot_QW_NavRouteDump_f);
	Cmd_AddCommand("evobot_nav_interactor", EvoBot_QW_NavInteractor_f);
	Cmd_AddCommand("evobot_nav_route_area", EvoBot_QW_NavRouteArea_f);
	Cmd_AddCommand("evobot_nav_route_clear", EvoBot_QW_NavRouteClear_f);
	Cmd_AddCommand("evobot_nav_route_status", EvoBot_QW_NavRouteStatus_f);
	Cmd_AddCommand("evobot_nav_cost", EvoBot_QW_NavCost_f);
	Cmd_AddCommand("evobot_nav_route_validate", EvoBot_QW_NavRouteValidate_f);
	Cmd_AddCommand("evobot_nav_plan_exit", EvoBot_QW_NavPlanExit_f);
	Cmd_AddCommand("evobot_nav_plan_status", EvoBot_QW_NavPlanStatus_f);
	Cmd_AddCommand("evobot_nav_plan_dump", EvoBot_QW_NavPlanDump_f);
	Cmd_AddCommand("evobot_nav_plan_clear", EvoBot_QW_NavPlanClear_f);
	Cmd_AddCommand("evobot_nav_problem_report", EvoBot_QW_NavProblemReport_f);
	Cmd_AddCommand("evobot_nav_frontier_report", EvoBot_QW_NavFrontierReport_f);
	Cmd_AddCommand("evobot_nav_water_jump", EvoBot_QW_NavWaterJump_f);
	Cmd_AddCommand("evobot_nav_water_exit", EvoBot_QW_NavWaterJump_f);
	Cmd_AddCommand("evobot_nav_jump_candidate", EvoBot_QW_NavWaterJump_f);
	Cmd_AddCommand("evobot_nav_lip_probe", EvoBot_QW_NavLipProbe_f);
	evobot_qw_initialized = 1;
}

void EvoBot_QW_MapLoaded(void)
{
	evobot_map_info_t map;

	if (!evobot_qw_initialized || evobot_qw_map_loaded)
		return;

	map.name = sv.mapname;
	map.checksum = sv.map_checksum;
	memset(evobot_qw_entity_cache, 0, sizeof(evobot_qw_entity_cache));
	evobot_qw_entity_cache_count = 0;
	EvoBot_MapLoaded(&map);
	evobot_qw_map_loaded = 1;
}

void EvoBot_QW_Frame(void)
{
	if (!evobot_qw_initialized)
		return;

	EvoBot_Frame(sv.time);
}

void EvoBot_QW_PrepareBotCommands(double frame_time)
{
#ifdef USE_PR2
	double command_msec;
	int msec;
	int i;

	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return;

	command_msec = frame_time * 1000.0 + evobot_qw_command_msec_remainder;
	msec = (int)command_msec;
	evobot_qw_command_msec_remainder = command_msec - msec;
	msec = bound(1, msec, 255);

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client_t *client;

		if (evobot_qw_client_handles[i] == EVOBOT_CLIENT_HANDLE_INVALID ||
			!EvoBot_QW_IsBotClientValid(evobot_qw_client_handles[i]))
			continue;

		client = &svs.clients[i];
		memset(&client->botcmd, 0, sizeof(client->botcmd));
		client->botcmd.msec = (byte)msec;
		VectorCopy(client->edict->v->v_angle, client->botcmd.angles);

		if (client->edict->v->fixangle)
		{
			VectorCopy(client->edict->v->angles, client->botcmd.angles);
			client->botcmd.angles[PITCH] *= -3;
			client->edict->v->fixangle = 0;
		}
		{
			evobot_exec_player_state_t player;
			evobot_virtual_input_t input;
			memset(&player, 0, sizeof(player));
			EvoBot_QW_CopyVector(client->edict->v->origin, &player.origin);
			EvoBot_QW_CopyVector(client->edict->v->velocity, &player.velocity);
			EvoBot_QW_CopyVector(client->edict->v->v_angle, &player.view_angles);
			player.on_ground = ((int)client->edict->v->flags & FL_ONGROUND) != 0;
			player.water_level = (int)client->edict->v->waterlevel;
			player.alive = client->edict->v->health > 0;
			player.movement_disabled =
				(int)client->edict->v->movetype == MOVETYPE_NONE ||
				(int)client->edict->v->solid == SOLID_NOT;
			if (EvoBot_ExecPrepareCommand(evobot_qw_client_handles[i], &player,
				frame_time, &input))
			{
				VectorSet(client->botcmd.angles, input.view_angles.v[0],
					input.view_angles.v[1], input.view_angles.v[2]);
				client->botcmd.forwardmove = (short)bound(-400,
					input.forward_move, 400);
				client->botcmd.sidemove = (short)bound(-400,
					input.side_move, 400);
				client->botcmd.upmove = (short)bound(-400, input.up_move, 400);
				client->botcmd.buttons =
					(input.buttons & EVOBOT_PLAYER_BUTTON_JUMP) ? BUTTON_JUMP : 0;
				if (input.buttons & EVOBOT_PLAYER_BUTTON_ATTACK)
					client->botcmd.buttons |= BUTTON_ATTACK;
				client->botcmd.impulse = (byte)input.impulse;
			}
		}
	}
#else
	(void)frame_time;
#endif
}

void EvoBot_QW_MapCleared(void)
{
	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return;

	EvoBot_MapCleared();
	memset(evobot_qw_client_handles, 0, sizeof(evobot_qw_client_handles));
	memset(evobot_qw_entity_cache, 0, sizeof(evobot_qw_entity_cache));
	evobot_qw_entity_cache_count = 0;
	evobot_qw_command_msec_remainder = 0;
	evobot_qw_map_loaded = 0;
}

void EvoBot_QW_Shutdown(void)
{
	if (!evobot_qw_initialized)
		return;

	EvoBot_QW_MapCleared();
	EvoBot_Shutdown();
	memset(&evobot_qw_host, 0, sizeof(evobot_qw_host));
	memset(evobot_qw_client_handles, 0, sizeof(evobot_qw_client_handles));
	evobot_qw_command_msec_remainder = 0;
	evobot_qw_initialized = 0;
}
