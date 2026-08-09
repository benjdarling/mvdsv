#include "evobot/evobot_nav_route.h"

#include "evobot/evobot_nav_debug.h"
#include "evobot_nav_route_internal.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct evobot_nav_cost_field_s
{
	uint64_t nav_revision;
	uint32_t source_area;
	evobot_nav_route_policy_t policy;
	float *costs;
	uint32_t *predecessors;
	size_t area_count;
	size_t routing_area_count;
	size_t reachable_area_count;
	size_t reachability_count;
	double execution_time;
	size_t result_memory;
} evobot_nav_cost_field_t;

typedef struct evobot_nav_heap_s
{
	uint32_t *areas;
	uint32_t *positions;
	size_t count;
	const float *costs;
} evobot_nav_heap_t;

static evobot_host_api_t evobot_route_host;
static evobot_nav_cost_field_t evobot_route_primary;
static evobot_nav_cost_field_t evobot_route_alternate;
static evobot_nav_route_summary_t evobot_route_summary;
static evobot_nav_route_step_t *evobot_route_steps;
static uint64_t evobot_route_revision;

#define EVOBOT_NAV_PLAN_MAX_SUBGOALS 32
typedef struct evobot_nav_plan_subgoal_s
{
	uint32_t activator;
	uint32_t affected;
	uint32_t area;
	evobot_interactor_activation_t activation;
} evobot_nav_plan_subgoal_t;

static struct
{
	int present;
	int solvable;
	evobot_nav_plan_status_t status;
	uint32_t source_area;
	int exit_interactor;
	evobot_nav_plan_subgoal_t subgoals[EVOBOT_NAV_PLAN_MAX_SUBGOALS];
	size_t subgoal_count;
	int unresolved_interactor;
	char unresolved_reason[128];
	float navigation_cost;
	float dynamic_wait;
	double calculation_time;
	size_t searched_world_states;
	size_t timed_world_states;
	size_t counter_world_states;
	size_t deepest_dependency_chain;
} evobot_nav_plan;

static void EvoBot_RoutePrint(const char *message)
{
	if (evobot_route_host.print)
		evobot_route_host.print(message);
}

static void EvoBot_RoutePrintf(const char *format, ...)
{
	char message[512];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';
	EvoBot_RoutePrint(message);
}

static double EvoBot_RouteTime(void)
{
	if (evobot_route_host.monotonic_time)
		return evobot_route_host.monotonic_time();
	return (double)clock() / CLOCKS_PER_SEC;
}

static void EvoBot_RouteTouch(void)
{
	evobot_route_revision++;
	if (!evobot_route_revision)
		evobot_route_revision++;
	evobot_route_summary.route_revision = evobot_route_revision;
}

static void EvoBot_RouteFreeField(evobot_nav_cost_field_t *field)
{
	free(field->costs);
	free(field->predecessors);
	memset(field, 0, sizeof(*field));
}

static void EvoBot_RouteClearInternal(void)
{
	EvoBot_RouteFreeField(&evobot_route_primary);
	EvoBot_RouteFreeField(&evobot_route_alternate);
	free(evobot_route_steps);
	evobot_route_steps = NULL;
	memset(&evobot_route_summary, 0, sizeof(evobot_route_summary));
	memset(&evobot_nav_plan, 0, sizeof(evobot_nav_plan));
	EvoBot_RouteTouch();
}

static int EvoBot_RouteSync(void)
{
	uint64_t revision = EvoBot_NavDebugRevision();

	if ((evobot_route_primary.nav_revision &&
		evobot_route_primary.nav_revision != revision) ||
		(evobot_route_summary.nav_revision &&
		evobot_route_summary.nav_revision != revision))
		EvoBot_RouteClearInternal();
	return evobot_route_primary.nav_revision == revision &&
		evobot_route_primary.costs != NULL;
}

static int EvoBot_RouteAreaRouting(const evobot_nav_debug_area_t *area)
{
	return area->supported ||
		((area->contents == EVOBOT_CONTENTS_WATER ||
			area->contents == EVOBOT_CONTENTS_SLIME ||
			area->contents == EVOBOT_CONTENTS_LAVA) && area->water_level >= 2);
}

static void EvoBot_RouteCrossingBounds(
	const evobot_nav_reachability_t *reachability, evobot_bounds_t *bounds)
{
	const evobot_vec3_t *points[4];
	int point;
	int axis;

	points[0] = &reachability->start.start;
	points[1] = &reachability->start.end;
	points[2] = &reachability->destination.start;
	points[3] = &reachability->destination.end;
	bounds->mins = *points[0];
	bounds->maxs = *points[0];
	for (point = 1; point < 4; point++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			if (points[point]->v[axis] < bounds->mins.v[axis])
				bounds->mins.v[axis] = points[point]->v[axis];
			if (points[point]->v[axis] > bounds->maxs.v[axis])
				bounds->maxs.v[axis] = points[point]->v[axis];
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		bounds->mins.v[axis] -= 0.5f;
		bounds->maxs.v[axis] += 0.5f;
	}
}

static evobot_dynamic_blocker_state_t EvoBot_RouteBlockerState(
	const evobot_nav_reachability_t *reachability)
{
	evobot_bounds_t bounds;

	if (!(reachability->flags & EVOBOT_NAV_REACH_DYNAMIC_BLOCKER))
		return EVOBOT_DYNAMIC_BLOCKER_CLEAR;
	if (reachability->dynamic_interactor <= 0)
		return EVOBOT_DYNAMIC_BLOCKER_UNKNOWN;
	EvoBot_RouteCrossingBounds(reachability, &bounds);
	return EvoBot_NavDebugBlockerState(reachability->dynamic_interactor,
		&bounds);
}

static int EvoBot_RouteAutomaticDoor(int interactor_id, float *delay)
{
	evobot_nav_debug_interactor_t interactor;

	if (interactor_id <= 0 ||
		!EvoBot_NavDebugInteractor((size_t)interactor_id - 1, &interactor) ||
		interactor.kind != EVOBOT_INTERACTOR_DOOR ||
		interactor.activation != EVOBOT_ACTIVATION_APPROACH)
		return 0;
	if (delay)
		*delay = interactor.travel_time > 0 ? interactor.travel_time : 1.0f;
	return 1;
}

static void EvoBot_RouteHeapSwap(evobot_nav_heap_t *heap, size_t a, size_t b)
{
	uint32_t area = heap->areas[a];

	heap->areas[a] = heap->areas[b];
	heap->areas[b] = area;
	heap->positions[heap->areas[a]] = (uint32_t)a;
	heap->positions[heap->areas[b]] = (uint32_t)b;
}

static void EvoBot_RouteHeapUp(evobot_nav_heap_t *heap, size_t index)
{
	while (index)
	{
		size_t parent = (index - 1) / 2;

		if (heap->costs[heap->areas[parent]] <=
			heap->costs[heap->areas[index]])
			break;
		EvoBot_RouteHeapSwap(heap, parent, index);
		index = parent;
	}
}

static void EvoBot_RouteHeapDown(evobot_nav_heap_t *heap, size_t index)
{
	for (;;)
	{
		size_t child = index * 2 + 1;
		size_t other;

		if (child >= heap->count)
			break;
		other = child + 1;
		if (other < heap->count &&
			heap->costs[heap->areas[other]] <
			heap->costs[heap->areas[child]])
			child = other;
		if (heap->costs[heap->areas[index]] <=
			heap->costs[heap->areas[child]])
			break;
		EvoBot_RouteHeapSwap(heap, index, child);
		index = child;
	}
}

static void EvoBot_RouteHeapInsert(evobot_nav_heap_t *heap, uint32_t area)
{
	uint32_t position = heap->positions[area];

	if (position != UINT32_MAX)
	{
		EvoBot_RouteHeapUp(heap, position);
		return;
	}
	heap->positions[area] = (uint32_t)heap->count;
	heap->areas[heap->count++] = area;
	EvoBot_RouteHeapUp(heap, heap->count - 1);
}

static uint32_t EvoBot_RouteHeapPop(evobot_nav_heap_t *heap)
{
	uint32_t result = heap->areas[0];

	heap->positions[result] = UINT32_MAX;
	heap->count--;
	if (heap->count)
	{
		heap->areas[0] = heap->areas[heap->count];
		heap->positions[heap->areas[0]] = 0;
		EvoBot_RouteHeapDown(heap, 0);
	}
	return result;
}

static int EvoBot_RouteBuildField(evobot_nav_cost_field_t *field,
	uint32_t source_area, const evobot_nav_route_policy_t *policy)
{
	evobot_nav_debug_summary_t summary;
	evobot_nav_reachability_t *reachabilities = NULL;
	int *heads = NULL;
	int *next = NULL;
	uint32_t *heap_areas = NULL;
	uint32_t *positions = NULL;
	unsigned char *settled = NULL;
	evobot_nav_heap_t heap;
	double started;
	size_t i;
	int success = 0;

	memset(&heap, 0, sizeof(heap));
	if (!EvoBot_NavDebugSummary(&summary) || !summary.present ||
		!source_area || source_area > summary.area_count)
		return 0;
	EvoBot_RouteFreeField(field);
	field->costs = malloc(summary.area_count * sizeof(*field->costs));
	field->predecessors = calloc(summary.area_count,
		sizeof(*field->predecessors));
	reachabilities = malloc(summary.reachability_count * sizeof(*reachabilities));
	heads = malloc(summary.area_count * sizeof(*heads));
	next = malloc(summary.reachability_count * sizeof(*next));
	heap_areas = malloc(summary.area_count * sizeof(*heap_areas));
	positions = malloc(summary.area_count * sizeof(*positions));
	settled = calloc(summary.area_count, 1);
	if ((!field->costs || !field->predecessors || !reachabilities || !heads ||
		!next || !heap_areas || !positions || !settled) && summary.area_count)
		goto done;
	for (i = 0; i < summary.area_count; i++)
	{
		evobot_nav_debug_area_t area;

		field->costs[i] = FLT_MAX;
		heads[i] = -1;
		positions[i] = UINT32_MAX;
		if (EvoBot_NavDebugArea(i, &area) && EvoBot_RouteAreaRouting(&area))
			field->routing_area_count++;
	}
	for (i = 0; i < summary.reachability_count; i++)
	{
		if (!EvoBot_NavDebugReachability(i, &reachabilities[i]) ||
			!reachabilities[i].source_area ||
			reachabilities[i].source_area > summary.area_count)
			goto done;
		next[i] = heads[reachabilities[i].source_area - 1];
		heads[reachabilities[i].source_area - 1] = (int)i;
	}
	field->nav_revision = summary.revision;
	field->source_area = source_area;
	field->policy = *policy;
	field->area_count = summary.area_count;
	field->reachability_count = summary.reachability_count;
	field->result_memory = summary.area_count *
		(sizeof(*field->costs) + sizeof(*field->predecessors));
	field->costs[source_area - 1] = 0;
	heap.areas = heap_areas;
	heap.positions = positions;
	heap.costs = field->costs;
	EvoBot_RouteHeapInsert(&heap, source_area - 1);
	started = EvoBot_RouteTime();
	while (heap.count)
	{
		uint32_t area = EvoBot_RouteHeapPop(&heap);
		int reachability_index;

		if (settled[area])
			continue;
		settled[area] = 1;
		for (reachability_index = heads[area]; reachability_index >= 0;
			reachability_index = next[reachability_index])
		{
			const evobot_nav_reachability_t *reachability =
				&reachabilities[reachability_index];
			uint32_t destination;
			float edge_cost = reachability->base_travel_time;
			float candidate;
			evobot_dynamic_blocker_state_t blocker_state;

			if (reachability->travel_type ==
				EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP ||
				!reachability->destination_area ||
				reachability->destination_area > summary.area_count)
				continue;
			blocker_state = EvoBot_RouteBlockerState(reachability);
			if (policy->mode == EVOBOT_NAV_ROUTE_CURRENT_WORLD &&
				blocker_state != EVOBOT_DYNAMIC_BLOCKER_CLEAR)
			{
				float opening_delay;

				if (!EvoBot_RouteAutomaticDoor(
					reachability->dynamic_interactor, &opening_delay))
					continue;
				edge_cost += opening_delay;
			}
			if (policy->cost && !policy->cost(reachability,
				field->costs[area], reachability->base_travel_time, &edge_cost,
				policy->user_data))
				continue;
			if (edge_cost < 0 || edge_cost >= FLT_MAX)
			{
				EvoBot_RoutePrintf("EvoBot routing: invalid negative or non-finite "
					"cost for reachability %" PRIu32 "\n", reachability->id);
				goto done;
			}
			destination = reachability->destination_area - 1;
			if (settled[destination])
				continue;
			candidate = field->costs[area] + edge_cost;
			if (candidate < field->costs[destination])
			{
				field->costs[destination] = candidate;
				field->predecessors[destination] = reachability->id;
				EvoBot_RouteHeapInsert(&heap, destination);
			}
		}
	}
	field->execution_time = EvoBot_RouteTime() - started;
	for (i = 0; i < summary.area_count; i++)
	{
		if (field->costs[i] < FLT_MAX)
			field->reachable_area_count++;
	}
	success = 1;

done:
	free(reachabilities);
	free(heads);
	free(next);
	free(heap_areas);
	free(positions);
	free(settled);
	if (!success)
		EvoBot_RouteFreeField(field);
	return success;
}

static int EvoBot_RouteBuildAlternate(void)
{
	evobot_nav_route_policy_t policy = evobot_route_primary.policy;

	if (evobot_route_alternate.costs &&
		evobot_route_alternate.nav_revision == evobot_route_primary.nav_revision)
		return 1;
	policy.mode = EVOBOT_NAV_ROUTE_IGNORE_DYNAMIC_BLOCKERS;
	if (!EvoBot_RouteBuildField(&evobot_route_alternate,
		evobot_route_primary.source_area, &policy))
		return 0;
	evobot_route_summary.ignore_dynamic_evaluated = 1;
	evobot_route_summary.ignore_dynamic_reachable_area_count =
		evobot_route_alternate.reachable_area_count;
	return 1;
}

static int EvoBot_RouteFinite(const evobot_nav_cost_field_t *field,
	uint32_t area)
{
	return field->costs && area && area <= field->area_count &&
		field->costs[area - 1] < FLT_MAX;
}

static float EvoBot_RouteBoundsDistance(const evobot_bounds_t *a,
	const evobot_bounds_t *b)
{
	float distance_squared = 0;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float distance = 0;

		if (a->maxs.v[axis] < b->mins.v[axis])
			distance = b->mins.v[axis] - a->maxs.v[axis];
		else if (b->maxs.v[axis] < a->mins.v[axis])
			distance = a->mins.v[axis] - b->maxs.v[axis];
		distance_squared += distance * distance;
	}
	return sqrtf(distance_squared);
}

static int EvoBot_RouteBoundsIntersect(const evobot_bounds_t *a,
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

static int EvoBot_RouteCollectExitAreas(uint32_t **areas, size_t *count,
	int *interactor_id)
{
	evobot_nav_debug_summary_t summary;
	unsigned char *seen;
	size_t interactor_index;

	*areas = NULL;
	*count = 0;
	*interactor_id = 0;
	if (!EvoBot_NavDebugSummary(&summary) || !summary.present)
		return 0;
	seen = calloc(summary.area_count, 1);
	if (!seen && summary.area_count)
		return 0;
	for (interactor_index = 0; interactor_index < summary.interactor_count;
		interactor_index++)
	{
		evobot_nav_debug_interactor_t interactor;
		evobot_bounds_t access;
		size_t area_index;
		int axis;

		if (!EvoBot_NavDebugInteractor(interactor_index, &interactor) ||
			interactor.kind != EVOBOT_INTERACTOR_LEVEL_EXIT)
			continue;
		if (!*interactor_id)
			*interactor_id = (int)interactor.id;
		for (axis = 0; axis < 3; axis++)
		{
			access.mins.v[axis] = interactor.bounds.mins.v[axis] -
				summary.player_bounds.maxs.v[axis];
			access.maxs.v[axis] = interactor.bounds.maxs.v[axis] -
				summary.player_bounds.mins.v[axis];
		}
		for (area_index = 0; area_index < summary.area_count; area_index++)
		{
			evobot_nav_debug_area_t area;
			uint32_t *memory;

			if (seen[area_index] || !EvoBot_NavDebugArea(area_index, &area) ||
				!EvoBot_RouteAreaRouting(&area) ||
				!EvoBot_RouteBoundsIntersect(&area.bounds, &access))
				continue;
			memory = realloc(*areas, (*count + 1) * sizeof(**areas));
			if (!memory)
			{
				free(*areas);
				free(seen);
				*areas = NULL;
				*count = 0;
				return 0;
			}
			*areas = memory;
			(*areas)[(*count)++] = area.id;
			seen[area_index] = 1;
		}
	}
	free(seen);
	return *count != 0;
}

static evobot_nav_route_segment_kind_t EvoBot_RouteSegmentKind(
	evobot_dynamic_blocker_state_t state)
{
	if (state == EVOBOT_DYNAMIC_BLOCKER_BLOCKED)
		return EVOBOT_NAV_ROUTE_SEGMENT_BLOCKED;
	if (state == EVOBOT_DYNAMIC_BLOCKER_UNKNOWN)
		return EVOBOT_NAV_ROUTE_SEGMENT_CONDITIONAL;
	return EVOBOT_NAV_ROUTE_SEGMENT_NORMAL;
}

static int EvoBot_RouteReconstruct(const evobot_nav_cost_field_t *field,
	uint32_t destination)
{
	uint32_t area = destination;
	size_t count = 0;
	size_t i;

	free(evobot_route_steps);
	evobot_route_steps = NULL;
	if (!EvoBot_RouteFinite(field, destination))
		return 0;
	while (area != field->source_area)
	{
		uint32_t predecessor;
		evobot_nav_reachability_t reachability;

		if (!area || area > field->area_count || count >= field->area_count)
			return 0;
		predecessor = field->predecessors[area - 1];
		if (!predecessor || !EvoBot_NavDebugReachability(predecessor - 1,
			&reachability) || reachability.destination_area != area)
			return 0;
		area = reachability.source_area;
		count++;
	}
	if (count)
	{
		evobot_route_steps = calloc(count, sizeof(*evobot_route_steps));
		if (!evobot_route_steps)
			return 0;
	}
	area = destination;
	for (i = count; i > 0; i--)
	{
		uint32_t predecessor = field->predecessors[area - 1];
		evobot_nav_reachability_t reachability;
		evobot_nav_route_step_t *step = &evobot_route_steps[i - 1];

		if (!EvoBot_NavDebugReachability(predecessor - 1, &reachability))
			return 0;
		step->reachability_id = reachability.id;
		step->source_area = reachability.source_area;
		step->destination_area = reachability.destination_area;
		step->travel_type = reachability.travel_type;
		step->cumulative_cost = field->costs[area - 1];
		step->dynamic_interactor = reachability.dynamic_interactor;
		step->blocker_state = EvoBot_RouteBlockerState(&reachability);
		step->segment_kind = EvoBot_RouteSegmentKind(step->blocker_state);
		area = reachability.source_area;
	}
	evobot_route_summary.route_length = count;
	evobot_route_summary.total_cost = field->costs[destination - 1];
	return 1;
}

static void EvoBot_RouteCopyFieldSummary(const evobot_nav_cost_field_t *field)
{
	evobot_route_summary.nav_revision = field->nav_revision;
	evobot_route_summary.source_area = field->source_area;
	evobot_route_summary.reachable_area_count = field->reachable_area_count;
	evobot_route_summary.routing_area_count = field->routing_area_count;
	evobot_route_summary.reachability_count = field->reachability_count;
	evobot_route_summary.execution_time = field->execution_time;
	evobot_route_summary.result_memory = field->result_memory;
}

static evobot_nav_route_result_t EvoBot_RouteBlockedResult(void)
{
	int unknown = 0;
	size_t i;

	for (i = 0; i < evobot_route_summary.route_length; i++)
	{
		if (evobot_route_steps[i].blocker_state == EVOBOT_DYNAMIC_BLOCKER_BLOCKED)
		{
			if (EvoBot_RouteAutomaticDoor(
				evobot_route_steps[i].dynamic_interactor, NULL))
				unknown = 1;
			else
				return EVOBOT_NAV_ROUTE_BLOCKED;
		}
		if (evobot_route_steps[i].blocker_state == EVOBOT_DYNAMIC_BLOCKER_UNKNOWN)
			unknown = 1;
	}
	return unknown ? EVOBOT_NAV_ROUTE_CONDITIONAL : EVOBOT_NAV_ROUTE_UNREACHABLE;
}

static evobot_nav_route_result_t EvoBot_RouteSelectedResult(void)
{
	evobot_nav_route_result_t result = EvoBot_RouteBlockedResult();

	return result == EVOBOT_NAV_ROUTE_UNREACHABLE ?
		EVOBOT_NAV_ROUTE_REACHABLE : result;
}

static float EvoBot_RouteDistanceToGoals(uint32_t area_id,
	const uint32_t *goals, size_t goal_count)
{
	evobot_nav_debug_area_t area;
	float best = FLT_MAX;
	size_t i;

	if (!area_id || !EvoBot_NavDebugArea(area_id - 1, &area))
		return FLT_MAX;
	for (i = 0; i < goal_count; i++)
	{
		evobot_nav_debug_area_t goal;
		float distance;

		if (!goals[i] || !EvoBot_NavDebugArea(goals[i] - 1, &goal))
			continue;
		distance = EvoBot_RouteBoundsDistance(&area.bounds, &goal.bounds);
		if (distance < best)
			best = distance;
	}
	return best;
}

static void EvoBot_RouteFindFrontier(const evobot_nav_cost_field_t *field,
	const uint32_t *goals, size_t goal_count)
{
	float best_distance = FLT_MAX;
	float boundary_distance = FLT_MAX;
	float unresolved_distance = FLT_MAX;
	uint32_t best_area = 0;
	size_t i;

	{
		evobot_nav_debug_summary_t nav_summary;

		EvoBot_NavDebugSummary(&nav_summary);
		for (i = 0; i < nav_summary.portal_count; i++)
		{
			evobot_nav_debug_portal_t portal;
			evobot_nav_debug_area_t reachable_area;
			evobot_nav_debug_area_t gap_area;
			uint32_t reachable;
			uint32_t gap;
			float distance;

			if (!EvoBot_NavDebugPortal(i, &portal) ||
				EvoBot_RouteFinite(field, portal.area_a) ==
				EvoBot_RouteFinite(field, portal.area_b))
				continue;
			reachable = EvoBot_RouteFinite(field, portal.area_a) ?
				portal.area_a : portal.area_b;
			gap = reachable == portal.area_a ? portal.area_b : portal.area_a;
			if (!EvoBot_NavDebugArea(reachable - 1, &reachable_area) ||
				!EvoBot_NavDebugArea(gap - 1, &gap_area) ||
				!EvoBot_RouteAreaRouting(&gap_area))
				continue;
			distance = EvoBot_RouteDistanceToGoals(gap, goals, goal_count);
			if (distance < boundary_distance)
			{
				boundary_distance = distance;
				best_area = reachable;
				evobot_route_summary.frontier_gap_area = gap;
				evobot_route_summary.frontier_portal = portal.id;
				evobot_route_summary.frontier_portal_kind = portal.kind;
				evobot_route_summary.frontier_height_delta =
					gap_area.floor_height - reachable_area.floor_height;
			}
		}
	}
	for (i = 0; i < field->area_count; i++)
	{
		float distance;

		if (field->costs[i] >= FLT_MAX)
			continue;
		distance = EvoBot_RouteDistanceToGoals((uint32_t)i + 1,
			goals, goal_count);
		if (!best_area && distance < best_distance)
		{
			best_distance = distance;
			best_area = (uint32_t)i + 1;
		}
	}
	if (best_area)
		best_distance = EvoBot_RouteDistanceToGoals(best_area, goals, goal_count);
	evobot_route_summary.frontier_area = best_area;
	evobot_route_summary.frontier_distance = best_distance;
	if (best_area)
		EvoBot_RouteReconstruct(field, best_area);
	for (i = 0; i < field->reachability_count; i++)
	{
		evobot_nav_reachability_t reachability;
		float distance;

		if (!EvoBot_NavDebugReachability(i, &reachability) ||
			reachability.travel_type !=
			EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP ||
			!EvoBot_RouteFinite(field, reachability.source_area))
			continue;
		distance = EvoBot_RouteDistanceToGoals(reachability.destination_area,
			goals, goal_count);
		if (distance < unresolved_distance)
		{
			unresolved_distance = distance;
			evobot_route_summary.frontier_reachability = reachability.id;
		}
	}
	if (best_area)
	{
		evobot_nav_debug_area_t area;
		float interactor_distance = FLT_MAX;
		evobot_interactor_kind_t kind = EVOBOT_INTERACTOR_OTHER;
		int interactor = 0;
		evobot_nav_debug_summary_t nav_summary;

		EvoBot_NavDebugArea(best_area - 1, &area);
		EvoBot_NavDebugSummary(&nav_summary);
		for (i = 0; i < nav_summary.interactor_count; i++)
		{
			evobot_nav_debug_interactor_t candidate;
			float distance;

			if (!EvoBot_NavDebugInteractor(i, &candidate) ||
				candidate.kind == EVOBOT_INTERACTOR_LEVEL_EXIT)
				continue;
			distance = EvoBot_RouteBoundsDistance(&area.bounds,
				&candidate.swept_bounds);
			if (distance < interactor_distance)
			{
				interactor_distance = distance;
				interactor = (int)candidate.id;
				kind = candidate.kind;
			}
		}
		evobot_route_summary.frontier_interactor = interactor;
		evobot_route_summary.frontier_interactor_kind = kind;
		evobot_route_summary.frontier_interactor_distance = interactor_distance;
	}
	if (evobot_route_summary.frontier_reachability &&
		(unresolved_distance <= boundary_distance + 64.0f ||
		 !evobot_route_summary.frontier_portal))
		snprintf(evobot_route_summary.frontier_reason,
			sizeof(evobot_route_summary.frontier_reason),
			"unresolved water-jump reachability %" PRIu32,
			evobot_route_summary.frontier_reachability);
	else if (evobot_route_summary.frontier_portal)
	{
		const char *kind = evobot_route_summary.frontier_portal_kind ==
			EVOBOT_NAV_DEBUG_FACE_LEDGE ? "ledge" :
			evobot_route_summary.frontier_portal_kind ==
			EVOBOT_NAV_DEBUG_FACE_LIQUID ? "liquid" : "shared";

		evobot_route_summary.frontier_reachability = 0;

		if (evobot_route_summary.frontier_portal_kind ==
			EVOBOT_NAV_DEBUG_FACE_LEDGE &&
			evobot_route_summary.frontier_height_delta > 18.0f)
			snprintf(evobot_route_summary.frontier_reason,
				sizeof(evobot_route_summary.frontier_reason),
				"jump-up ledge candidate at portal %" PRIu32
				" to area %" PRIu32 " (%.1f units)",
				evobot_route_summary.frontier_portal,
				evobot_route_summary.frontier_gap_area,
				evobot_route_summary.frontier_height_delta);
		else
			snprintf(evobot_route_summary.frontier_reason,
				sizeof(evobot_route_summary.frontier_reason),
				"no validated reachability crosses %s portal %" PRIu32
				" to area %" PRIu32 " (height %.1f)", kind,
				evobot_route_summary.frontier_portal,
				evobot_route_summary.frontier_gap_area,
				evobot_route_summary.frontier_height_delta);
	}
	else if (evobot_route_summary.frontier_interactor &&
		evobot_route_summary.frontier_interactor_distance <= 128.0f &&
		(evobot_route_summary.frontier_interactor_kind ==
			EVOBOT_INTERACTOR_PLATFORM ||
		 evobot_route_summary.frontier_interactor_kind ==
			EVOBOT_INTERACTOR_TRAIN ||
		 evobot_route_summary.frontier_interactor_kind ==
			EVOBOT_INTERACTOR_TELEPORTER))
		snprintf(evobot_route_summary.frontier_reason,
			sizeof(evobot_route_summary.frontier_reason),
			"nearby unsupported interactor %d",
			evobot_route_summary.frontier_interactor);
	else if (best_area)
		snprintf(evobot_route_summary.frontier_reason,
			sizeof(evobot_route_summary.frontier_reason),
			"no implemented reachability closes %.1f-unit frontier gap",
			best_distance);
	else
		snprintf(evobot_route_summary.frontier_reason,
			sizeof(evobot_route_summary.frontier_reason),
			"no routing area is reachable from the source");
}

static int EvoBot_RouteSelectGoals(const uint32_t *goals, size_t goal_count)
{
	uint32_t destination = 0;
	float best_cost = FLT_MAX;
	size_t i;

	for (i = 0; i < goal_count; i++)
	{
		if (EvoBot_RouteFinite(&evobot_route_primary, goals[i]) &&
			evobot_route_primary.costs[goals[i] - 1] < best_cost)
		{
			best_cost = evobot_route_primary.costs[goals[i] - 1];
			destination = goals[i];
		}
	}
	if (destination)
	{
		evobot_route_summary.destination_area = destination;
		if (!EvoBot_RouteReconstruct(&evobot_route_primary, destination))
			return 0;
		evobot_route_summary.result = EvoBot_RouteSelectedResult();
		return 1;
	}
	if (evobot_route_primary.policy.mode == EVOBOT_NAV_ROUTE_CURRENT_WORLD &&
		EvoBot_RouteBuildAlternate())
	{
		best_cost = FLT_MAX;
		for (i = 0; i < goal_count; i++)
		{
			if (EvoBot_RouteFinite(&evobot_route_alternate, goals[i]) &&
				evobot_route_alternate.costs[goals[i] - 1] < best_cost)
			{
				best_cost = evobot_route_alternate.costs[goals[i] - 1];
				destination = goals[i];
			}
		}
	}
	if (destination)
	{
		evobot_route_summary.destination_area = destination;
		if (!EvoBot_RouteReconstruct(&evobot_route_alternate, destination))
			return 0;
		evobot_route_summary.result = EvoBot_RouteBlockedResult();
		evobot_route_summary.ignore_dynamic_reaches_destination = 1;
		return 1;
	}
	evobot_route_summary.destination_area = goal_count ? goals[0] : 0;
	evobot_route_summary.result = EVOBOT_NAV_ROUTE_UNREACHABLE;
	EvoBot_RouteFindFrontier(evobot_route_alternate.costs ?
		&evobot_route_alternate : &evobot_route_primary, goals, goal_count);
	return 1;
}

void EvoBot_NavRouteInit(const evobot_host_api_t *host)
{
	memset(&evobot_route_host, 0, sizeof(evobot_route_host));
	if (host)
		evobot_route_host = *host;
}

void EvoBot_NavRouteShutdown(void)
{
	EvoBot_RouteClearInternal();
	memset(&evobot_route_host, 0, sizeof(evobot_route_host));
}

int EvoBot_NavRouteBuild(uint32_t source_area,
	const evobot_nav_route_policy_t *policy)
{
	evobot_nav_route_policy_t default_policy;

	default_policy.mode = EVOBOT_NAV_ROUTE_CURRENT_WORLD;
	default_policy.cost = NULL;
	default_policy.user_data = NULL;
	if (!policy)
		policy = &default_policy;
	if (policy->mode != EVOBOT_NAV_ROUTE_CURRENT_WORLD &&
		policy->mode != EVOBOT_NAV_ROUTE_IGNORE_DYNAMIC_BLOCKERS)
		return 0;
	EvoBot_RouteClearInternal();
	if (!EvoBot_RouteBuildField(&evobot_route_primary, source_area, policy))
	{
		EvoBot_RoutePrint("EvoBot routing: could not build cost field\n");
		return 0;
	}
	EvoBot_RouteCopyFieldSummary(&evobot_route_primary);
	EvoBot_RouteTouch();
	return 1;
}

int EvoBot_NavRouteCost(uint32_t area, evobot_nav_cost_query_t *query)
{
	if (!query || !EvoBot_RouteSync() || !area ||
		area > evobot_route_primary.area_count)
		return 0;
	query->area = area;
	query->cost = evobot_route_primary.costs[area - 1];
	query->predecessor_reachability =
		evobot_route_primary.predecessors[area - 1];
	return query->cost < FLT_MAX;
}

int EvoBot_NavRouteSelectArea(uint32_t destination_area)
{
	if (!EvoBot_RouteSync() || !destination_area ||
		destination_area > evobot_route_primary.area_count)
		return 0;
	evobot_route_summary.exit_area_count = 0;
	evobot_route_summary.exit_interactor = 0;
	if (!EvoBot_RouteSelectGoals(&destination_area, 1))
		return 0;
	EvoBot_RouteTouch();
	return 1;
}

int EvoBot_NavRouteSelectExit(void)
{
	uint32_t *areas;
	size_t count;
	int interactor;
	int result;

	if (!EvoBot_RouteSync())
		return 0;
	if (!EvoBot_RouteCollectExitAreas(&areas, &count, &interactor))
	{
		EvoBot_RoutePrint("EvoBot routing: no accessible level-exit areas found\n");
		return 0;
	}
	evobot_route_summary.exit_area_count = count;
	evobot_route_summary.exit_interactor = interactor;
	result = EvoBot_RouteSelectGoals(areas, count);
	free(areas);
	if (result)
		EvoBot_RouteTouch();
	return result;
}

int EvoBot_NavRouteFirst(evobot_nav_route_step_t *step)
{
	return EvoBot_NavRouteNext(0, step);
}

int EvoBot_NavRouteNext(size_t index, evobot_nav_route_step_t *step)
{
	if (!step || !EvoBot_RouteSync() ||
		index >= evobot_route_summary.route_length)
		return 0;
	*step = evobot_route_steps[index];
	return 1;
}

void EvoBot_NavRouteClear(void)
{
	EvoBot_RouteClearInternal();
}

static const char *EvoBot_RouteResultName(evobot_nav_route_result_t result)
{
	switch (result)
	{
	case EVOBOT_NAV_ROUTE_REACHABLE: return "reachable";
	case EVOBOT_NAV_ROUTE_BLOCKED: return "blocked";
	case EVOBOT_NAV_ROUTE_CONDITIONAL: return "conditional";
	case EVOBOT_NAV_ROUTE_UNREACHABLE: return "unreachable";
	case EVOBOT_NAV_ROUTE_NONE:
	default: return "none";
	}
}

static const char *EvoBot_RouteInteractorName(evobot_interactor_kind_t kind)
{
	switch (kind)
	{
	case EVOBOT_INTERACTOR_DOOR: return "door";
	case EVOBOT_INTERACTOR_BUTTON: return "button";
	case EVOBOT_INTERACTOR_PLATFORM: return "platform";
	case EVOBOT_INTERACTOR_TRAIN: return "train";
	case EVOBOT_INTERACTOR_TELEPORTER: return "teleporter";
	case EVOBOT_INTERACTOR_TELEPORT_DESTINATION: return "teleport destination";
	case EVOBOT_INTERACTOR_LEVEL_EXIT: return "level exit";
	case EVOBOT_INTERACTOR_TRIGGER: return "trigger";
	case EVOBOT_INTERACTOR_LOGIC: return "logic";
	case EVOBOT_INTERACTOR_OTHER:
	default: return "other";
	}
}

static const char *EvoBot_RouteActivationName(
	evobot_interactor_activation_t activation)
{
	switch (activation)
	{
	case EVOBOT_ACTIVATION_NONE: return "none";
	case EVOBOT_ACTIVATION_APPROACH: return "approach";
	case EVOBOT_ACTIVATION_TOUCH: return "touch";
	case EVOBOT_ACTIVATION_SHOOT: return "shoot";
	case EVOBOT_ACTIVATION_EXTERNAL: return "external";
	case EVOBOT_ACTIVATION_UNKNOWN:
	default: return "unknown";
	}
}

static void EvoBot_RoutePrintSamples(void)
{
	uint32_t selected[3] = { 0, 0, 0 };
	size_t printed = 0;
	size_t i;
	int pass;

	EvoBot_RoutePrint("sample costs from the same field:\n");
	for (pass = 0; pass < 3 && printed < 3; pass++)
	{
		for (i = 0; i < evobot_route_primary.area_count && printed < 3; i++)
		{
			evobot_nav_debug_area_t area;
			size_t previous;
			int finite = evobot_route_primary.costs[i] < FLT_MAX;

			if (!EvoBot_NavDebugArea(i, &area) ||
				area.id == evobot_route_primary.source_area ||
				(pass == 0 && (!finite || area.dynamic_interactor <= 0)) ||
				(pass == 1 && !finite) ||
				(pass == 2 && (finite || area.dynamic_interactor <= 0)))
				continue;
			for (previous = 0; previous < printed; previous++)
			{
				if (selected[previous] == area.id)
					break;
			}
			if (previous < printed)
				continue;
			selected[printed++] = area.id;
			if (finite && area.dynamic_interactor > 0)
				EvoBot_RoutePrintf("  area %" PRIu32
					" (interactor %d): %.3f s\n", area.id,
					area.dynamic_interactor, evobot_route_primary.costs[i]);
			else if (finite)
				EvoBot_RoutePrintf("  area %" PRIu32 ": %.3f s\n",
					area.id, evobot_route_primary.costs[i]);
			else
				EvoBot_RoutePrintf("  area %" PRIu32
					" (interactor %d): unreachable\n", area.id,
					area.dynamic_interactor);
		}
	}
	if (!printed)
		EvoBot_RoutePrint("  no interactor-associated routing areas\n");
}

static void EvoBot_RoutePrintBoundaryAudit(void)
{
	const evobot_nav_cost_field_t *field = evobot_route_alternate.costs ?
		&evobot_route_alternate : &evobot_route_primary;
	evobot_nav_debug_summary_t summary;
	size_t total = 0;
	size_t ledge = 0;
	size_t liquid = 0;
	size_t no_direction = 0;
	size_t no_interval = 0;
	size_t no_support = 0;
	size_t no_containment = 0;
	size_t no_overlap = 0;
	size_t movement = 0;
	size_t entered_liquid = 0;
	size_t other = 0;
	size_t missed_link = 0;
	uint32_t movement_portal = 0;
	uint32_t missed_portal = 0;
	size_t i;

	if (!EvoBot_NavDebugSummary(&summary))
		return;
	for (i = 0; i < summary.portal_count; i++)
	{
		evobot_nav_debug_portal_t portal;
		evobot_nav_debug_walk_candidate_t walk;
		uint32_t source;
		uint32_t destination;
		int reachable_a;
		int reachable_b;

		if (!EvoBot_NavDebugPortal(i, &portal) || !portal.area_a ||
			!portal.area_b || portal.area_a > field->area_count ||
			portal.area_b > field->area_count)
			continue;
		reachable_a = EvoBot_RouteFinite(field, portal.area_a);
		reachable_b = EvoBot_RouteFinite(field, portal.area_b);
		if (reachable_a == reachable_b)
			continue;
		total++;
		if (portal.kind == EVOBOT_NAV_DEBUG_FACE_LEDGE)
		{
			ledge++;
			continue;
		}
		if (portal.kind == EVOBOT_NAV_DEBUG_FACE_LIQUID)
		{
			liquid++;
			continue;
		}
		source = reachable_a ? portal.area_a : portal.area_b;
		destination = reachable_a ? portal.area_b : portal.area_a;
		if (!EvoBot_NavDebugWalkCandidate(portal.id, source, destination, &walk))
		{
			other++;
			continue;
		}
		if (!walk.direction_valid)
			no_direction++;
		else if (!walk.portal_interval_count)
			no_interval++;
		else if (!walk.source_support_samples ||
			!walk.destination_support_samples)
			no_support++;
		else if (!walk.overlap_samples &&
			(!walk.source_in_area || !walk.destination_in_area))
			no_containment++;
		else if (!walk.overlap_samples)
			no_overlap++;
		else if (walk.final_walk)
		{
			missed_link++;
			if (!missed_portal)
				missed_portal = portal.id;
		}
		else if (walk.reached_destination && walk.result.state.water_level >= 2)
			entered_liquid++;
		else if (!walk.reached_destination)
		{
			movement++;
			if (!movement_portal)
				movement_portal = portal.id;
		}
		else
			other++;
	}
	EvoBot_RoutePrintf("boundary audit: %zu portals (ledge %zu, liquid %zu, "
		"direction %zu, interval %zu, support %zu, containment %zu, "
		"overlap %zu, movement %zu, entered liquid %zu, other %zu, "
		"missing generated link %zu)\n", total, ledge, liquid, no_direction,
		no_interval, no_support, no_containment, no_overlap, movement,
		entered_liquid, other, missed_link);
	if (movement_portal || missed_portal)
		EvoBot_RoutePrintf("boundary audit samples: movement portal %" PRIu32
			", missing-link portal %" PRIu32 "\n", movement_portal,
			missed_portal);
}

void EvoBot_NavRoutePrintStatus(void)
{
	size_t i;

	if (!EvoBot_RouteSync())
	{
		EvoBot_RoutePrint("EvoBot route status\nresult: none\n");
		return;
	}
	EvoBot_RoutePrintf("EvoBot route status\nsource area: %" PRIu32
		"\ndestination area: %" PRIu32 "\nresult: %s\n",
		evobot_route_summary.source_area, evobot_route_summary.destination_area,
		EvoBot_RouteResultName(evobot_route_summary.result));
	if (evobot_route_summary.result == EVOBOT_NAV_ROUTE_REACHABLE ||
		evobot_route_summary.result == EVOBOT_NAV_ROUTE_BLOCKED ||
		evobot_route_summary.result == EVOBOT_NAV_ROUTE_CONDITIONAL)
		EvoBot_RoutePrintf("cost: %.3f s\nreachabilities: %zu\n",
			evobot_route_summary.total_cost, evobot_route_summary.route_length);
	EvoBot_RoutePrintf("reachable routing areas: %zu / %zu\n"
		"graph reachabilities: %zu\ndijkstra time: %.6f seconds\n"
		"cost/predecessor memory: %zu bytes\n",
		evobot_route_summary.reachable_area_count,
		evobot_route_summary.routing_area_count,
		evobot_route_summary.reachability_count,
		evobot_route_summary.execution_time,
		evobot_route_summary.result_memory);
	if (evobot_route_summary.ignore_dynamic_evaluated)
		EvoBot_RoutePrintf("ignoring dynamic blockers: %s (%zu reachable areas)\n",
			evobot_route_summary.ignore_dynamic_reaches_destination ?
				"destination reachable" : "destination unreachable",
			evobot_route_summary.ignore_dynamic_reachable_area_count);
	if (evobot_route_summary.exit_area_count)
		EvoBot_RoutePrintf("exit interactor: %d\nexit areas: %zu\n",
			evobot_route_summary.exit_interactor,
			evobot_route_summary.exit_area_count);
	for (i = 0; i < evobot_route_summary.route_length; i++)
	{
		const evobot_nav_route_step_t *step = &evobot_route_steps[i];

		if (step->blocker_state == EVOBOT_DYNAMIC_BLOCKER_BLOCKED)
			EvoBot_RoutePrintf("blocked by interactor %d at reachability %" PRIu32
				"\n", step->dynamic_interactor, step->reachability_id);
		else if (step->blocker_state == EVOBOT_DYNAMIC_BLOCKER_UNKNOWN)
			EvoBot_RoutePrintf("conditional interactor %d at reachability %" PRIu32
				"\n", step->dynamic_interactor, step->reachability_id);
	}
	if (evobot_route_summary.result == EVOBOT_NAV_ROUTE_UNREACHABLE)
	{
		if (evobot_route_summary.frontier_portal)
		{
			evobot_nav_debug_walk_candidate_t walk;
			const char *problem_type =
				!strncmp(evobot_route_summary.frontier_reason,
					"jump-up ledge candidate", 23) ?
					"jump-up ledge" : "missing reachability";

			EvoBot_RoutePrintf("problem type: %s\n"
				"frontier area: %" PRIu32 "\nadjacent area: %" PRIu32
				"\nportal: %" PRIu32 "\n",
				problem_type,
				evobot_route_summary.frontier_area,
				evobot_route_summary.frontier_gap_area,
				evobot_route_summary.frontier_portal);
			if (EvoBot_NavDebugWalkCandidate(
				evobot_route_summary.frontier_portal,
				evobot_route_summary.frontier_area,
				evobot_route_summary.frontier_gap_area, &walk))
				EvoBot_RoutePrintf("walk candidate: present %d, direction %d, "
					"projected %d, intervals %u, samples %u\n"
					"walk support: source %u, destination %u, overlap %u, "
					"valid %d/%d, inside %d/%d, height %.3f / %.3f\n"
					"walk movement: moved %d, reached %d, segment %d, hull %d, grounded %d, "
					"water level %d, supported arrival %d, water jump %d, "
					"pm %d, final %d\n"
					"walk points: direction [%.2f %.2f %.2f], "
					"start [%.2f %.2f %.2f], wanted [%.2f %.2f %.2f], "
					"result [%.2f %.2f %.2f]\n",
					walk.present, walk.direction_valid,
					walk.projected_interval_direction,
					walk.portal_interval_count, walk.sample_count,
					walk.source_support_samples,
					walk.destination_support_samples, walk.overlap_samples,
					walk.source_valid, walk.destination_valid,
					walk.source_in_area, walk.destination_in_area,
					walk.height_delta, walk.step_limit,
					walk.movement_succeeded, walk.reached_destination,
					walk.segment_reached_destination,
					walk.hull_crossed_portal,
					walk.result.state.on_ground, walk.result.state.water_level,
					walk.supported_arrival, walk.water_jump,
					walk.pm_validation_passed, walk.final_walk,
					walk.direction.v[0], walk.direction.v[1], walk.direction.v[2],
					walk.start_origin.v[0], walk.start_origin.v[1],
					walk.start_origin.v[2], walk.desired_destination.v[0],
					walk.desired_destination.v[1],
					walk.desired_destination.v[2], walk.result.state.origin.v[0],
					walk.result.state.origin.v[1],
					walk.result.state.origin.v[2]);
		}
		EvoBot_RoutePrintf("best frontier area: %" PRIu32
			"\nfrontier distance: %.1f\nfrontier reason: %s\n",
			evobot_route_summary.frontier_area,
			evobot_route_summary.frontier_distance,
			evobot_route_summary.frontier_reason);
		if (evobot_route_summary.frontier_portal)
			EvoBot_RoutePrintf("frontier portal: %" PRIu32
				"\narea beyond frontier: %" PRIu32 "\n",
				evobot_route_summary.frontier_portal,
				evobot_route_summary.frontier_gap_area);
		if (evobot_route_summary.frontier_interactor)
			EvoBot_RoutePrintf("nearby interactor: %d (%s, %.1f units)\n",
				evobot_route_summary.frontier_interactor,
				EvoBot_RouteInteractorName(
					evobot_route_summary.frontier_interactor_kind),
				evobot_route_summary.frontier_interactor_distance);
		EvoBot_RoutePrintBoundaryAudit();
	}
	EvoBot_RoutePrintSamples();
}

void EvoBot_NavRoutePrintCost(uint32_t area)
{
	evobot_nav_cost_query_t query;

	if (!EvoBot_RouteSync())
	{
		EvoBot_RoutePrint("EvoBot routing: no cost field is selected\n");
		return;
	}
	if (!area || area > evobot_route_primary.area_count)
	{
		EvoBot_RoutePrintf("EvoBot routing: area %" PRIu32 " is invalid\n", area);
		return;
	}
	if (!EvoBot_NavRouteCost(area, &query))
	{
		EvoBot_RoutePrintf("EvoBot cost from area %" PRIu32 " to area %" PRIu32
			": unreachable\n", evobot_route_primary.source_area, area);
		return;
	}
	EvoBot_RoutePrintf("EvoBot cost from area %" PRIu32 " to area %" PRIu32
		": %.3f s (predecessor reachability %" PRIu32 ")\n",
		evobot_route_primary.source_area, area, query.cost,
		query.predecessor_reachability);
}

void EvoBot_NavRouteValidate(void)
{
	size_t errors = 0;
	size_t i;

	if (!EvoBot_RouteSync())
	{
		EvoBot_RoutePrint("EvoBot routing validation: no cost field is selected\n");
		return;
	}
	if (evobot_route_primary.costs[evobot_route_primary.source_area - 1] != 0)
		errors++;
	for (i = 0; i < evobot_route_primary.area_count; i++)
	{
		uint32_t predecessor;
		evobot_nav_reachability_t reachability;
		float edge_cost;
		float expected;
		float tolerance;

		if (evobot_route_primary.costs[i] >= FLT_MAX ||
			i + 1 == evobot_route_primary.source_area)
			continue;
		predecessor = evobot_route_primary.predecessors[i];
		if (!predecessor || !EvoBot_NavDebugReachability(predecessor - 1,
			&reachability) || reachability.destination_area != i + 1 ||
			!EvoBot_RouteFinite(&evobot_route_primary,
				reachability.source_area))
		{
			errors++;
			continue;
		}
		edge_cost = reachability.base_travel_time;
		if (evobot_route_primary.policy.mode == EVOBOT_NAV_ROUTE_CURRENT_WORLD &&
			EvoBot_RouteBlockerState(&reachability) !=
				EVOBOT_DYNAMIC_BLOCKER_CLEAR)
		{
			float opening_delay;
			if (!EvoBot_RouteAutomaticDoor(reachability.dynamic_interactor,
				&opening_delay))
			{
				errors++;
				continue;
			}
			edge_cost += opening_delay;
		}
		if (evobot_route_primary.policy.cost &&
			!evobot_route_primary.policy.cost(&reachability,
				evobot_route_primary.costs[reachability.source_area - 1],
				reachability.base_travel_time, &edge_cost,
				evobot_route_primary.policy.user_data))
		{
			errors++;
			continue;
		}
		expected = evobot_route_primary.costs[reachability.source_area - 1] +
			edge_cost;
		tolerance = 0.0001f * (1.0f + expected);
		if (edge_cost < 0 || fabsf(evobot_route_primary.costs[i] - expected) >
			tolerance)
			errors++;
	}
	if (errors)
		EvoBot_RoutePrintf("EvoBot routing validation: failed (%zu errors)\n",
			errors);
	else
		EvoBot_RoutePrintf("EvoBot routing validation: ok (%zu reachable areas, "
			"directed predecessor chains consistent)\n",
			evobot_route_primary.reachable_area_count);
}

static uint32_t EvoBot_PlanReachableActivationArea(
	const evobot_nav_debug_interactor_t *interactor,
	const evobot_nav_cost_field_t *field)
{
	evobot_nav_debug_summary_t summary;
	evobot_bounds_t access;
	uint32_t best_area = 0;
	float best_cost = FLT_MAX;
	size_t i;
	int axis;

	if (!EvoBot_NavDebugSummary(&summary))
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		access.mins.v[axis] = interactor->bounds.mins.v[axis] -
			summary.player_bounds.maxs.v[axis] - 16.0f;
		access.maxs.v[axis] = interactor->bounds.maxs.v[axis] -
			summary.player_bounds.mins.v[axis] + 16.0f;
	}
	for (i = 0; i < summary.area_count; i++)
	{
		evobot_nav_debug_area_t area;

		if (EvoBot_NavDebugArea(i, &area) && EvoBot_RouteAreaRouting(&area) &&
			EvoBot_RouteFinite(field, area.id) &&
			EvoBot_RouteBoundsIntersect(&area.bounds, &access) &&
			field->costs[area.id - 1] < best_cost)
		{
			best_area = area.id;
			best_cost = field->costs[area.id - 1];
		}
	}
	return best_area;
}

static int EvoBot_PlanFindActivator(uint32_t affected,
	evobot_nav_plan_subgoal_t *subgoal, char *reason, size_t reason_size)
{
	evobot_nav_debug_summary_t summary;
	uint32_t *queue;
	unsigned char *visited;
	size_t head = 0;
	size_t tail = 0;
	int saw_shoot = 0;
	size_t i;

	if (!EvoBot_NavDebugSummary(&summary) || !summary.interactor_count)
		return 0;
	queue = malloc(summary.interactor_count * sizeof(*queue));
	visited = calloc(summary.interactor_count, 1);
	if (!queue || !visited)
	{
		free(queue);
		free(visited);
		return 0;
	}
	queue[tail++] = affected;
	while (head < tail)
	{
		uint32_t wanted = queue[head++];
		size_t activation_count = EvoBot_NavDebugActivationCount();

		if (!wanted || wanted > summary.interactor_count || visited[wanted - 1])
			continue;
		visited[wanted - 1] = 1;
		for (i = 0; i < activation_count; i++)
		{
			evobot_nav_debug_activation_t activation;
			evobot_nav_debug_interactor_t source;
			uint32_t area;

			if (!EvoBot_NavDebugActivation(i, &activation) ||
				activation.destination_interactor != wanted ||
				!EvoBot_NavDebugInteractor(
					activation.source_interactor - 1, &source))
				continue;
			if (source.kind == EVOBOT_INTERACTOR_BUTTON ||
				source.kind == EVOBOT_INTERACTOR_TRIGGER)
			{
				if (source.activation == EVOBOT_ACTIVATION_SHOOT)
				{
					saw_shoot = 1;
					continue;
				}
				area = EvoBot_PlanReachableActivationArea(&source,
					&evobot_route_primary);
				if (area)
				{
					memset(subgoal, 0, sizeof(*subgoal));
					subgoal->activator = source.id;
					subgoal->affected = affected;
					subgoal->area = area;
					subgoal->activation = source.activation;
					free(queue);
					free(visited);
					return 1;
				}
			}
			else if (tail < summary.interactor_count &&
				!visited[activation.source_interactor - 1])
				queue[tail++] = activation.source_interactor;
		}
	}
	snprintf(reason, reason_size, "%s", saw_shoot ?
		"only shoot activation is known" :
		"no reachable touch/approach activator");
	free(queue);
	free(visited);
	return 0;
}

static int EvoBot_PlanGoalPresent(uint32_t activator, uint32_t affected)
{
	size_t i;
	for (i = 0; i < evobot_nav_plan.subgoal_count; i++)
		if (evobot_nav_plan.subgoals[i].activator == activator &&
			evobot_nav_plan.subgoals[i].affected == affected)
			return 1;
	return 0;
}

static int EvoBot_PlanAppendGoal(const evobot_nav_debug_interactor_t *source,
	uint32_t affected, uint32_t area)
{
	evobot_nav_plan_subgoal_t *goal;
	evobot_nav_debug_interactor_t interactor;

	if (EvoBot_PlanGoalPresent(source->id, affected))
		return 1;
	if (evobot_nav_plan.subgoal_count >= EVOBOT_NAV_PLAN_MAX_SUBGOALS)
		return 0;
	goal = &evobot_nav_plan.subgoals[evobot_nav_plan.subgoal_count++];
	memset(goal, 0, sizeof(*goal));
	goal->activator = source->id;
	goal->affected = affected;
	goal->area = area;
	goal->activation = source->activation;
	if (EvoBot_NavDebugInteractor(affected - 1, &interactor))
		evobot_nav_plan.dynamic_wait += interactor.travel_time;
	return 1;
}

static int EvoBot_PlanResolveAffected(uint32_t affected,
	unsigned char *dependency_state, size_t dependency_count,
	char *reason, size_t reason_size)
{
	uint32_t *queue;
	unsigned char *seen;
	size_t head = 0;
	size_t tail = 0;
	size_t i;
	int saw_shoot = 0;

	if (!affected || affected > dependency_count)
		return 0;
	if (dependency_state[affected - 1] == 2)
		return 1;
	if (dependency_state[affected - 1] == 1)
	{
		snprintf(reason, reason_size, "cyclic activation dependency");
		return 0;
	}
	dependency_state[affected - 1] = 1;
	queue = malloc(dependency_count * sizeof(*queue));
	seen = calloc(dependency_count, 1);
	if (!queue || !seen)
	{
		free(queue);
		free(seen);
		dependency_state[affected - 1] = 0;
		return 0;
	}
	queue[tail++] = affected;
	while (head < tail)
	{
		uint32_t wanted = queue[head++];
		size_t activation_count = EvoBot_NavDebugActivationCount();

		if (!wanted || wanted > dependency_count || seen[wanted - 1])
			continue;
		seen[wanted - 1] = 1;
		for (i = 0; i < activation_count; i++)
		{
			evobot_nav_debug_activation_t activation;
			evobot_nav_debug_interactor_t source;
			uint32_t area;

			if (!EvoBot_NavDebugActivation(i, &activation) ||
				activation.destination_interactor != wanted ||
				!EvoBot_NavDebugInteractor(activation.source_interactor - 1,
					&source))
				continue;
			if (source.kind != EVOBOT_INTERACTOR_BUTTON &&
				source.kind != EVOBOT_INTERACTOR_TRIGGER)
			{
				if (tail < dependency_count &&
					!seen[activation.source_interactor - 1])
					queue[tail++] = activation.source_interactor;
				continue;
			}
			if (source.activation == EVOBOT_ACTIVATION_SHOOT)
			{
				saw_shoot = 1;
				continue;
			}
			area = EvoBot_PlanReachableActivationArea(&source,
				&evobot_route_primary);
			if (area && EvoBot_PlanAppendGoal(&source, affected, area))
			{
				dependency_state[affected - 1] = 2;
				free(queue);
				free(seen);
				return 1;
			}
			if (evobot_route_alternate.costs)
			{
				uint32_t cursor;
				size_t guard = 0;
				int path_ok = 1;

				area = EvoBot_PlanReachableActivationArea(&source,
					&evobot_route_alternate);
				cursor = area;
				while (cursor && cursor != evobot_route_alternate.source_area &&
					guard++ < evobot_route_alternate.area_count)
				{
					uint32_t predecessor =
						evobot_route_alternate.predecessors[cursor - 1];
					evobot_nav_reachability_t reachability;
					if (!predecessor || !EvoBot_NavDebugReachability(predecessor - 1,
						&reachability))
					{
						path_ok = 0;
						break;
					}
					if (reachability.dynamic_interactor > 0 &&
						EvoBot_RouteBlockerState(&reachability) !=
							EVOBOT_DYNAMIC_BLOCKER_CLEAR &&
						!EvoBot_RouteAutomaticDoor(
							reachability.dynamic_interactor, NULL) &&
						!EvoBot_PlanResolveAffected(
							(uint32_t)reachability.dynamic_interactor,
							dependency_state, dependency_count, reason, reason_size))
					{
						path_ok = 0;
						break;
					}
					cursor = reachability.source_area;
				}
				if (area && path_ok && cursor == evobot_route_alternate.source_area &&
					EvoBot_PlanAppendGoal(&source, affected, area))
				{
					dependency_state[affected - 1] = 2;
					free(queue);
					free(seen);
					return 1;
				}
			}
		}
	}
	dependency_state[affected - 1] = 0;
	snprintf(reason, reason_size, "%s", saw_shoot ?
		"only shoot activation is known" :
		"no reachable touch/approach activator");
	free(queue);
	free(seen);
	return 0;
}

typedef struct evobot_nav_plan_world_s
{
	unsigned char *available;
	unsigned char *activated;
	unsigned char *logic_active;
	unsigned char *has_activator;
	unsigned short *counter_progress;
	float *available_from;
	float *available_until;
	size_t interactor_count;
	int conditional;
	int cycle;
	size_t searched_states;
	size_t timed_states;
	size_t counter_states;
	uint32_t current_area;
	float navigation_cost;
	float elapsed_time;
} evobot_nav_plan_world_t;

static int EvoBot_PlanWorldCost(const evobot_nav_reachability_t *reachability,
	float source_cost, float base_cost, float *cost, void *user_data)
{
	evobot_nav_plan_world_t *world = user_data;
	evobot_nav_debug_interactor_t mover;
	int timed_interactor = 0;
	float delay;

	*cost = base_cost;
	if ((reachability->flags & EVOBOT_NAV_REACH_DYNAMIC_BLOCKER) &&
		reachability->dynamic_interactor > 0)
	{
		int interactor = reachability->dynamic_interactor;

		if (EvoBot_RouteBlockerState(reachability) ==
			EVOBOT_DYNAMIC_BLOCKER_CLEAR)
			return 1;
		if (EvoBot_RouteAutomaticDoor(interactor, &delay))
		{
			*cost += delay;
			return 1;
		}
		if ((size_t)interactor > world->interactor_count ||
			!world->available[interactor - 1])
			return 0;
		timed_interactor = interactor;
	}
	if (reachability->travel_type == EVOBOT_NAV_TRAVEL_PLATFORM &&
		reachability->mover_interactor > 0 &&
		EvoBot_NavDebugInteractor(reachability->mover_interactor - 1, &mover) &&
		mover.activation == EVOBOT_ACTIVATION_EXTERNAL &&
		((size_t)reachability->mover_interactor > world->interactor_count ||
		 !world->available[reachability->mover_interactor - 1]))
		return 0;
	else if (reachability->travel_type == EVOBOT_NAV_TRAVEL_PLATFORM &&
		reachability->mover_interactor > 0)
		timed_interactor = reachability->mover_interactor;
	if (timed_interactor > 0 &&
		(size_t)timed_interactor <= world->interactor_count)
	{
		float arrival = world->elapsed_time + source_cost;
		float usable_from = world->available_from[timed_interactor - 1];
		float usable_until = world->available_until[timed_interactor - 1];

		if (arrival < usable_from)
		{
			*cost += usable_from - arrival;
			arrival = usable_from;
		}
		if (arrival + base_cost > usable_until + 0.001f)
			return 0;
	}
	return 1;
}

static int EvoBot_PlanBuildWorldField(evobot_nav_cost_field_t *field,
	uint32_t source_area, evobot_nav_plan_world_t *world)
{
	evobot_nav_route_policy_t policy;

	memset(&policy, 0, sizeof(policy));
	policy.mode = EVOBOT_NAV_ROUTE_IGNORE_DYNAMIC_BLOCKERS;
	policy.cost = EvoBot_PlanWorldCost;
	policy.user_data = world;
	world->searched_states++;
	return EvoBot_RouteBuildField(field, source_area, &policy);
}

static void EvoBot_PlanSetAvailable(
	const evobot_nav_debug_interactor_t *interactor,
	evobot_nav_plan_world_t *world)
{
	float usable_from;
	float usable_until = FLT_MAX;

	if (!interactor->id || interactor->id > world->interactor_count)
		return;
	usable_from = world->elapsed_time +
		(interactor->travel_time > 0 ? interactor->travel_time : 0);
	if (interactor->lifetime == EVOBOT_INTERACTOR_TEMPORARY)
	{
		usable_until = interactor->wait > 0 ?
			usable_from + interactor->wait : usable_from;
		world->timed_states++;
	}
	world->available[interactor->id - 1] = 1;
	world->available_from[interactor->id - 1] = usable_from;
	world->available_until[interactor->id - 1] = usable_until;
}

static int EvoBot_PlanSourceAffectsRecursive(uint32_t source_id,
	uint32_t wanted, unsigned char *visited, size_t count)
{
	evobot_nav_debug_interactor_t source;
	size_t i;

	if (!source_id || source_id > count || visited[source_id - 1])
		return 0;
	if (source_id == wanted)
		return 1;
	visited[source_id - 1] = 1;
	if (EvoBot_NavDebugInteractor(source_id - 1, &source) &&
		source.killtarget[0])
	{
		evobot_nav_debug_interactor_t destination;
		if (EvoBot_NavDebugInteractor(wanted - 1, &destination) &&
			destination.targetname[0] &&
			!strcmp(source.killtarget, destination.targetname))
			return 1;
	}
	for (i = 0; i < EvoBot_NavDebugActivationCount(); i++)
	{
		evobot_nav_debug_activation_t activation;
		if (EvoBot_NavDebugActivation(i, &activation) &&
			activation.source_interactor == source_id &&
			(activation.destination_interactor == wanted ||
			 EvoBot_PlanSourceAffectsRecursive(
				activation.destination_interactor, wanted, visited, count)))
			return 1;
	}
	return 0;
}

static int EvoBot_PlanSourceAffects(uint32_t source_id, uint32_t wanted,
	size_t count)
{
	unsigned char *visited;
	int result;

	if (!wanted)
		return 0;
	visited = calloc(count, 1);
	if (!visited)
		return 0;
	result = EvoBot_PlanSourceAffectsRecursive(source_id, wanted, visited, count);
	free(visited);
	return result;
}

static int EvoBot_PlanHasActivator(uint32_t wanted, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
	{
		evobot_nav_debug_interactor_t source;
		if (!EvoBot_NavDebugInteractor(i, &source))
			continue;
		if ((source.kind == EVOBOT_INTERACTOR_BUTTON ||
			 source.kind == EVOBOT_INTERACTOR_TRIGGER) &&
			(source.target[0] || source.killtarget[0]) &&
			EvoBot_PlanSourceAffects(source.id, wanted, count))
			return 1;
		if (source.id == wanted &&
			((source.kind == EVOBOT_INTERACTOR_DOOR &&
			  source.activation == EVOBOT_ACTIVATION_SHOOT) ||
			 (source.kind == EVOBOT_INTERACTOR_PLATFORM &&
			  source.activation != EVOBOT_ACTIVATION_EXTERNAL)))
			return 1;
	}
	return 0;
}

static int EvoBot_PlanOptimisticCost(
	const evobot_nav_reachability_t *reachability, float source_cost,
	float base_cost, float *cost, void *user_data)
{
	evobot_nav_plan_world_t *world = user_data;
	uint32_t interactor = 0;
	(void)source_cost;
	*cost = base_cost;

	if ((reachability->flags & EVOBOT_NAV_REACH_DYNAMIC_BLOCKER) &&
		reachability->dynamic_interactor > 0 &&
		EvoBot_RouteBlockerState(reachability) != EVOBOT_DYNAMIC_BLOCKER_CLEAR &&
		!EvoBot_RouteAutomaticDoor(reachability->dynamic_interactor, NULL))
		interactor = (uint32_t)reachability->dynamic_interactor;
	if (reachability->travel_type == EVOBOT_NAV_TRAVEL_PLATFORM &&
		reachability->mover_interactor > 0)
	{
		evobot_nav_debug_interactor_t mover;
		if (EvoBot_NavDebugInteractor(
			reachability->mover_interactor - 1, &mover) &&
			mover.activation == EVOBOT_ACTIVATION_EXTERNAL)
			interactor = (uint32_t)reachability->mover_interactor;
	}
	if (!interactor || (interactor <= world->interactor_count &&
		world->available[interactor - 1]))
		return 1;
	if (interactor > world->interactor_count ||
		!world->has_activator[interactor - 1])
		return 0;
	*cost += 1.0f;
	return 1;
}

static int EvoBot_PlanBuildOptimisticField(evobot_nav_cost_field_t *field,
	uint32_t source_area, evobot_nav_plan_world_t *world)
{
	evobot_nav_route_policy_t policy;

	memset(&policy, 0, sizeof(policy));
	policy.mode = EVOBOT_NAV_ROUTE_IGNORE_DYNAMIC_BLOCKERS;
	policy.cost = EvoBot_PlanOptimisticCost;
	policy.user_data = world;
	return EvoBot_RouteBuildField(field, source_area, &policy);
}

static uint32_t EvoBot_PlanWantedBlockerToArea(uint32_t source_area,
	uint32_t destination, evobot_nav_plan_world_t *world)
{
	evobot_nav_cost_field_t field;
	uint32_t wanted = 0;
	size_t guard = 0;

	memset(&field, 0, sizeof(field));
	if (!destination || !EvoBot_PlanBuildOptimisticField(&field, source_area, world) ||
		!EvoBot_RouteFinite(&field, destination))
		goto done;
	while (destination != source_area && guard++ < field.area_count)
	{
		uint32_t predecessor = field.predecessors[destination - 1];
		evobot_nav_reachability_t reachability;
		uint32_t candidate = 0;

		if (!predecessor ||
			!EvoBot_NavDebugReachability(predecessor - 1, &reachability))
			break;
		if ((reachability.flags & EVOBOT_NAV_REACH_DYNAMIC_BLOCKER) &&
			reachability.dynamic_interactor > 0 &&
			EvoBot_RouteBlockerState(&reachability) != EVOBOT_DYNAMIC_BLOCKER_CLEAR &&
			!EvoBot_RouteAutomaticDoor(reachability.dynamic_interactor, NULL))
			candidate = (uint32_t)reachability.dynamic_interactor;
		if (reachability.travel_type == EVOBOT_NAV_TRAVEL_PLATFORM &&
			reachability.mover_interactor > 0)
		{
			evobot_nav_debug_interactor_t mover;
			if (EvoBot_NavDebugInteractor(
				reachability.mover_interactor - 1, &mover) &&
				mover.activation == EVOBOT_ACTIVATION_EXTERNAL)
				candidate = (uint32_t)reachability.mover_interactor;
		}
		if (candidate && (candidate > world->interactor_count ||
			!world->available[candidate - 1]))
			wanted = candidate;
		destination = reachability.source_area;
	}
done:
	EvoBot_RouteFreeField(&field);
	return wanted;
}

static uint32_t EvoBot_PlanWantedBlocker(uint32_t source_area,
	const evobot_nav_plan_world_t *world)
{
	evobot_nav_route_policy_t policy;
	evobot_nav_cost_field_t field;
	uint32_t *exit_areas = NULL;
	size_t exit_count = 0;
	uint32_t destination = 0;
	uint32_t wanted = 0;
	float best_cost = FLT_MAX;
	int exit_interactor = 0;
	size_t i;
	size_t guard = 0;

	memset(&policy, 0, sizeof(policy));
	memset(&field, 0, sizeof(field));
	policy.mode = EVOBOT_NAV_ROUTE_IGNORE_DYNAMIC_BLOCKERS;
	policy.cost = EvoBot_PlanOptimisticCost;
	policy.user_data = (void *)world;
	if (!EvoBot_RouteBuildField(&field, source_area, &policy) ||
		!EvoBot_RouteCollectExitAreas(&exit_areas, &exit_count, &exit_interactor))
		goto done;
	for (i = 0; i < exit_count; i++)
		if (EvoBot_RouteFinite(&field, exit_areas[i]) &&
			field.costs[exit_areas[i] - 1] < best_cost)
		{
			destination = exit_areas[i];
			best_cost = field.costs[exit_areas[i] - 1];
		}
	while (destination && destination != source_area &&
		guard++ < field.area_count)
	{
		uint32_t predecessor = field.predecessors[destination - 1];
		evobot_nav_reachability_t reachability;
		uint32_t candidate = 0;

		if (!predecessor ||
			!EvoBot_NavDebugReachability(predecessor - 1, &reachability))
			break;
		if ((reachability.flags & EVOBOT_NAV_REACH_DYNAMIC_BLOCKER) &&
			reachability.dynamic_interactor > 0 &&
			EvoBot_RouteBlockerState(&reachability) !=
				EVOBOT_DYNAMIC_BLOCKER_CLEAR &&
			!EvoBot_RouteAutomaticDoor(reachability.dynamic_interactor, NULL))
			candidate = (uint32_t)reachability.dynamic_interactor;
		if (reachability.travel_type == EVOBOT_NAV_TRAVEL_PLATFORM &&
			reachability.mover_interactor > 0)
		{
			evobot_nav_debug_interactor_t mover;
			if (EvoBot_NavDebugInteractor(
				reachability.mover_interactor - 1, &mover) &&
				mover.activation == EVOBOT_ACTIVATION_EXTERNAL)
				candidate = (uint32_t)reachability.mover_interactor;
		}
		if (candidate && (candidate > world->interactor_count ||
			!world->available[candidate - 1]))
			wanted = candidate;
		destination = reachability.source_area;
	}

done:
	free(exit_areas);
	EvoBot_RouteFreeField(&field);
	return wanted;
}

static uint32_t EvoBot_PlanNearestReachableArea(
	const evobot_nav_debug_interactor_t *interactor,
	const evobot_nav_cost_field_t *field, float maximum_distance)
{
	evobot_nav_debug_summary_t summary;
	uint32_t best = 0;
	float best_distance = maximum_distance;
	float best_cost = FLT_MAX;
	size_t i;

	if (!EvoBot_NavDebugSummary(&summary))
		return 0;
	for (i = 0; i < summary.area_count; i++)
	{
		evobot_nav_debug_area_t area;
		float distance;

		if (!EvoBot_NavDebugArea(i, &area) || !EvoBot_RouteAreaRouting(&area) ||
			!EvoBot_RouteFinite(field, area.id))
			continue;
		distance = EvoBot_RouteBoundsDistance(&area.bounds, &interactor->bounds);
		if (distance < best_distance ||
			(fabsf(distance - best_distance) < 0.01f &&
			 field->costs[area.id - 1] < best_cost))
		{
			best = area.id;
			best_distance = distance;
			best_cost = field->costs[area.id - 1];
		}
	}
	return best;
}

static int EvoBot_PlanApplyEffects(
	const evobot_nav_debug_interactor_t *source, uint32_t activation_area,
	evobot_nav_plan_world_t *world, size_t depth)
{
	size_t i;
	int changed = 0;

	if (depth > world->interactor_count)
	{
		world->cycle = 1;
		return 0;
	}
	if (source->id && source->id <= world->interactor_count &&
		world->logic_active[source->id - 1])
	{
		world->cycle = 1;
		return 0;
	}
	if (source->id && source->id <= world->interactor_count)
		world->logic_active[source->id - 1] = 1;
	for (i = 0; i < EvoBot_NavDebugActivationCount(); i++)
	{
		evobot_nav_debug_activation_t activation;
		evobot_nav_debug_interactor_t destination;
		int ready = 1;

		if (!EvoBot_NavDebugActivation(i, &activation) ||
			activation.source_interactor != source->id ||
			!EvoBot_NavDebugInteractor(
				activation.destination_interactor - 1, &destination))
			continue;
		if (!strcmp(destination.classname, "trigger_counter"))
		{
			int required = destination.activation_required_count > 0 ?
				destination.activation_required_count : 2;
			int counted = 0;

			if (world->counter_progress[destination.id - 1] < (unsigned)required)
			{
				world->counter_progress[destination.id - 1]++;
				world->counter_states++;
				changed = 1;
				counted = 1;
			}
			if (counted && activation_area)
				EvoBot_PlanAppendGoal(source, destination.id, activation_area);
			ready = world->counter_progress[destination.id - 1] >=
				(unsigned)required;
		}
		else if (!world->available[destination.id - 1] && activation_area)
			EvoBot_PlanAppendGoal(source, destination.id, activation_area);
		if (!ready || world->available[destination.id - 1])
			continue;
		EvoBot_PlanSetAvailable(&destination, world);
		changed = 1;
		if (source->activation == EVOBOT_ACTIVATION_SHOOT)
			world->conditional = 1;
		if (destination.kind == EVOBOT_INTERACTOR_LOGIC)
			changed |= EvoBot_PlanApplyEffects(&destination, 0, world, depth + 1);
	}
	if (source->killtarget[0])
	{
		for (i = 0; i < world->interactor_count; i++)
		{
			evobot_nav_debug_interactor_t killed;

			if (EvoBot_NavDebugInteractor(i, &killed) && killed.targetname[0] &&
				!strcmp(source->killtarget, killed.targetname) &&
				!world->available[i])
			{
				EvoBot_PlanSetAvailable(&killed, world);
				changed = 1;
				if (activation_area)
					EvoBot_PlanAppendGoal(source, killed.id, activation_area);
			}
		}
	}
	if (source->id && source->id <= world->interactor_count)
		world->logic_active[source->id - 1] = 0;
	return changed;
}

static int EvoBot_PlanForwardClosure(uint32_t source_area,
	evobot_nav_plan_world_t *world, int allow_shoot,
	evobot_nav_cost_field_t *result_field)
{
	evobot_nav_cost_field_t field;
	int progress;
	(void)source_area;

	memset(&field, 0, sizeof(field));
	do
	{
		evobot_nav_debug_interactor_t selected;
		uint32_t selected_area = 0;
		uint32_t wanted;
		float selected_cost = FLT_MAX;
		int selected_affinity = 0;
		int selected_direct_shoot = 0;
		size_t i;

		progress = 0;
		if (!EvoBot_PlanBuildWorldField(&field, world->current_area, world))
			break;
		wanted = EvoBot_PlanWantedBlocker(world->current_area, world);
		if (wanted)
		{
			evobot_nav_cost_field_t optimistic;
			evobot_nav_route_policy_t policy;

			memset(&optimistic, 0, sizeof(optimistic));
			memset(&policy, 0, sizeof(policy));
			policy.mode = EVOBOT_NAV_ROUTE_IGNORE_DYNAMIC_BLOCKERS;
			policy.cost = EvoBot_PlanOptimisticCost;
			policy.user_data = world;
			if (EvoBot_RouteBuildField(&optimistic, world->current_area, &policy))
			{
				for (i = 0; i < world->interactor_count; i++)
				{
					evobot_nav_debug_interactor_t required_source;
					uint32_t current_area;
					uint32_t optimistic_area;
					uint32_t prerequisite;

					if (world->activated[i] ||
						!EvoBot_NavDebugInteractor(i, &required_source) ||
						(required_source.id != wanted &&
						 !EvoBot_PlanSourceAffects(required_source.id, wanted,
							world->interactor_count)))
						continue;
					current_area = EvoBot_PlanReachableActivationArea(
						&required_source, &field);
					if (current_area)
						continue;
					optimistic_area = EvoBot_PlanReachableActivationArea(
						&required_source, &optimistic);
					prerequisite = optimistic_area ?
						EvoBot_PlanWantedBlockerToArea(world->current_area,
							optimistic_area, world) : 0;
					if (prerequisite)
					{
						wanted = prerequisite;
						break;
					}
				}
			}
			EvoBot_RouteFreeField(&optimistic);
		}
		memset(&selected, 0, sizeof(selected));
		for (i = 0; i < world->interactor_count; i++)
		{
			evobot_nav_debug_interactor_t source;
			uint32_t area;
			float candidate_cost;
			int affinity;
			int direct_shoot = 0;

			if (world->activated[i] ||
				!EvoBot_NavDebugInteractor(i, &source))
				continue;
			if (source.kind == EVOBOT_INTERACTOR_DOOR &&
				source.activation == EVOBOT_ACTIVATION_SHOOT)
			{
				if (!allow_shoot)
					continue;
				area = EvoBot_PlanNearestReachableArea(&source, &field, 1024.0f);
				if (!area)
					continue;
				direct_shoot = 1;
			}
			else if ((source.kind != EVOBOT_INTERACTOR_BUTTON &&
				 source.kind != EVOBOT_INTERACTOR_TRIGGER) ||
				(!source.target[0] && !source.killtarget[0]) ||
				source.activation == EVOBOT_ACTIVATION_EXTERNAL ||
				source.activation == EVOBOT_ACTIVATION_UNKNOWN)
				continue;
			else if (source.activation == EVOBOT_ACTIVATION_SHOOT)
			{
				if (!allow_shoot)
					continue;
				area = EvoBot_PlanNearestReachableArea(&source, &field, 1024.0f);
			}
			else
				area = EvoBot_PlanReachableActivationArea(&source, &field);
			if (!area)
				continue;
			candidate_cost = field.costs[area - 1];
			affinity = (source.id == wanted ||
				EvoBot_PlanSourceAffects(source.id, wanted,
					world->interactor_count)) ? 2 : 1;
			if (affinity > selected_affinity ||
				(affinity == selected_affinity && candidate_cost < selected_cost))
			{
				selected = source;
				selected_area = area;
				selected_cost = candidate_cost;
				selected_affinity = affinity;
				selected_direct_shoot = direct_shoot;
			}
		}
		if (selected_area)
		{
			EvoBot_RoutePrintf("plan closure: wanted interactor %" PRIu32
				", selected %" PRIu32 " at area %" PRIu32 "\n",
				wanted, selected.id, selected_area);
			world->activated[selected.id - 1] = 1;
			world->navigation_cost += selected_cost;
			world->elapsed_time += selected_cost;
			world->current_area = selected_area;
			if (selected_direct_shoot)
			{
				EvoBot_PlanSetAvailable(&selected, world);
				world->conditional = 1;
				EvoBot_PlanAppendGoal(&selected, selected.id, selected_area);
				progress = 1;
			}
			else if (EvoBot_PlanApplyEffects(&selected, selected_area, world, 0))
				progress = 1;
		}
		else if (wanted)
			EvoBot_RoutePrintf("plan closure stalled at interactor %" PRIu32 "\n",
				wanted);
	} while (progress);
	EvoBot_RouteFreeField(&field);
	if (!EvoBot_PlanBuildWorldField(result_field, world->current_area, world))
		return 0;
	return 1;
}

static uint32_t EvoBot_PlanReachableExit(const evobot_nav_cost_field_t *field,
	int *exit_interactor, float *cost)
{
	uint32_t *areas = NULL;
	size_t count = 0;
	uint32_t best = 0;
	size_t i;

	*cost = FLT_MAX;
	if (!EvoBot_RouteCollectExitAreas(&areas, &count, exit_interactor))
		return 0;
	for (i = 0; i < count; i++)
		if (EvoBot_RouteFinite(field, areas[i]) &&
			field->costs[areas[i] - 1] < *cost)
		{
			best = areas[i];
			*cost = field->costs[areas[i] - 1];
		}
	free(areas);
	return best;
}

int EvoBot_NavPlanExit(uint32_t source_area)
{
	double started = EvoBot_RouteTime();
	evobot_nav_debug_summary_t nav_summary;
	evobot_nav_plan_world_t world;
	evobot_nav_cost_field_t world_field;
	uint32_t destination = 0;
	float plan_cost = FLT_MAX;
	int exit_interactor = 0;

	if (!EvoBot_NavRouteBuild(source_area, NULL) || !EvoBot_NavRouteSelectExit())
	{
		memset(&evobot_nav_plan, 0, sizeof(evobot_nav_plan));
		evobot_nav_plan.present = 1;
		evobot_nav_plan.source_area = source_area;
		snprintf(evobot_nav_plan.unresolved_reason,
			sizeof(evobot_nav_plan.unresolved_reason), "route query failed");
		return 0;
	}
	memset(&evobot_nav_plan, 0, sizeof(evobot_nav_plan));
	evobot_nav_plan.present = 1;
	evobot_nav_plan.source_area = source_area;
	evobot_nav_plan.exit_interactor = evobot_route_summary.exit_interactor;
	evobot_nav_plan.navigation_cost = evobot_route_summary.total_cost;
	if (evobot_route_summary.result == EVOBOT_NAV_ROUTE_REACHABLE)
	{
		evobot_nav_plan.solvable = 1;
		evobot_nav_plan.status = EVOBOT_NAV_PLAN_STATIC_EXIT;
		evobot_nav_plan.calculation_time = EvoBot_RouteTime() - started;
		return 1;
	}
	memset(&world, 0, sizeof(world));
	memset(&world_field, 0, sizeof(world_field));
	if (!EvoBot_NavDebugSummary(&nav_summary) || !nav_summary.interactor_count)
	{
		evobot_nav_plan.status = EVOBOT_NAV_PLAN_UNREACHABLE;
		snprintf(evobot_nav_plan.unresolved_reason,
			sizeof(evobot_nav_plan.unresolved_reason),
			"navigation graph has no interactor metadata");
		evobot_nav_plan.calculation_time = EvoBot_RouteTime() - started;
		return 1;
	}
	world.interactor_count = nav_summary.interactor_count;
	world.current_area = source_area;
	world.available = calloc(world.interactor_count, 1);
	world.activated = calloc(world.interactor_count, 1);
	world.logic_active = calloc(world.interactor_count, 1);
	world.has_activator = calloc(world.interactor_count, 1);
	world.counter_progress = calloc(world.interactor_count,
		sizeof(*world.counter_progress));
	world.available_from = calloc(world.interactor_count,
		sizeof(*world.available_from));
	world.available_until = malloc(world.interactor_count *
		sizeof(*world.available_until));
	if (!world.available || !world.activated || !world.logic_active ||
		!world.has_activator ||
		!world.counter_progress || !world.available_from ||
		!world.available_until)
	{
		snprintf(evobot_nav_plan.unresolved_reason,
			sizeof(evobot_nav_plan.unresolved_reason), "out of memory");
		evobot_nav_plan.status = EVOBOT_NAV_PLAN_BLOCKED;
		goto done;
	}
	{
		size_t i;
		for (i = 0; i < world.interactor_count; i++)
		{
			world.available_until[i] = FLT_MAX;
			world.has_activator[i] = (unsigned char)EvoBot_PlanHasActivator(
				(uint32_t)i + 1, world.interactor_count);
		}
	}
	if (EvoBot_PlanForwardClosure(source_area, &world, 0, &world_field))
		destination = EvoBot_PlanReachableExit(&world_field, &exit_interactor,
			&plan_cost);
	if (!destination)
	{
		EvoBot_RouteFreeField(&world_field);
		if (EvoBot_PlanForwardClosure(source_area, &world, 1, &world_field))
			destination = EvoBot_PlanReachableExit(&world_field,
				&exit_interactor, &plan_cost);
	}
	if (destination)
	{
		evobot_nav_plan.navigation_cost = world.navigation_cost + plan_cost;
		evobot_nav_plan.exit_interactor = exit_interactor;
		evobot_nav_plan.status = world.conditional ?
			EVOBOT_NAV_PLAN_CONDITIONAL : EVOBOT_NAV_PLAN_COMPLETE;
		evobot_nav_plan.solvable =
			evobot_nav_plan.status == EVOBOT_NAV_PLAN_COMPLETE;
		if (world.conditional)
			snprintf(evobot_nav_plan.unresolved_reason,
				sizeof(evobot_nav_plan.unresolved_reason),
				"requires shoot activation or unproven timed connectivity");
	}
	else if (evobot_route_summary.ignore_dynamic_reaches_destination)
	{
		evobot_nav_plan.status = EVOBOT_NAV_PLAN_BLOCKED;
		snprintf(evobot_nav_plan.unresolved_reason,
			sizeof(evobot_nav_plan.unresolved_reason), "%s",
			world.cycle ? "cyclic activation dependency" :
			"no proven reachable activation sequence");
	}
	else
	{
		evobot_nav_plan.status = EVOBOT_NAV_PLAN_UNREACHABLE;
		snprintf(evobot_nav_plan.unresolved_reason,
			sizeof(evobot_nav_plan.unresolved_reason), "%s",
			evobot_route_summary.frontier_reason[0] ?
			evobot_route_summary.frontier_reason :
			"physical route remains unreachable");
	}
	if (world.cycle && evobot_nav_plan.status == EVOBOT_NAV_PLAN_COMPLETE)
	{
		evobot_nav_plan.status = EVOBOT_NAV_PLAN_BLOCKED;
		evobot_nav_plan.solvable = 0;
		snprintf(evobot_nav_plan.unresolved_reason,
			sizeof(evobot_nav_plan.unresolved_reason),
			"cyclic activation dependency");
	}
	evobot_nav_plan.searched_world_states = world.searched_states;
	evobot_nav_plan.timed_world_states = world.timed_states;
	evobot_nav_plan.counter_world_states = world.counter_states;
	evobot_nav_plan.deepest_dependency_chain = evobot_nav_plan.subgoal_count;

done:
	evobot_nav_plan.calculation_time = EvoBot_RouteTime() - started;
	EvoBot_RouteFreeField(&world_field);
	free(world.available);
	free(world.activated);
	free(world.logic_active);
	free(world.has_activator);
	free(world.counter_progress);
	free(world.available_from);
	free(world.available_until);
	return 1;
}

static const char *EvoBot_PlanStatusName(evobot_nav_plan_status_t status)
{
	switch (status)
	{
	case EVOBOT_NAV_PLAN_STATIC_EXIT: return "static exit";
	case EVOBOT_NAV_PLAN_COMPLETE: return "complete";
	case EVOBOT_NAV_PLAN_CONDITIONAL: return "conditional";
	case EVOBOT_NAV_PLAN_BLOCKED: return "blocked";
	case EVOBOT_NAV_PLAN_UNREACHABLE: return "unreachable";
	case EVOBOT_NAV_PLAN_NONE:
	default: return "none";
	}
}

void EvoBot_NavPlanPrintStatus(void)
{
	size_t i;

	if (!evobot_nav_plan.present)
	{
		EvoBot_RoutePrint("EvoBot plan status\nresult: none\n");
		return;
	}
	EvoBot_RoutePrintf("EvoBot plan status\nsource area: %" PRIu32
		"\nexit interactor: %d\nresult: %s\nsubgoals: %zu\n",
		evobot_nav_plan.source_area, evobot_nav_plan.exit_interactor,
		EvoBot_PlanStatusName(evobot_nav_plan.status),
		evobot_nav_plan.subgoal_count);
	for (i = 0; i < evobot_nav_plan.subgoal_count; i++)
	{
		const evobot_nav_plan_subgoal_t *goal = &evobot_nav_plan.subgoals[i];

		evobot_nav_debug_interactor_t activator;
		evobot_nav_debug_interactor_t affected;
		const char *activation = "unknown";
		const char *activator_class = "unknown";
		const char *affected_class = "unknown";

		if (EvoBot_NavDebugInteractor(goal->activator - 1, &activator))
		{
			activation = EvoBot_RouteActivationName(activator.activation);
			activator_class = activator.classname;
		}
		if (EvoBot_NavDebugInteractor(goal->affected - 1, &affected))
			affected_class = affected.classname;
		EvoBot_RoutePrintf("%zu. navigate to area %" PRIu32
			"; activate interactor %" PRIu32 " %s (%s); state change "
			"interactor %" PRIu32 " %s\n", i + 1, goal->area,
			goal->activator, activator_class, activation, goal->affected,
			affected_class);
	}
	if (evobot_nav_plan.status == EVOBOT_NAV_PLAN_CONDITIONAL)
		EvoBot_RoutePrintf("conditional requirement: %s\n",
			evobot_nav_plan.unresolved_reason);
	else if (evobot_nav_plan.status == EVOBOT_NAV_PLAN_BLOCKED ||
		evobot_nav_plan.status == EVOBOT_NAV_PLAN_UNREACHABLE)
		EvoBot_RoutePrintf("strongest unresolved dependency: interactor %d: %s\n",
			evobot_nav_plan.unresolved_interactor,
			evobot_nav_plan.unresolved_reason);
	EvoBot_RoutePrintf("estimated navigation cost: %.3f s\n"
		"estimated dynamic wait: %.3f s\nworld states searched: %zu\n"
		"timed states evaluated: %zu\ncounter states evaluated: %zu\n"
		"deepest dependency chain: %zu\nplan calculation time: %.6f s\n",
		evobot_nav_plan.navigation_cost, evobot_nav_plan.dynamic_wait,
		evobot_nav_plan.searched_world_states,
		evobot_nav_plan.timed_world_states,
		evobot_nav_plan.counter_world_states,
		evobot_nav_plan.deepest_dependency_chain,
		evobot_nav_plan.calculation_time);
}

static void EvoBot_PlanDumpDependency(uint32_t affected, int depth,
	unsigned char *active, size_t interactor_count)
{
	evobot_nav_debug_interactor_t destination;
	size_t i;

	if (!affected || affected > interactor_count || depth > 12)
		return;
	if (active[affected - 1])
	{
		EvoBot_RoutePrintf("dependency depth %d: interactor %" PRIu32
			" cycle\n", depth, affected);
		return;
	}
	if (!EvoBot_NavDebugInteractor(affected - 1, &destination))
		return;
	active[affected - 1] = 1;
	EvoBot_RoutePrintf("dependency depth %d: interactor %" PRIu32
		" %s/%s activation %s targetname '%s'\n", depth, affected,
		EvoBot_RouteInteractorName(destination.kind), destination.classname,
		EvoBot_RouteActivationName(destination.activation),
		destination.targetname);
	for (i = 0; i < EvoBot_NavDebugActivationCount(); i++)
	{
		evobot_nav_debug_activation_t activation;
		evobot_nav_debug_interactor_t source;
		uint32_t primary_area;
		uint32_t alternate_area;

		if (!EvoBot_NavDebugActivation(i, &activation) ||
			activation.destination_interactor != affected ||
			!EvoBot_NavDebugInteractor(activation.source_interactor - 1,
				&source))
			continue;
		primary_area = EvoBot_PlanReachableActivationArea(&source,
			&evobot_route_primary);
		alternate_area = EvoBot_PlanReachableActivationArea(&source,
			&evobot_route_alternate);
		EvoBot_RoutePrintf("dependency edge: %" PRIu32 " %s/%s --%s--> %"
			PRIu32 " target '%s' reachable current %" PRIu32
			" hypothetical %" PRIu32 "\n", source.id,
			EvoBot_RouteInteractorName(source.kind), source.classname,
			EvoBot_RouteActivationName(source.activation), affected,
			source.target, primary_area, alternate_area);
		if (source.kind == EVOBOT_INTERACTOR_LOGIC ||
			source.activation == EVOBOT_ACTIVATION_EXTERNAL)
			EvoBot_PlanDumpDependency(source.id, depth + 1, active,
				interactor_count);
	}
	active[affected - 1] = 0;
}

void EvoBot_NavPlanPrintDump(void)
{
	evobot_nav_debug_summary_t nav_summary;
	unsigned char *active;
	unsigned char *listed;
	size_t blocker_count = 0;
	size_t i;

	if (!evobot_nav_plan.present || !EvoBot_NavDebugSummary(&nav_summary))
	{
		EvoBot_RoutePrint("EvoBot plan dump\nresult: none\n");
		return;
	}
	active = calloc(nav_summary.interactor_count, 1);
	listed = calloc(nav_summary.interactor_count, 1);
	if ((!active || !listed) && nav_summary.interactor_count)
	{
		free(active);
		free(listed);
		EvoBot_RoutePrint("EvoBot plan dump: out of memory\n");
		return;
	}
	EvoBot_RoutePrintf("EvoBot plan dump\nstatic route: %s\n",
		EvoBot_RouteResultName(evobot_route_summary.result));
	for (i = 0; i < evobot_route_summary.route_length; i++)
	{
		const evobot_nav_route_step_t *step = &evobot_route_steps[i];
		evobot_nav_reachability_t reachability;
		int interactor = step->dynamic_interactor;

		if (!EvoBot_NavDebugReachability(step->reachability_id - 1,
			&reachability))
			continue;
		if (interactor <= 0 &&
			reachability.travel_type == EVOBOT_NAV_TRAVEL_PLATFORM)
			interactor = reachability.mover_interactor;
		if (interactor <= 0 || interactor > (int)nav_summary.interactor_count ||
			listed[interactor - 1])
			continue;
		if (step->blocker_state == EVOBOT_DYNAMIC_BLOCKER_CLEAR &&
			reachability.travel_type != EVOBOT_NAV_TRAVEL_PLATFORM)
			continue;
		listed[interactor - 1] = 1;
		blocker_count++;
		EvoBot_RoutePrintf("blocker %zu: route step %zu reachability %" PRIu32
			" areas %" PRIu32 "->%" PRIu32 " interactor %d\n",
			blocker_count, i + 1, step->reachability_id, step->source_area,
			step->destination_area, interactor);
		EvoBot_PlanDumpDependency((uint32_t)interactor, 0, active,
			nav_summary.interactor_count);
	}
	if (evobot_route_summary.result == EVOBOT_NAV_ROUTE_UNREACHABLE)
		EvoBot_RoutePrintf("physical frontier: area %" PRIu32
			" portal %" PRIu32 " beyond area %" PRIu32 " interactor %d: %s\n",
			evobot_route_summary.frontier_area,
			evobot_route_summary.frontier_portal,
			evobot_route_summary.frontier_gap_area,
			evobot_route_summary.frontier_interactor,
			evobot_route_summary.frontier_reason);
	for (i = 0; i < nav_summary.interactor_count; i++)
	{
		evobot_nav_debug_interactor_t logic;
		size_t edge;

		if (!EvoBot_NavDebugInteractor(i, &logic) ||
			logic.kind != EVOBOT_INTERACTOR_LOGIC)
			continue;
		EvoBot_RoutePrintf("logic chain: interactor %" PRIu32 " %s target '%s' "
			"targetname '%s'\n", logic.id, logic.classname, logic.target,
			logic.targetname);
		EvoBot_PlanDumpDependency(logic.id, 0, active,
			nav_summary.interactor_count);
		for (edge = 0; edge < EvoBot_NavDebugActivationCount(); edge++)
		{
			evobot_nav_debug_activation_t activation;
			evobot_nav_debug_interactor_t destination;

			if (!EvoBot_NavDebugActivation(edge, &activation) ||
				activation.source_interactor != logic.id ||
				!EvoBot_NavDebugInteractor(
					activation.destination_interactor - 1, &destination))
				continue;
			EvoBot_RoutePrintf("logic continuation: %" PRIu32 " --external--> %"
				PRIu32 " %s/%s\n", logic.id, destination.id,
				EvoBot_RouteInteractorName(destination.kind),
				destination.classname);
		}
	}
	for (i = 0; i < nav_summary.interactor_count; i++)
	{
		evobot_nav_debug_interactor_t push;
		if (EvoBot_NavDebugInteractor(i, &push) &&
			!strcmp(push.classname, "trigger_push"))
			EvoBot_RoutePrintf("push trigger: interactor %" PRIu32
				" velocity [%.1f %.1f %.1f] bounds [%.1f %.1f %.1f] "
				"[%.1f %.1f %.1f]\n", push.id, push.velocity.v[0],
				push.velocity.v[1], push.velocity.v[2], push.bounds.mins.v[0],
				push.bounds.mins.v[1], push.bounds.mins.v[2],
				push.bounds.maxs.v[0], push.bounds.maxs.v[1],
				push.bounds.maxs.v[2]);
	}
	EvoBot_RoutePrintf("deepest proven point: %s; blocker chain entries: %zu\n",
		evobot_route_summary.result == EVOBOT_NAV_ROUTE_UNREACHABLE ?
		"physical frontier" : "hypothetical exit path", blocker_count);
	free(active);
	free(listed);
}

void EvoBot_NavPlanClear(void)
{
	memset(&evobot_nav_plan, 0, sizeof(evobot_nav_plan));
}

int EvoBot_NavPlanDebugSummary(evobot_nav_plan_summary_t *summary)
{
	if (!summary || !evobot_nav_plan.present)
		return 0;
	memset(summary, 0, sizeof(*summary));
	summary->present = evobot_nav_plan.present;
	summary->solvable = evobot_nav_plan.solvable;
	summary->source_area = evobot_nav_plan.source_area;
	summary->exit_interactor = evobot_nav_plan.exit_interactor;
	summary->subgoal_count = evobot_nav_plan.subgoal_count;
	summary->unresolved_interactor = evobot_nav_plan.unresolved_interactor;
	summary->navigation_cost = evobot_nav_plan.navigation_cost;
	summary->dynamic_wait = evobot_nav_plan.dynamic_wait;
	summary->calculation_time = evobot_nav_plan.calculation_time;
	summary->status = evobot_nav_plan.status;
	summary->searched_world_states = evobot_nav_plan.searched_world_states;
	summary->deepest_dependency_chain =
		evobot_nav_plan.deepest_dependency_chain;
	return 1;
}

int EvoBot_NavPlanDebugGoal(size_t index, evobot_nav_plan_goal_t *goal)
{
	if (!goal || index >= evobot_nav_plan.subgoal_count)
		return 0;
	goal->activation_area = evobot_nav_plan.subgoals[index].area;
	goal->activator_interactor = evobot_nav_plan.subgoals[index].activator;
	goal->affected_interactor = evobot_nav_plan.subgoals[index].affected;
	return 1;
}

int EvoBot_NavRouteDebugSummary(evobot_nav_route_summary_t *summary)
{
	if (!summary)
		return 0;
	EvoBot_RouteSync();
	*summary = evobot_route_summary;
	return evobot_route_primary.costs != NULL;
}

int EvoBot_NavRouteDebugStep(size_t index,
	evobot_nav_route_step_t *step,
	evobot_nav_reachability_t *reachability)
{
	if (!step || !reachability || !EvoBot_RouteSync() ||
		index >= evobot_route_summary.route_length)
		return 0;
	*step = evobot_route_steps[index];
	return EvoBot_NavDebugReachability(step->reachability_id - 1,
		reachability);
}

int EvoBot_NavRouteDebugFrontier(evobot_nav_route_step_t *step,
	evobot_nav_reachability_t *reachability)
{
	if (!step || !reachability || !EvoBot_RouteSync() ||
		!evobot_route_summary.frontier_reachability ||
		!EvoBot_NavDebugReachability(
			evobot_route_summary.frontier_reachability - 1, reachability))
		return 0;
	memset(step, 0, sizeof(*step));
	step->reachability_id = reachability->id;
	step->source_area = reachability->source_area;
	step->destination_area = reachability->destination_area;
	step->travel_type = reachability->travel_type;
	step->dynamic_interactor = reachability->dynamic_interactor;
	step->blocker_state = EvoBot_RouteBlockerState(reachability);
	step->segment_kind = EVOBOT_NAV_ROUTE_SEGMENT_FRONTIER;
	return 1;
}

const char *EvoBot_NavRouteDebugProblemCauseName(
	evobot_nav_route_problem_cause_t cause)
{
	switch (cause)
	{
	case EVOBOT_NAV_ROUTE_PROBLEM_NO_DIRECTION: return "no-direction";
	case EVOBOT_NAV_ROUTE_PROBLEM_NO_INTERVAL: return "no-portal-interval";
	case EVOBOT_NAV_ROUTE_PROBLEM_SOURCE_SUPPORT: return "source-support";
	case EVOBOT_NAV_ROUTE_PROBLEM_DESTINATION_SUPPORT: return "destination-support";
	case EVOBOT_NAV_ROUTE_PROBLEM_SOURCE_OWNERSHIP: return "source-ownership";
	case EVOBOT_NAV_ROUTE_PROBLEM_DESTINATION_OWNERSHIP: return "destination-ownership";
	case EVOBOT_NAV_ROUTE_PROBLEM_HEIGHT: return "height";
	case EVOBOT_NAV_ROUTE_PROBLEM_PM_NO_ENTRY: return "pm-no-entry";
	case EVOBOT_NAV_ROUTE_PROBLEM_PM_UNSUPPORTED: return "pm-unsupported";
	case EVOBOT_NAV_ROUTE_PROBLEM_PM_LIQUID: return "pm-liquid";
	case EVOBOT_NAV_ROUTE_PROBLEM_MISSING_LINK: return "missing-link";
	default: return "unknown";
	}
}

static evobot_nav_route_problem_cause_t EvoBot_RouteProblemCause(
	const evobot_nav_debug_walk_candidate_t *walk)
{
	if (!walk->direction_valid)
		return EVOBOT_NAV_ROUTE_PROBLEM_NO_DIRECTION;
	if (!walk->portal_interval_count)
		return EVOBOT_NAV_ROUTE_PROBLEM_NO_INTERVAL;
	if (!walk->source_support_samples || !walk->source_valid)
		return EVOBOT_NAV_ROUTE_PROBLEM_SOURCE_SUPPORT;
	if (!walk->destination_support_samples || !walk->destination_valid)
		return EVOBOT_NAV_ROUTE_PROBLEM_DESTINATION_SUPPORT;
	if (!walk->height_valid)
		return EVOBOT_NAV_ROUTE_PROBLEM_HEIGHT;
	if (!walk->source_in_area)
		return EVOBOT_NAV_ROUTE_PROBLEM_SOURCE_OWNERSHIP;
	if (walk->projected_interval_direction && !walk->destination_in_area)
		return EVOBOT_NAV_ROUTE_PROBLEM_DESTINATION_OWNERSHIP;
	if (walk->final_walk)
		return EVOBOT_NAV_ROUTE_PROBLEM_MISSING_LINK;
	if (!walk->movement_succeeded || !walk->reached_destination)
		return EVOBOT_NAV_ROUTE_PROBLEM_PM_NO_ENTRY;
	if (walk->result.state.water_level >= 2)
		return EVOBOT_NAV_ROUTE_PROBLEM_PM_LIQUID;
	if (!walk->result.state.on_ground && !walk->supported_arrival)
		return EVOBOT_NAV_ROUTE_PROBLEM_PM_UNSUPPORTED;
	return EVOBOT_NAV_ROUTE_PROBLEM_UNKNOWN;
}

static int EvoBot_RouteProblemAt(size_t wanted,
	evobot_nav_route_problem_t *problem, size_t *count)
{
	const evobot_nav_cost_field_t *field;
	evobot_nav_debug_summary_t summary;
	size_t found = 0;
	size_t i;

	if (!EvoBot_RouteSync() || !EvoBot_NavDebugSummary(&summary))
		return 0;
	field = evobot_route_alternate.costs ? &evobot_route_alternate :
		&evobot_route_primary;
	for (i = 0; i < summary.portal_count; i++)
	{
		evobot_nav_debug_portal_t portal;
		evobot_nav_debug_area_t destination_area;
		uint32_t source;
		uint32_t destination;
		int reachable_a;
		int reachable_b;

		if (!EvoBot_NavDebugPortal(i, &portal) ||
			portal.kind != EVOBOT_NAV_DEBUG_FACE_PORTAL || !portal.area_a ||
			!portal.area_b || portal.area_a > field->area_count ||
			portal.area_b > field->area_count)
			continue;
		reachable_a = EvoBot_RouteFinite(field, portal.area_a);
		reachable_b = EvoBot_RouteFinite(field, portal.area_b);
		if (reachable_a == reachable_b)
			continue;
		source = reachable_a ? portal.area_a : portal.area_b;
		destination = reachable_a ? portal.area_b : portal.area_a;
		if (!EvoBot_NavDebugArea(destination - 1, &destination_area) ||
			!EvoBot_RouteAreaRouting(&destination_area))
			continue;
		if (problem && found == wanted)
		{
			memset(problem, 0, sizeof(*problem));
			problem->index = found;
			problem->portal_id = portal.id;
			problem->source_area = source;
			problem->destination_area = destination;
			EvoBot_NavDebugWalkCandidate(portal.id, source, destination,
				&problem->forward);
			EvoBot_NavDebugWalkCandidate(portal.id, destination, source,
				&problem->reverse);
			problem->cause = EvoBot_RouteProblemCause(&problem->forward);
			return 1;
		}
		found++;
	}
	if (count)
		*count = found;
	return problem == NULL;
}

size_t EvoBot_NavRouteDebugProblemCount(void)
{
	size_t count = 0;

	EvoBot_RouteProblemAt(0, NULL, &count);
	return count;
}

int EvoBot_NavRouteDebugProblem(size_t index,
	evobot_nav_route_problem_t *problem)
{
	return problem && EvoBot_RouteProblemAt(index, problem, NULL);
}

static void EvoBot_RoutePrintProblemArea(const char *label,
	const evobot_nav_debug_area_t *area)
{
	EvoBot_RoutePrintf("%s area: %" PRIu32 " bounds [%.2f %.2f %.2f] "
		"[%.2f %.2f %.2f] center [%.2f %.2f %.2f]\n", label, area->id,
		area->bounds.mins.v[0], area->bounds.mins.v[1], area->bounds.mins.v[2],
		area->bounds.maxs.v[0], area->bounds.maxs.v[1], area->bounds.maxs.v[2],
		area->center.v[0], area->center.v[1], area->center.v[2]);
	EvoBot_RoutePrintf("%s support: contents %d supported %d floor %.3f "
		"distance %.3f normal [%.3f %.3f %.3f] water %d dynamic %d "
		"faces %zu portals %zu\n", label, (int)area->contents, area->supported,
		area->floor_height, area->support_distance, area->support_normal.v[0],
		area->support_normal.v[1], area->support_normal.v[2], area->water_level,
		area->dynamic_interactor, area->face_count, area->portal_count);
}

static void EvoBot_RoutePrintProblemCandidate(const char *label,
	const evobot_nav_debug_walk_candidate_t *walk)
{
	EvoBot_RoutePrintf("%s candidate: direction %d projected %d "
		"stacked %d [%.3f %.3f %.3f], intervals %u samples %u, support %u/%u, "
		"inside %d/%d, overlap %u, height %.3f/%.3f\n", label,
		walk->direction_valid, walk->projected_interval_direction,
		walk->stacked_decomposition,
		walk->direction.v[0], walk->direction.v[1], walk->direction.v[2],
		walk->portal_interval_count, walk->sample_count,
		walk->source_support_samples, walk->destination_support_samples,
		walk->source_in_area, walk->destination_in_area, walk->overlap_samples,
		walk->height_delta, walk->step_limit);
	EvoBot_RoutePrintf("%s PM: moved %d steps %u reached %d segment %d hull %d "
		"grounded %d support-arrival %d water %d water-jump %d final %d "
		"start [%.2f %.2f %.2f] wanted [%.2f %.2f %.2f] result "
		"[%.2f %.2f %.2f]\n", label, walk->movement_succeeded,
		walk->move_steps, walk->reached_destination,
		walk->segment_reached_destination, walk->hull_crossed_portal,
		walk->result.state.on_ground,
		walk->supported_arrival, walk->result.state.water_level,
		walk->water_jump, walk->final_walk, walk->start_origin.v[0],
		walk->start_origin.v[1], walk->start_origin.v[2],
		walk->desired_destination.v[0], walk->desired_destination.v[1],
		walk->desired_destination.v[2], walk->result.state.origin.v[0],
		walk->result.state.origin.v[1], walk->result.state.origin.v[2]);
}

void EvoBot_NavRoutePrintProblem(size_t index)
{
	evobot_nav_route_problem_t problem;
	evobot_nav_debug_portal_t portal;
	evobot_nav_debug_area_t source;
	evobot_nav_debug_area_t destination;
	size_t count = EvoBot_NavRouteDebugProblemCount();
	size_t i;

	if (!count || !EvoBot_NavRouteDebugProblem(index, &problem) ||
		!EvoBot_NavDebugPortal(problem.portal_id - 1, &portal) ||
		!EvoBot_NavDebugArea(problem.source_area - 1, &source) ||
		!EvoBot_NavDebugArea(problem.destination_area - 1, &destination))
	{
		EvoBot_RoutePrintf("EvoBot route problem: index %zu unavailable "
			"(%zu shared boundary problems)\n", index, count);
		return;
	}
	EvoBot_RoutePrintf("EvoBot route problem %zu / %zu: portal %" PRIu32
		" source %" PRIu32 " destination %" PRIu32 " cause %s\n",
		index + 1, count, problem.portal_id, problem.source_area,
		problem.destination_area,
		EvoBot_NavRouteDebugProblemCauseName(problem.cause));
	EvoBot_RoutePrintf("portal: kind %d plane [%.6f %.6f %.6f] %.6f "
		"vertices %zu\n", (int)portal.kind, portal.normal.v[0],
		portal.normal.v[1], portal.normal.v[2], portal.distance,
		portal.vertex_count);
	for (i = 0; i < portal.vertex_count; i++)
	{
		evobot_vec3_t vertex;

		if (EvoBot_NavDebugPortalVertex(problem.portal_id - 1, i, &vertex))
			EvoBot_RoutePrintf("portal vertex %zu: [%.3f %.3f %.3f]\n", i,
				vertex.v[0], vertex.v[1], vertex.v[2]);
	}
	EvoBot_RoutePrintProblemArea("source", &source);
	EvoBot_RoutePrintProblemArea("destination", &destination);
	EvoBot_RoutePrintf("support relation: floor delta %.3f normal dot %.6f "
		"same-height %d\n", destination.floor_height - source.floor_height,
		source.support_normal.v[0] * destination.support_normal.v[0] +
		source.support_normal.v[1] * destination.support_normal.v[1] +
		source.support_normal.v[2] * destination.support_normal.v[2],
		fabsf(destination.floor_height - source.floor_height) <= 0.5f);
	EvoBot_RoutePrintProblemCandidate("forward", &problem.forward);
	EvoBot_RoutePrintProblemCandidate("reverse", &problem.reverse);
}

static void EvoBot_RoutePrintAirCandidate(const char *label,
	const evobot_nav_debug_air_candidate_t *candidate)
{
	EvoBot_RoutePrintf("%s: present %d valid %d rejection %s attempts %u "
		"edge %" PRIu32 " width %.2f direction [%.3f %.3f %.3f]\n",
		label, candidate->present, candidate->valid,
		EvoBot_NavDebugAirRejectionName(candidate->rejection),
		candidate->attempts, candidate->edge_index, candidate->portal_width,
		candidate->direction.v[0], candidate->direction.v[1],
		candidate->direction.v[2]);
	EvoBot_RoutePrintf("%s geometry: source floor %.3f destination floor %.3f "
		"delta %.3f headroom %.2f nominal gap 0.00 overlap %.2f\n",
		label, candidate->source_floor, candidate->destination_floor,
		candidate->height_delta, candidate->headroom, candidate->portal_width);
	EvoBot_RoutePrintf("%s PM: speed %.1f approach %u steps %u blocked %u "
		"left %d entered %d airborne %d landed %d hull-clear %d area %" PRIu32
		" time %.3f rise %.2f horizontal %.2f liquid %d\n",
		label, candidate->command_speed, candidate->approach_frames,
		candidate->move_steps, candidate->blocked_steps, candidate->left_source,
		candidate->entered_destination, candidate->airborne, candidate->landed,
		candidate->landing_hull_clear,
		candidate->landing_area, candidate->elapsed, candidate->maximum_rise,
		candidate->horizontal_displacement, candidate->liquid_landing);
	EvoBot_RoutePrintf("%s points: start [%.2f %.2f %.2f] launch "
		"[%.2f %.2f %.2f] landing [%.2f %.2f %.2f] final [%.2f %.2f %.2f]\n",
		label, candidate->start_origin.v[0], candidate->start_origin.v[1],
		candidate->start_origin.v[2], candidate->launch_origin.v[0],
		candidate->launch_origin.v[1], candidate->launch_origin.v[2],
		candidate->landing_origin.v[0], candidate->landing_origin.v[1],
		candidate->landing_origin.v[2], candidate->final_origin.v[0],
		candidate->final_origin.v[1], candidate->final_origin.v[2]);
}

void EvoBot_NavRoutePrintFrontierAudit(void)
{
	evobot_nav_debug_portal_t portal;
	evobot_nav_debug_area_t source;
	evobot_nav_debug_area_t destination;
	evobot_nav_debug_air_candidate_t drop;
	evobot_nav_debug_air_candidate_t jump;
	size_t i;

	if (!evobot_route_summary.frontier_portal ||
		!EvoBot_NavDebugPortal(evobot_route_summary.frontier_portal - 1, &portal) ||
		!EvoBot_NavDebugArea(evobot_route_summary.frontier_area - 1, &source) ||
		!EvoBot_NavDebugArea(evobot_route_summary.frontier_gap_area - 1,
			&destination))
	{
		EvoBot_RoutePrintf("EvoBot frontier audit: current route has no portal frontier\n");
		return;
	}
	EvoBot_RoutePrintf("EvoBot frontier audit\nportal: %" PRIu32
		" kind %d source %" PRIu32 " destination %" PRIu32
		" plane [%.6f %.6f %.6f] %.6f vertices %zu\n",
		portal.id, (int)portal.kind, source.id, destination.id,
		portal.normal.v[0], portal.normal.v[1], portal.normal.v[2],
		portal.distance, portal.vertex_count);
	for (i = 0; i < portal.vertex_count; i++)
	{
		evobot_vec3_t vertex;

		if (EvoBot_NavDebugPortalVertex(portal.id - 1, i, &vertex))
			EvoBot_RoutePrintf("portal vertex %zu: [%.3f %.3f %.3f]\n",
				i, vertex.v[0], vertex.v[1], vertex.v[2]);
	}
	EvoBot_RoutePrintProblemArea("source", &source);
	EvoBot_RoutePrintProblemArea("destination", &destination);
	EvoBot_RoutePrintf("classification geometry: vertical delta %.3f step limit 18.000 "
		"step possible %d shared-portal horizontal gap 0.000 dynamic %d/%d\n",
		destination.floor_height - source.floor_height,
		fabsf(destination.floor_height - source.floor_height) <= 18.0f,
		source.dynamic_interactor, destination.dynamic_interactor);
	memset(&drop, 0, sizeof(drop));
	if (EvoBot_NavDebugAirCandidate(portal.id, source.id, destination.id,
		EVOBOT_NAV_DEBUG_AIR_DROP, &drop))
		EvoBot_RoutePrintAirCandidate("DROP forward", &drop);
	memset(&jump, 0, sizeof(jump));
	if (EvoBot_NavDebugAirCandidate(portal.id, source.id, destination.id,
		EVOBOT_NAV_DEBUG_AIR_JUMP_UP, &jump))
		EvoBot_RoutePrintAirCandidate("JUMP_UP forward", &jump);
	memset(&jump, 0, sizeof(jump));
	if (EvoBot_NavDebugAirCandidate(portal.id, source.id, destination.id,
		EVOBOT_NAV_DEBUG_AIR_WATER_JUMP, &jump))
		EvoBot_RoutePrintAirCandidate("WATER_JUMP forward", &jump);
	memset(&drop, 0, sizeof(drop));
	if (EvoBot_NavDebugAirCandidate(portal.id, destination.id, source.id,
		EVOBOT_NAV_DEBUG_AIR_DROP, &drop))
		EvoBot_RoutePrintAirCandidate("DROP reverse", &drop);
	memset(&jump, 0, sizeof(jump));
	if (EvoBot_NavDebugAirCandidate(portal.id, destination.id, source.id,
		EVOBOT_NAV_DEBUG_AIR_JUMP_UP, &jump))
		EvoBot_RoutePrintAirCandidate("JUMP_UP reverse", &jump);
	memset(&jump, 0, sizeof(jump));
	if (EvoBot_NavDebugAirCandidate(portal.id, destination.id, source.id,
		EVOBOT_NAV_DEBUG_AIR_WATER_JUMP, &jump))
		EvoBot_RoutePrintAirCandidate("WATER_JUMP reverse", &jump);
	if (evobot_route_summary.frontier_interactor)
		EvoBot_RoutePrintf("nearest interactor: %d kind %d distance %.2f\n",
			evobot_route_summary.frontier_interactor,
			(int)evobot_route_summary.frontier_interactor_kind,
			evobot_route_summary.frontier_interactor_distance);
}
