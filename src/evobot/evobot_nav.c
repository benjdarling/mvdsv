#include "evobot_nav.h"

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVOBOT_NAV_FORMAT_VERSION 1
#define EVOBOT_NAV_COARSE_SIZE 64.0f
#define EVOBOT_NAV_FINE_SIZE 16.0f
#define EVOBOT_NAV_MIN_SIZE 8.0f
#define EVOBOT_NAV_SUPPORT_PROBE 256.0f
#define EVOBOT_NAV_WALKABLE_NORMAL_Z 0.7f
#define EVOBOT_NAV_STEP_SIZE 18.0f
#define EVOBOT_NAV_EPSILON 0.25f
#define EVOBOT_NAV_FLOAT_EPSILON 0.01f
#define EVOBOT_NAV_INVALID_ID (-1)
#define EVOBOT_NAV_MULTIPLE_DYNAMIC (-2)
#define EVOBOT_NAV_MAX_FILE_SIZE (64u * 1024u * 1024u)

typedef enum evobot_nav_state_e
{
	EVOBOT_NAV_STATE_EMPTY,
	EVOBOT_NAV_STATE_GENERATED,
	EVOBOT_NAV_STATE_LOADED
} evobot_nav_state_t;

typedef enum evobot_nav_boundary_kind_e
{
	EVOBOT_NAV_BOUNDARY_ADJACENT,
	EVOBOT_NAV_BOUNDARY_LEDGE,
	EVOBOT_NAV_BOUNDARY_LIQUID
} evobot_nav_boundary_kind_t;

typedef struct evobot_nav_volume_s
{
	uint32_t id;
	evobot_bounds_t bounds;
	evobot_contents_t contents;
	int supported;
	float floor_height;
	float support_distance;
	evobot_vec3_t support_normal;
	int water_level;
	int dynamic_interactor;
} evobot_nav_volume_t;

typedef struct evobot_nav_boundary_s
{
	uint32_t id;
	int volume_a;
	int volume_b;
	evobot_nav_boundary_kind_t kind;
	evobot_bounds_t bounds;
	int axis;
} evobot_nav_boundary_t;

typedef struct evobot_nav_interactor_s
{
	uint32_t id;
	evobot_host_interactor_t host;
} evobot_nav_interactor_t;

typedef struct evobot_nav_stats_s
{
	uint64_t sample_points;
	uint64_t free_points;
	uint64_t blocked_points;
	uint64_t trace_tests;
	uint64_t rejected_cells;
	uint64_t accepted_cells;
	double generation_time;
} evobot_nav_stats_t;

typedef struct evobot_nav_dataset_s
{
	evobot_nav_state_t state;
	char map_name[EVOBOT_NAV_MAP_MAX];
	uint32_t map_checksum;
	evobot_bounds_t world_bounds;
	evobot_bounds_t player_bounds;
	evobot_nav_volume_t *volumes;
	size_t volume_count;
	size_t volume_capacity;
	evobot_nav_boundary_t *boundaries;
	size_t boundary_count;
	size_t boundary_capacity;
	evobot_nav_interactor_t *interactors;
	size_t interactor_count;
	size_t interactor_capacity;
	evobot_nav_stats_t stats;
} evobot_nav_dataset_t;

typedef struct evobot_nav_buffer_s
{
	char *data;
	size_t length;
	size_t capacity;
	int failed;
} evobot_nav_buffer_t;

typedef struct evobot_nav_parser_s
{
	const char *cursor;
	const char *end;
	int failed;
} evobot_nav_parser_t;

typedef struct evobot_nav_cell_result_s
{
	int free_count;
	int blocked_count;
	int semantic_mixed;
	evobot_nav_volume_t volume;
} evobot_nav_cell_result_t;

static evobot_host_api_t evobot_nav_host;
static evobot_nav_dataset_t evobot_nav;
static int evobot_nav_initialized;
static int evobot_nav_map_loaded;
static char evobot_nav_active_map[EVOBOT_NAV_MAP_MAX];
static uint32_t evobot_nav_active_checksum;
static int evobot_nav_sort_axis;

static void EvoBot_NavPrint(const char *message)
{
	if (evobot_nav_host.print)
		evobot_nav_host.print(message);
}

static void EvoBot_NavPrintf(const char *format, ...)
{
	char message[512];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';
	EvoBot_NavPrint(message);
}

static float EvoBot_NavMin(float a, float b)
{
	return a < b ? a : b;
}

static float EvoBot_NavMax(float a, float b)
{
	return a > b ? a : b;
}

static int EvoBot_NavFloatEqual(float a, float b)
{
	return fabs(a - b) <= EVOBOT_NAV_FLOAT_EPSILON;
}

static float EvoBot_NavBoundsSize(const evobot_bounds_t *bounds, int axis)
{
	return bounds->maxs.v[axis] - bounds->mins.v[axis];
}

static void EvoBot_NavBoundsCenter(const evobot_bounds_t *bounds, evobot_vec3_t *center)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
		center->v[axis] = (bounds->mins.v[axis] + bounds->maxs.v[axis]) * 0.5f;
}

static int EvoBot_NavBoundsIntersect(const evobot_bounds_t *a, const evobot_bounds_t *b)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (a->maxs.v[axis] <= b->mins.v[axis] + EVOBOT_NAV_FLOAT_EPSILON ||
			a->mins.v[axis] >= b->maxs.v[axis] - EVOBOT_NAV_FLOAT_EPSILON)
			return 0;
	}

	return 1;
}

static int EvoBot_NavBoundsContains(const evobot_bounds_t *outer, const evobot_bounds_t *inner)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (inner->mins.v[axis] < outer->mins.v[axis] - EVOBOT_NAV_FLOAT_EPSILON ||
			inner->maxs.v[axis] > outer->maxs.v[axis] + EVOBOT_NAV_FLOAT_EPSILON)
			return 0;
	}

	return 1;
}

static int EvoBot_NavReserve(void **items, size_t *capacity, size_t count, size_t item_size)
{
	void *new_items;
	size_t new_capacity;

	if (count <= *capacity)
		return 1;

	new_capacity = *capacity ? *capacity * 2 : 256;
	while (new_capacity < count)
		new_capacity *= 2;

	new_items = realloc(*items, new_capacity * item_size);
	if (!new_items)
		return 0;

	*items = new_items;
	*capacity = new_capacity;
	return 1;
}

static int EvoBot_NavAddVolume(const evobot_nav_volume_t *volume)
{
	if (!EvoBot_NavReserve((void **)&evobot_nav.volumes, &evobot_nav.volume_capacity,
		evobot_nav.volume_count + 1, sizeof(*evobot_nav.volumes)))
		return 0;

	evobot_nav.volumes[evobot_nav.volume_count++] = *volume;
	return 1;
}

static int EvoBot_NavAddBoundary(const evobot_nav_boundary_t *boundary)
{
	if (!EvoBot_NavReserve((void **)&evobot_nav.boundaries, &evobot_nav.boundary_capacity,
		evobot_nav.boundary_count + 1, sizeof(*evobot_nav.boundaries)))
		return 0;

	evobot_nav.boundaries[evobot_nav.boundary_count++] = *boundary;
	return 1;
}

static int EvoBot_NavAddInteractor(const evobot_host_interactor_t *host)
{
	evobot_nav_interactor_t *interactor;

	if (!EvoBot_NavReserve((void **)&evobot_nav.interactors, &evobot_nav.interactor_capacity,
		evobot_nav.interactor_count + 1, sizeof(*evobot_nav.interactors)))
		return 0;

	interactor = &evobot_nav.interactors[evobot_nav.interactor_count];
	memset(interactor, 0, sizeof(*interactor));
	interactor->id = (uint32_t)evobot_nav.interactor_count;
	interactor->host = *host;
	evobot_nav.interactor_count++;
	return 1;
}

static void EvoBot_NavFreeDataset(evobot_nav_dataset_t *dataset)
{
	free(dataset->volumes);
	free(dataset->boundaries);
	free(dataset->interactors);
	memset(dataset, 0, sizeof(*dataset));
}

static int EvoBot_NavIsLiquid(evobot_contents_t contents)
{
	return contents == EVOBOT_CONTENTS_WATER ||
		contents == EVOBOT_CONTENTS_SLIME ||
		contents == EVOBOT_CONTENTS_LAVA;
}

static const char *EvoBot_NavContentsName(evobot_contents_t contents)
{
	switch (contents)
	{
	case EVOBOT_CONTENTS_AIR:
		return "air";
	case EVOBOT_CONTENTS_WATER:
		return "water";
	case EVOBOT_CONTENTS_SLIME:
		return "slime";
	case EVOBOT_CONTENTS_LAVA:
		return "lava";
	case EVOBOT_CONTENTS_SOLID:
		return "solid";
	case EVOBOT_CONTENTS_OTHER:
	default:
		return "other";
	}
}

static evobot_contents_t EvoBot_NavContentsFromName(const char *name)
{
	if (!strcmp(name, "air"))
		return EVOBOT_CONTENTS_AIR;
	if (!strcmp(name, "water"))
		return EVOBOT_CONTENTS_WATER;
	if (!strcmp(name, "slime"))
		return EVOBOT_CONTENTS_SLIME;
	if (!strcmp(name, "lava"))
		return EVOBOT_CONTENTS_LAVA;
	if (!strcmp(name, "solid"))
		return EVOBOT_CONTENTS_SOLID;
	return EVOBOT_CONTENTS_OTHER;
}

static const char *EvoBot_NavBoundaryName(evobot_nav_boundary_kind_t kind)
{
	switch (kind)
	{
	case EVOBOT_NAV_BOUNDARY_LEDGE:
		return "ledge";
	case EVOBOT_NAV_BOUNDARY_LIQUID:
		return "liquid";
	case EVOBOT_NAV_BOUNDARY_ADJACENT:
	default:
		return "adjacent";
	}
}

static evobot_nav_boundary_kind_t EvoBot_NavBoundaryFromName(const char *name)
{
	if (!strcmp(name, "ledge"))
		return EVOBOT_NAV_BOUNDARY_LEDGE;
	if (!strcmp(name, "liquid"))
		return EVOBOT_NAV_BOUNDARY_LIQUID;
	return EVOBOT_NAV_BOUNDARY_ADJACENT;
}

static const char *EvoBot_NavInteractorName(evobot_interactor_kind_t kind)
{
	switch (kind)
	{
	case EVOBOT_INTERACTOR_DOOR:
		return "door";
	case EVOBOT_INTERACTOR_BUTTON:
		return "button";
	case EVOBOT_INTERACTOR_PLATFORM:
		return "platform";
	case EVOBOT_INTERACTOR_TRAIN:
		return "train";
	case EVOBOT_INTERACTOR_TELEPORTER:
		return "teleporter";
	case EVOBOT_INTERACTOR_TELEPORT_DESTINATION:
		return "teleport_destination";
	case EVOBOT_INTERACTOR_LEVEL_EXIT:
		return "level_exit";
	case EVOBOT_INTERACTOR_LOGIC:
		return "logic";
	case EVOBOT_INTERACTOR_OTHER:
	default:
		return "other";
	}
}

static evobot_interactor_kind_t EvoBot_NavInteractorFromName(const char *name)
{
	if (!strcmp(name, "door"))
		return EVOBOT_INTERACTOR_DOOR;
	if (!strcmp(name, "button"))
		return EVOBOT_INTERACTOR_BUTTON;
	if (!strcmp(name, "platform"))
		return EVOBOT_INTERACTOR_PLATFORM;
	if (!strcmp(name, "train"))
		return EVOBOT_INTERACTOR_TRAIN;
	if (!strcmp(name, "teleporter"))
		return EVOBOT_INTERACTOR_TELEPORTER;
	if (!strcmp(name, "teleport_destination"))
		return EVOBOT_INTERACTOR_TELEPORT_DESTINATION;
	if (!strcmp(name, "level_exit"))
		return EVOBOT_INTERACTOR_LEVEL_EXIT;
	if (!strcmp(name, "logic"))
		return EVOBOT_INTERACTOR_LOGIC;
	return EVOBOT_INTERACTOR_OTHER;
}

static const char *EvoBot_NavStateName(evobot_nav_state_t state)
{
	switch (state)
	{
	case EVOBOT_NAV_STATE_GENERATED:
		return "generated";
	case EVOBOT_NAV_STATE_LOADED:
		return "loaded";
	case EVOBOT_NAV_STATE_EMPTY:
	default:
		return "empty";
	}
}

static int EvoBot_NavPointFree(const evobot_vec3_t *point)
{
	evobot_trace_t trace;
	evobot_contents_t contents;

	memset(&trace, 0, sizeof(trace));
	evobot_nav.stats.sample_points++;
	evobot_nav.stats.trace_tests++;
	if (!evobot_nav_host.trace_player_world ||
		!evobot_nav_host.trace_player_world(point, point, &trace))
	{
		evobot_nav.stats.blocked_points++;
		return 0;
	}

	contents = evobot_nav_host.point_contents ?
		evobot_nav_host.point_contents(point) : EVOBOT_CONTENTS_OTHER;
	if (trace.start_solid || trace.all_solid ||
		contents == EVOBOT_CONTENTS_SOLID || contents == EVOBOT_CONTENTS_OTHER)
	{
		evobot_nav.stats.blocked_points++;
		return 0;
	}

	evobot_nav.stats.free_points++;
	return 1;
}

static int EvoBot_NavTraceClear(const evobot_vec3_t *start, const evobot_vec3_t *end)
{
	evobot_trace_t trace;

	memset(&trace, 0, sizeof(trace));
	evobot_nav.stats.trace_tests++;
	if (!evobot_nav_host.trace_player_world ||
		!evobot_nav_host.trace_player_world(start, end, &trace))
		return 0;

	return !trace.start_solid && !trace.all_solid && trace.fraction >= 1.0f;
}

static int EvoBot_NavPointSupport(const evobot_vec3_t *point, float max_distance,
	float *floor_height, float *support_distance, evobot_vec3_t *normal)
{
	evobot_vec3_t end;
	evobot_trace_t trace;

	end = *point;
	end.v[2] -= max_distance;
	memset(&trace, 0, sizeof(trace));
	evobot_nav.stats.trace_tests++;
	if (!evobot_nav_host.trace_player_world ||
		!evobot_nav_host.trace_player_world(point, &end, &trace) ||
		trace.start_solid || trace.all_solid || trace.fraction >= 1.0f ||
		trace.normal.v[2] < EVOBOT_NAV_WALKABLE_NORMAL_Z)
		return 0;

	*support_distance = point->v[2] - trace.end.v[2];
	*floor_height = trace.end.v[2] + evobot_nav.player_bounds.mins.v[2];
	*normal = trace.normal;
	return 1;
}

static void EvoBot_NavClassifyContents(const evobot_vec3_t *point,
	evobot_contents_t *contents, int *water_level)
{
	evobot_vec3_t sample;
	evobot_contents_t feet;
	evobot_contents_t middle;
	evobot_contents_t head;
	evobot_contents_t center;

	center = evobot_nav_host.point_contents(point);
	sample = *point;
	sample.v[2] += evobot_nav.player_bounds.mins.v[2] + 1.0f;
	feet = evobot_nav_host.point_contents(&sample);

	*water_level = 0;
	*contents = EvoBot_NavIsLiquid(feet) ? feet : center;
	if (!EvoBot_NavIsLiquid(feet))
	{
		if (!EvoBot_NavIsLiquid(*contents))
			*contents = EVOBOT_CONTENTS_AIR;
		return;
	}

	*water_level = 1;
	sample.v[2] = point->v[2] +
		(evobot_nav.player_bounds.mins.v[2] + evobot_nav.player_bounds.maxs.v[2]) * 0.5f;
	middle = evobot_nav_host.point_contents(&sample);
	if (middle != feet)
		return;

	*water_level = 2;
	sample.v[2] = point->v[2] + 22.0f;
	head = evobot_nav_host.point_contents(&sample);
	if (head == feet)
		*water_level = 3;
}

static int EvoBot_NavDynamicForBounds(const evobot_bounds_t *bounds, int *partial)
{
	int found = EVOBOT_NAV_INVALID_ID;
	size_t i;

	*partial = 0;
	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		evobot_bounds_t expanded;
		int axis;

		if (!evobot_nav.interactors[i].host.dynamic_brush)
			continue;

		for (axis = 0; axis < 3; axis++)
		{
			expanded.mins.v[axis] = evobot_nav.interactors[i].host.swept_bounds.mins.v[axis] -
				evobot_nav.player_bounds.maxs.v[axis];
			expanded.maxs.v[axis] = evobot_nav.interactors[i].host.swept_bounds.maxs.v[axis] -
				evobot_nav.player_bounds.mins.v[axis];
		}

		if (!EvoBot_NavBoundsIntersect(bounds, &expanded))
			continue;
		if (!EvoBot_NavBoundsContains(&expanded, bounds))
			*partial = 1;
		if (found == EVOBOT_NAV_INVALID_ID)
			found = (int)i;
		else if (found != (int)i)
			found = EVOBOT_NAV_MULTIPLE_DYNAMIC;
	}

	return found;
}

static void EvoBot_NavCellPoints(const evobot_bounds_t *bounds, float points[3][3])
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float inset = EvoBot_NavMin(EVOBOT_NAV_EPSILON,
			EvoBot_NavBoundsSize(bounds, axis) * 0.125f);
		points[axis][0] = bounds->mins.v[axis] + inset;
		points[axis][1] = (bounds->mins.v[axis] + bounds->maxs.v[axis]) * 0.5f;
		points[axis][2] = bounds->maxs.v[axis] - inset;
	}
}

static int EvoBot_NavValidateSweeps(float points[3][3])
{
	evobot_vec3_t start;
	evobot_vec3_t end;
	int axis;
	int a;
	int b;

	for (axis = 0; axis < 3; axis++)
	{
		int axis_a = (axis + 1) % 3;
		int axis_b = (axis + 2) % 3;
		for (a = 0; a < 3; a++)
		{
			for (b = 0; b < 3; b++)
			{
				start.v[axis] = points[axis][0];
				end.v[axis] = points[axis][2];
				start.v[axis_a] = end.v[axis_a] = points[axis_a][a];
				start.v[axis_b] = end.v[axis_b] = points[axis_b][b];
				if (!EvoBot_NavTraceClear(&start, &end))
					return 0;
			}
		}
	}

	return 1;
}

static void EvoBot_NavEvaluateCell(const evobot_bounds_t *bounds, evobot_nav_cell_result_t *result)
{
	float points[3][3];
	evobot_contents_t first_contents = EVOBOT_CONTENTS_OTHER;
	int first_water_level = -1;
	int x;
	int y;
	int z;
	int partial_dynamic;
	evobot_vec3_t center;
	float support_limit;

	memset(result, 0, sizeof(*result));
	result->volume.bounds = *bounds;
	result->volume.dynamic_interactor = EVOBOT_NAV_INVALID_ID;
	EvoBot_NavCellPoints(bounds, points);

	for (x = 0; x < 3; x++)
	{
		for (y = 0; y < 3; y++)
		{
			for (z = 0; z < 3; z++)
			{
				evobot_vec3_t point;
				evobot_contents_t contents;
				int water_level;

				point.v[0] = points[0][x];
				point.v[1] = points[1][y];
				point.v[2] = points[2][z];
				if (!EvoBot_NavPointFree(&point))
				{
					result->blocked_count++;
					continue;
				}

				result->free_count++;
				EvoBot_NavClassifyContents(&point, &contents, &water_level);
				if (first_water_level < 0)
				{
					first_contents = contents;
					first_water_level = water_level;
				}
				else if (contents != first_contents || water_level != first_water_level)
				{
					result->semantic_mixed = 1;
				}
			}
		}
	}

	if (result->free_count != 27)
		return;
	if (!EvoBot_NavValidateSweeps(points))
	{
		result->blocked_count = 1;
		return;
	}

	result->volume.contents = first_contents;
	result->volume.water_level = first_water_level;
	result->volume.dynamic_interactor = EvoBot_NavDynamicForBounds(bounds, &partial_dynamic);
	if (partial_dynamic)
		result->semantic_mixed = 1;

	EvoBot_NavBoundsCenter(bounds, &center);
	support_limit = EvoBot_NavBoundsSize(bounds, 2) * 0.5f + 1.0f;
	if (EvoBot_NavPointSupport(&center, support_limit,
		&result->volume.floor_height, &result->volume.support_distance,
		&result->volume.support_normal))
	{
		result->volume.supported = 1;
	}
}

static int EvoBot_NavSubdivideCell(const evobot_bounds_t *bounds)
{
	evobot_nav_cell_result_t result;
	float size;
	int should_refine;

	EvoBot_NavEvaluateCell(bounds, &result);
	size = EvoBot_NavMax(EvoBot_NavBoundsSize(bounds, 0),
		EvoBot_NavMax(EvoBot_NavBoundsSize(bounds, 1), EvoBot_NavBoundsSize(bounds, 2)));

	if (result.free_count == 0)
		should_refine = size > EVOBOT_NAV_COARSE_SIZE * 0.5f + EVOBOT_NAV_FLOAT_EPSILON;
	else
		should_refine = size > EVOBOT_NAV_MIN_SIZE + EVOBOT_NAV_FLOAT_EPSILON &&
			(result.free_count != 27 || result.semantic_mixed ||
			(result.volume.supported && size > EVOBOT_NAV_FINE_SIZE + EVOBOT_NAV_FLOAT_EPSILON));

	if (should_refine)
	{
		evobot_vec3_t middle;
		int x;
		int y;
		int z;

		EvoBot_NavBoundsCenter(bounds, &middle);
		for (x = 0; x < 2; x++)
		{
			for (y = 0; y < 2; y++)
			{
				for (z = 0; z < 2; z++)
				{
					evobot_bounds_t child;

					child.mins.v[0] = x ? middle.v[0] : bounds->mins.v[0];
					child.maxs.v[0] = x ? bounds->maxs.v[0] : middle.v[0];
					child.mins.v[1] = y ? middle.v[1] : bounds->mins.v[1];
					child.maxs.v[1] = y ? bounds->maxs.v[1] : middle.v[1];
					child.mins.v[2] = z ? middle.v[2] : bounds->mins.v[2];
					child.maxs.v[2] = z ? bounds->maxs.v[2] : middle.v[2];
					if (!EvoBot_NavSubdivideCell(&child))
						return 0;
				}
			}
		}
		return 1;
	}

	if (result.free_count == 27 && !result.blocked_count && !result.semantic_mixed)
	{
		result.volume.id = (uint32_t)evobot_nav.volume_count;
		if (!EvoBot_NavAddVolume(&result.volume))
			return 0;
		evobot_nav.stats.accepted_cells++;
	}
	else
	{
		evobot_nav.stats.rejected_cells++;
	}

	return 1;
}

static int EvoBot_NavCaptureInteractors(void)
{
	int count;
	int i;

	if (!evobot_nav_host.interactor_count || !evobot_nav_host.get_interactor)
		return 0;

	count = evobot_nav_host.interactor_count();
	if (count < 0)
		return 0;

	for (i = 0; i < count; i++)
	{
		evobot_host_interactor_t interactor;

		memset(&interactor, 0, sizeof(interactor));
		if (!evobot_nav_host.get_interactor(i, &interactor) ||
			!EvoBot_NavAddInteractor(&interactor))
			return 0;
	}

	return 1;
}

static int EvoBot_NavVolumeCompatible(const evobot_nav_volume_t *a,
	const evobot_nav_volume_t *b)
{
	if (a->contents != b->contents || a->supported != b->supported ||
		a->water_level != b->water_level ||
		a->dynamic_interactor != b->dynamic_interactor)
		return 0;

	if (a->supported)
	{
		float normal_dot;

		if (fabs(a->floor_height - b->floor_height) > 1.0f)
			return 0;
		normal_dot = a->support_normal.v[0] * b->support_normal.v[0] +
			a->support_normal.v[1] * b->support_normal.v[1] +
			a->support_normal.v[2] * b->support_normal.v[2];
		if (normal_dot < 0.99f)
			return 0;
	}

	return 1;
}

static int EvoBot_NavValidateMergedVolume(const evobot_nav_volume_t *volume)
{
	float points[3][3];
	int x;
	int y;
	int z;
	int partial_dynamic;
	int dynamic_interactor;

	EvoBot_NavCellPoints(&volume->bounds, points);
	for (x = 0; x < 3; x++)
	{
		for (y = 0; y < 3; y++)
		{
			for (z = 0; z < 3; z++)
			{
				evobot_vec3_t point;
				evobot_contents_t contents;
				int water_level;

				point.v[0] = points[0][x];
				point.v[1] = points[1][y];
				point.v[2] = points[2][z];
				if (!EvoBot_NavPointFree(&point))
					return 0;
				EvoBot_NavClassifyContents(&point, &contents, &water_level);
				if (contents != volume->contents || water_level != volume->water_level)
					return 0;
			}
		}
	}

	if (!EvoBot_NavValidateSweeps(points))
		return 0;
	dynamic_interactor = EvoBot_NavDynamicForBounds(&volume->bounds, &partial_dynamic);
	if (partial_dynamic || dynamic_interactor != volume->dynamic_interactor)
		return 0;

	if (volume->supported)
	{
		for (x = 0; x < 3; x++)
		{
			for (y = 0; y < 3; y++)
			{
				evobot_vec3_t point;
				evobot_vec3_t normal;
				float floor_height;
				float support_distance;

				point.v[0] = points[0][x];
				point.v[1] = points[1][y];
				point.v[2] = points[2][1];
				if (!EvoBot_NavPointSupport(&point,
					EvoBot_NavBoundsSize(&volume->bounds, 2) * 0.5f + 1.0f,
					&floor_height, &support_distance, &normal) ||
					fabs(floor_height - volume->floor_height) > 1.0f)
					return 0;
			}
		}
	}

	return 1;
}

static int EvoBot_NavCompareFloat(float a, float b)
{
	if (a < b - EVOBOT_NAV_FLOAT_EPSILON)
		return -1;
	if (a > b + EVOBOT_NAV_FLOAT_EPSILON)
		return 1;
	return 0;
}

static int EvoBot_NavVolumeCompare(const void *left, const void *right)
{
	const evobot_nav_volume_t *a = (const evobot_nav_volume_t *)left;
	const evobot_nav_volume_t *b = (const evobot_nav_volume_t *)right;
	int axes[2];
	int result;

	if (a->contents != b->contents)
		return (int)a->contents - (int)b->contents;
	if (a->supported != b->supported)
		return a->supported - b->supported;
	if (a->water_level != b->water_level)
		return a->water_level - b->water_level;
	if (a->dynamic_interactor != b->dynamic_interactor)
		return a->dynamic_interactor - b->dynamic_interactor;
	if (a->supported)
	{
		result = EvoBot_NavCompareFloat(a->floor_height, b->floor_height);
		if (result)
			return result;
	}

	axes[0] = (evobot_nav_sort_axis + 1) % 3;
	axes[1] = (evobot_nav_sort_axis + 2) % 3;
	result = EvoBot_NavCompareFloat(a->bounds.mins.v[axes[0]], b->bounds.mins.v[axes[0]]);
	if (result)
		return result;
	result = EvoBot_NavCompareFloat(a->bounds.maxs.v[axes[0]], b->bounds.maxs.v[axes[0]]);
	if (result)
		return result;
	result = EvoBot_NavCompareFloat(a->bounds.mins.v[axes[1]], b->bounds.mins.v[axes[1]]);
	if (result)
		return result;
	result = EvoBot_NavCompareFloat(a->bounds.maxs.v[axes[1]], b->bounds.maxs.v[axes[1]]);
	if (result)
		return result;
	return EvoBot_NavCompareFloat(a->bounds.mins.v[evobot_nav_sort_axis],
		b->bounds.mins.v[evobot_nav_sort_axis]);
}

static int EvoBot_NavCanMerge(const evobot_nav_volume_t *a,
	const evobot_nav_volume_t *b, int axis, evobot_nav_volume_t *merged)
{
	int other_a = (axis + 1) % 3;
	int other_b = (axis + 2) % 3;

	if (!EvoBot_NavVolumeCompatible(a, b) ||
		!EvoBot_NavFloatEqual(a->bounds.mins.v[other_a], b->bounds.mins.v[other_a]) ||
		!EvoBot_NavFloatEqual(a->bounds.maxs.v[other_a], b->bounds.maxs.v[other_a]) ||
		!EvoBot_NavFloatEqual(a->bounds.mins.v[other_b], b->bounds.mins.v[other_b]) ||
		!EvoBot_NavFloatEqual(a->bounds.maxs.v[other_b], b->bounds.maxs.v[other_b]) ||
		!EvoBot_NavFloatEqual(a->bounds.maxs.v[axis], b->bounds.mins.v[axis]))
		return 0;

	*merged = *a;
	merged->bounds.maxs.v[axis] = b->bounds.maxs.v[axis];
	return EvoBot_NavValidateMergedVolume(merged);
}

static int EvoBot_NavMergeAxis(int axis)
{
	size_t read_index;
	size_t write_index;
	int merged_any = 0;

	if (evobot_nav.volume_count < 2)
		return 0;

	evobot_nav_sort_axis = axis;
	qsort(evobot_nav.volumes, evobot_nav.volume_count, sizeof(*evobot_nav.volumes),
		EvoBot_NavVolumeCompare);

	read_index = 0;
	write_index = 0;
	while (read_index < evobot_nav.volume_count)
	{
		evobot_nav_volume_t merged;

		if (read_index + 1 < evobot_nav.volume_count &&
			EvoBot_NavCanMerge(&evobot_nav.volumes[read_index],
				&evobot_nav.volumes[read_index + 1], axis, &merged))
		{
			evobot_nav.volumes[write_index++] = merged;
			read_index += 2;
			merged_any = 1;
		}
		else
		{
			evobot_nav.volumes[write_index++] = evobot_nav.volumes[read_index++];
		}
	}

	evobot_nav.volume_count = write_index;
	return merged_any;
}

static void EvoBot_NavMergeVolumes(void)
{
	int changed;
	int axis;

	do
	{
		changed = 0;
		for (axis = 0; axis < 3; axis++)
			changed |= EvoBot_NavMergeAxis(axis);
	}
	while (changed);

	for (axis = 0; axis < (int)evobot_nav.volume_count; axis++)
		evobot_nav.volumes[axis].id = (uint32_t)axis;
}

static int EvoBot_NavSharedFace(const evobot_nav_volume_t *a,
	const evobot_nav_volume_t *b, evobot_bounds_t *face, int *face_axis)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		int other_a = (axis + 1) % 3;
		int other_b = (axis + 2) % 3;
		float position;

		if (EvoBot_NavFloatEqual(a->bounds.maxs.v[axis], b->bounds.mins.v[axis]))
			position = a->bounds.maxs.v[axis];
		else if (EvoBot_NavFloatEqual(b->bounds.maxs.v[axis], a->bounds.mins.v[axis]))
			position = a->bounds.mins.v[axis];
		else
			continue;

		face->mins.v[other_a] = EvoBot_NavMax(a->bounds.mins.v[other_a], b->bounds.mins.v[other_a]);
		face->maxs.v[other_a] = EvoBot_NavMin(a->bounds.maxs.v[other_a], b->bounds.maxs.v[other_a]);
		face->mins.v[other_b] = EvoBot_NavMax(a->bounds.mins.v[other_b], b->bounds.mins.v[other_b]);
		face->maxs.v[other_b] = EvoBot_NavMin(a->bounds.maxs.v[other_b], b->bounds.maxs.v[other_b]);
		if (face->maxs.v[other_a] - face->mins.v[other_a] <= EVOBOT_NAV_FLOAT_EPSILON ||
			face->maxs.v[other_b] - face->mins.v[other_b] <= EVOBOT_NAV_FLOAT_EPSILON)
			continue;

		face->mins.v[axis] = face->maxs.v[axis] = position;
		*face_axis = axis;
		return 1;
	}

	return 0;
}

static evobot_nav_boundary_kind_t EvoBot_NavClassifyBoundary(
	const evobot_nav_volume_t *a, const evobot_nav_volume_t *b, int axis)
{
	if (a->contents != b->contents &&
		(EvoBot_NavIsLiquid(a->contents) || EvoBot_NavIsLiquid(b->contents)))
		return EVOBOT_NAV_BOUNDARY_LIQUID;

	if (axis != 2 &&
		(a->supported != b->supported ||
		(a->supported && b->supported &&
			fabs(a->floor_height - b->floor_height) > EVOBOT_NAV_STEP_SIZE)))
		return EVOBOT_NAV_BOUNDARY_LEDGE;

	return EVOBOT_NAV_BOUNDARY_ADJACENT;
}

static int EvoBot_NavGenerateBoundaries(void)
{
	size_t i;
	size_t j;

	for (i = 0; i < evobot_nav.volume_count; i++)
	{
		for (j = i + 1; j < evobot_nav.volume_count; j++)
		{
			evobot_nav_boundary_t boundary;

			memset(&boundary, 0, sizeof(boundary));
			if (!EvoBot_NavSharedFace(&evobot_nav.volumes[i], &evobot_nav.volumes[j],
				&boundary.bounds, &boundary.axis))
				continue;

			boundary.id = (uint32_t)evobot_nav.boundary_count;
			boundary.volume_a = (int)i;
			boundary.volume_b = (int)j;
			boundary.kind = EvoBot_NavClassifyBoundary(&evobot_nav.volumes[i],
				&evobot_nav.volumes[j], boundary.axis);
			if (!EvoBot_NavAddBoundary(&boundary))
				return 0;
		}
	}

	return 1;
}

static int EvoBot_NavGenerateExteriorLedges(void)
{
	size_t i;
	int side;

	for (i = 0; i < evobot_nav.volume_count; i++)
	{
		evobot_nav_volume_t *volume = &evobot_nav.volumes[i];

		if (!volume->supported)
			continue;

		for (side = 0; side < 4; side++)
		{
			int axis = side / 2;
			int high_side = side & 1;
			evobot_vec3_t probe;
			evobot_vec3_t normal;
			float floor_height;
			float support_distance;
			int already_covered = 0;
			size_t j;

			EvoBot_NavBoundsCenter(&volume->bounds, &probe);
			probe.v[axis] = high_side ? volume->bounds.maxs.v[axis] + 1.0f :
				volume->bounds.mins.v[axis] - 1.0f;
			for (j = 0; j < evobot_nav.boundary_count; j++)
			{
				evobot_nav_boundary_t *boundary = &evobot_nav.boundaries[j];
				if (boundary->axis == axis &&
					(boundary->volume_a == (int)i || boundary->volume_b == (int)i) &&
					EvoBot_NavFloatEqual(boundary->bounds.mins.v[axis],
						high_side ? volume->bounds.maxs.v[axis] : volume->bounds.mins.v[axis]))
				{
					already_covered = 1;
					break;
				}
			}
			if (already_covered || !EvoBot_NavPointFree(&probe) ||
				EvoBot_NavPointSupport(&probe, EVOBOT_NAV_STEP_SIZE,
					&floor_height, &support_distance, &normal))
				continue;

			{
				evobot_nav_boundary_t boundary;

				memset(&boundary, 0, sizeof(boundary));
				boundary.id = (uint32_t)evobot_nav.boundary_count;
				boundary.volume_a = (int)i;
				boundary.volume_b = EVOBOT_NAV_INVALID_ID;
				boundary.kind = EVOBOT_NAV_BOUNDARY_LEDGE;
				boundary.axis = axis;
				boundary.bounds = volume->bounds;
				boundary.bounds.mins.v[axis] = boundary.bounds.maxs.v[axis] =
					high_side ? volume->bounds.maxs.v[axis] : volume->bounds.mins.v[axis];
				if (!EvoBot_NavAddBoundary(&boundary))
					return 0;
			}
		}
	}

	return 1;
}

static int EvoBot_NavGenerateExteriorLiquidBoundaries(void)
{
	size_t i;
	int side;

	for (i = 0; i < evobot_nav.volume_count; i++)
	{
		evobot_nav_volume_t *volume = &evobot_nav.volumes[i];
		float probe_offset = EVOBOT_NAV_MIN_SIZE + 1.0f;

		if (!EvoBot_NavIsLiquid(volume->contents))
			continue;

		for (side = 0; side < 6; side++)
		{
			int axis = side / 2;
			int high_side = side & 1;
			evobot_vec3_t probe;
			evobot_contents_t contents;
			int water_level;

			EvoBot_NavBoundsCenter(&volume->bounds, &probe);
			probe.v[axis] = high_side ? volume->bounds.maxs.v[axis] + probe_offset :
				volume->bounds.mins.v[axis] - probe_offset;
			if (!EvoBot_NavPointFree(&probe))
				continue;
			EvoBot_NavClassifyContents(&probe, &contents, &water_level);
			if (contents == volume->contents ||
				(!EvoBot_NavIsLiquid(contents) && !EvoBot_NavIsLiquid(volume->contents)))
				continue;

			{
				evobot_nav_boundary_t boundary;

				memset(&boundary, 0, sizeof(boundary));
				boundary.id = (uint32_t)evobot_nav.boundary_count;
				boundary.volume_a = (int)i;
				boundary.volume_b = EVOBOT_NAV_INVALID_ID;
				boundary.kind = EVOBOT_NAV_BOUNDARY_LIQUID;
				boundary.axis = axis;
				boundary.bounds = volume->bounds;
				boundary.bounds.mins.v[axis] = boundary.bounds.maxs.v[axis] =
					high_side ? volume->bounds.maxs.v[axis] : volume->bounds.mins.v[axis];
				if (!EvoBot_NavAddBoundary(&boundary))
					return 0;
			}
		}
	}

	return 1;
}

static int EvoBot_NavBuildSamples(void)
{
	float aligned_mins[3];
	float aligned_maxs[3];
	float x;
	float y;
	float z;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		aligned_mins[axis] = floorf(evobot_nav.world_bounds.mins.v[axis] /
			EVOBOT_NAV_COARSE_SIZE) * EVOBOT_NAV_COARSE_SIZE;
		aligned_maxs[axis] = ceilf(evobot_nav.world_bounds.maxs.v[axis] /
			EVOBOT_NAV_COARSE_SIZE) * EVOBOT_NAV_COARSE_SIZE;
	}

	for (x = aligned_mins[0]; x < aligned_maxs[0]; x += EVOBOT_NAV_COARSE_SIZE)
	{
		for (y = aligned_mins[1]; y < aligned_maxs[1]; y += EVOBOT_NAV_COARSE_SIZE)
		{
			for (z = aligned_mins[2]; z < aligned_maxs[2]; z += EVOBOT_NAV_COARSE_SIZE)
			{
				evobot_bounds_t cell;

				cell.mins.v[0] = x;
				cell.maxs.v[0] = x + EVOBOT_NAV_COARSE_SIZE;
				cell.mins.v[1] = y;
				cell.maxs.v[1] = y + EVOBOT_NAV_COARSE_SIZE;
				cell.mins.v[2] = z;
				cell.maxs.v[2] = z + EVOBOT_NAV_COARSE_SIZE;
				if (!EvoBot_NavSubdivideCell(&cell))
					return 0;
			}
		}
	}

	return 1;
}

static int EvoBot_NavMapReady(void)
{
	if (!evobot_nav_initialized || !evobot_nav_map_loaded)
	{
		EvoBot_NavPrint("EvoBot navigation: no map is loaded\n");
		return 0;
	}
	return 1;
}

static void EvoBot_NavPath(char *path, size_t path_size, const char *map_name,
	const char *extension, int debug)
{
	if (debug)
		snprintf(path, path_size, "evobot/nav/debug/%s.%s", map_name, extension);
	else
		snprintf(path, path_size, "evobot/nav/%s.%s", map_name, extension);
	path[path_size - 1] = '\0';
}

static int EvoBot_NavSafeMapName(const char *name)
{
	const unsigned char *c = (const unsigned char *)name;

	if (!name || !name[0])
		return 0;
	while (*c)
	{
		if (!isalnum(*c) && *c != '_' && *c != '-')
			return 0;
		c++;
	}
	return 1;
}

void EvoBot_NavInit(const evobot_host_api_t *host)
{
	if (evobot_nav_initialized)
		return;

	memset(&evobot_nav_host, 0, sizeof(evobot_nav_host));
	if (host)
		evobot_nav_host = *host;
	evobot_nav_initialized = 1;
}

void EvoBot_NavMapLoaded(const char *map_name, uint32_t map_checksum)
{
	if (!evobot_nav_initialized)
		return;

	EvoBot_NavFreeDataset(&evobot_nav);
	snprintf(evobot_nav_active_map, sizeof(evobot_nav_active_map), "%s",
		map_name ? map_name : "");
	evobot_nav_active_map[sizeof(evobot_nav_active_map) - 1] = '\0';
	evobot_nav_active_checksum = map_checksum;
	evobot_nav_map_loaded = 1;
}

void EvoBot_NavMapCleared(void)
{
	EvoBot_NavFreeDataset(&evobot_nav);
	evobot_nav_map_loaded = 0;
	evobot_nav_active_map[0] = '\0';
	evobot_nav_active_checksum = 0;
}

void EvoBot_NavShutdown(void)
{
	EvoBot_NavFreeDataset(&evobot_nav);
	memset(&evobot_nav_host, 0, sizeof(evobot_nav_host));
	evobot_nav_initialized = 0;
	evobot_nav_map_loaded = 0;
	evobot_nav_active_map[0] = '\0';
	evobot_nav_active_checksum = 0;
}

void EvoBot_NavClear(void)
{
	if (!EvoBot_NavMapReady())
		return;

	EvoBot_NavFreeDataset(&evobot_nav);
	EvoBot_NavPrint("EvoBot navigation cleared\n");
}

void EvoBot_NavGenerate(void)
{
	double start_time;
	size_t temporary_volume_count;

	if (!EvoBot_NavMapReady())
		return;
	if (!evobot_nav_host.world_bounds || !evobot_nav_host.player_bounds ||
		!evobot_nav_host.trace_player_world || !evobot_nav_host.point_contents ||
		!evobot_nav_host.monotonic_time)
	{
		EvoBot_NavPrint("EvoBot navigation: required world queries are unavailable\n");
		return;
	}

	EvoBot_NavFreeDataset(&evobot_nav);
	start_time = evobot_nav_host.monotonic_time();
	snprintf(evobot_nav.map_name, sizeof(evobot_nav.map_name), "%s", evobot_nav_active_map);
	evobot_nav.map_checksum = evobot_nav_active_checksum;
	if (!evobot_nav_host.world_bounds(&evobot_nav.world_bounds) ||
		!evobot_nav_host.player_bounds(&evobot_nav.player_bounds) ||
		!EvoBot_NavCaptureInteractors() || !EvoBot_NavBuildSamples())
	{
		EvoBot_NavFreeDataset(&evobot_nav);
		EvoBot_NavPrint("EvoBot navigation: generation failed\n");
		return;
	}

	temporary_volume_count = evobot_nav.volume_count;
	EvoBot_NavMergeVolumes();
	if (!EvoBot_NavGenerateBoundaries() || !EvoBot_NavGenerateExteriorLedges() ||
		!EvoBot_NavGenerateExteriorLiquidBoundaries())
	{
		EvoBot_NavFreeDataset(&evobot_nav);
		EvoBot_NavPrint("EvoBot navigation: boundary generation failed\n");
		return;
	}

	evobot_nav.stats.generation_time = evobot_nav_host.monotonic_time() - start_time;
	evobot_nav.state = EVOBOT_NAV_STATE_GENERATED;
	EvoBot_NavPrint("EvoBot navigation generated\n");
	EvoBot_NavPrintf("temporary samples tested: %" PRIu64 "\n", evobot_nav.stats.sample_points);
	EvoBot_NavPrintf("occupied samples: %" PRIu64 "\n", evobot_nav.stats.free_points);
	EvoBot_NavPrintf("rejected samples: %" PRIu64 "\n", evobot_nav.stats.blocked_points);
	EvoBot_NavPrintf("temporary volumes: %u\n", (unsigned)temporary_volume_count);
	EvoBot_NavPrintf("volumes: %u\n", (unsigned)evobot_nav.volume_count);
	EvoBot_NavPrintf("boundaries: %u\n", (unsigned)evobot_nav.boundary_count);
	EvoBot_NavPrintf("volume reduction ratio: %.2f:1\n",
		evobot_nav.volume_count ? (double)temporary_volume_count / evobot_nav.volume_count : 0.0);
	EvoBot_NavPrintf("generation time: %.3f seconds\n", evobot_nav.stats.generation_time);
}

void EvoBot_NavPrintStatus(void)
{
	size_t supported = 0;
	size_t unsupported = 0;
	size_t water = 0;
	size_t slime = 0;
	size_t lava = 0;
	size_t adjacent = 0;
	size_t ledge = 0;
	size_t liquid = 0;
	size_t i;

	for (i = 0; i < evobot_nav.volume_count; i++)
	{
		evobot_nav_volume_t *volume = &evobot_nav.volumes[i];
		if (volume->supported)
			supported++;
		else
			unsupported++;
		if (volume->contents == EVOBOT_CONTENTS_WATER)
			water++;
		else if (volume->contents == EVOBOT_CONTENTS_SLIME)
			slime++;
		else if (volume->contents == EVOBOT_CONTENTS_LAVA)
			lava++;
	}
	for (i = 0; i < evobot_nav.boundary_count; i++)
	{
		if (evobot_nav.boundaries[i].kind == EVOBOT_NAV_BOUNDARY_LEDGE)
			ledge++;
		else if (evobot_nav.boundaries[i].kind == EVOBOT_NAV_BOUNDARY_LIQUID)
			liquid++;
		else
			adjacent++;
	}

	EvoBot_NavPrint("EvoBot navigation\n");
	EvoBot_NavPrintf("map: %s\n", evobot_nav_map_loaded ? evobot_nav_active_map : "<none>");
	EvoBot_NavPrintf("checksum: %" PRIu32 "\n", evobot_nav_map_loaded ? evobot_nav_active_checksum : 0);
	EvoBot_NavPrintf("state: %s\n", EvoBot_NavStateName(evobot_nav.state));
	EvoBot_NavPrintf("volumes: %u\n", (unsigned)evobot_nav.volume_count);
	EvoBot_NavPrintf("boundaries: %u\n", (unsigned)evobot_nav.boundary_count);
	EvoBot_NavPrintf("interactors: %u\n", (unsigned)evobot_nav.interactor_count);
	EvoBot_NavPrintf("supported volumes: %u\n", (unsigned)supported);
	EvoBot_NavPrintf("unsupported volumes: %u\n", (unsigned)unsupported);
	EvoBot_NavPrintf("water volumes: %u\n", (unsigned)water);
	EvoBot_NavPrintf("slime volumes: %u\n", (unsigned)slime);
	EvoBot_NavPrintf("lava volumes: %u\n", (unsigned)lava);
	EvoBot_NavPrintf("ordinary boundaries: %u\n", (unsigned)adjacent);
	EvoBot_NavPrintf("ledge boundaries: %u\n", (unsigned)ledge);
	EvoBot_NavPrintf("liquid boundaries: %u\n", (unsigned)liquid);
	EvoBot_NavPrintf("generation time: %.3f seconds\n", evobot_nav.stats.generation_time);
}

static int EvoBot_NavBufferReserve(evobot_nav_buffer_t *buffer, size_t extra)
{
	char *new_data;
	size_t required;
	size_t capacity;

	if (buffer->failed)
		return 0;
	required = buffer->length + extra + 1;
	if (required <= buffer->capacity)
		return 1;

	capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
	while (capacity < required)
		capacity *= 2;
	new_data = (char *)realloc(buffer->data, capacity);
	if (!new_data)
	{
		buffer->failed = 1;
		return 0;
	}

	buffer->data = new_data;
	buffer->capacity = capacity;
	return 1;
}

static int EvoBot_NavBufferAppend(evobot_nav_buffer_t *buffer, const char *text)
{
	size_t length = strlen(text);

	if (!EvoBot_NavBufferReserve(buffer, length))
		return 0;
	memcpy(buffer->data + buffer->length, text, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
	return 1;
}

static int EvoBot_NavBufferAppendFormat(evobot_nav_buffer_t *buffer, const char *format, ...)
{
	char local[512];
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(local, sizeof(local), format, args);
	va_end(args);
	if (length < 0)
	{
		buffer->failed = 1;
		return 0;
	}
	if ((size_t)length < sizeof(local))
		return EvoBot_NavBufferAppend(buffer, local);

	if (!EvoBot_NavBufferReserve(buffer, (size_t)length))
		return 0;
	va_start(args, format);
	vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, args);
	va_end(args);
	buffer->length += (size_t)length;
	return 1;
}

static int EvoBot_NavBufferAppendJsonString(evobot_nav_buffer_t *buffer, const char *text)
{
	const unsigned char *c = (const unsigned char *)text;

	if (!EvoBot_NavBufferAppend(buffer, "\""))
		return 0;
	while (*c)
	{
		char escaped[8];

		switch (*c)
		{
		case '\\':
			if (!EvoBot_NavBufferAppend(buffer, "\\\\"))
				return 0;
			break;
		case '"':
			if (!EvoBot_NavBufferAppend(buffer, "\\\""))
				return 0;
			break;
		case '\n':
			if (!EvoBot_NavBufferAppend(buffer, "\\n"))
				return 0;
			break;
		case '\r':
			if (!EvoBot_NavBufferAppend(buffer, "\\r"))
				return 0;
			break;
		case '\t':
			if (!EvoBot_NavBufferAppend(buffer, "\\t"))
				return 0;
			break;
		default:
			if (*c < 32)
			{
				snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*c);
				if (!EvoBot_NavBufferAppend(buffer, escaped))
					return 0;
			}
			else
			{
				if (!EvoBot_NavBufferReserve(buffer, 1))
					return 0;
				buffer->data[buffer->length++] = (char)*c;
				buffer->data[buffer->length] = '\0';
			}
			break;
		}
		c++;
	}
	return EvoBot_NavBufferAppend(buffer, "\"");
}

static int EvoBot_NavWriteVec3(evobot_nav_buffer_t *buffer, const evobot_vec3_t *vector)
{
	return EvoBot_NavBufferAppendFormat(buffer, "[%.6g, %.6g, %.6g]",
		vector->v[0], vector->v[1], vector->v[2]);
}

static int EvoBot_NavWriteBounds(evobot_nav_buffer_t *buffer, const evobot_bounds_t *bounds)
{
	if (!EvoBot_NavBufferAppend(buffer, "{\"mins\": ") ||
		!EvoBot_NavWriteVec3(buffer, &bounds->mins) ||
		!EvoBot_NavBufferAppend(buffer, ", \"maxs\": ") ||
		!EvoBot_NavWriteVec3(buffer, &bounds->maxs) ||
		!EvoBot_NavBufferAppend(buffer, "}"))
		return 0;
	return 1;
}

static int EvoBot_NavSerializeJson(evobot_nav_buffer_t *buffer)
{
	size_t i;

	if (!EvoBot_NavBufferAppend(buffer, "{\n  \"format\": \"evobot-nav\",\n") ||
		!EvoBot_NavBufferAppendFormat(buffer, "  \"version\": %d,\n", EVOBOT_NAV_FORMAT_VERSION) ||
		!EvoBot_NavBufferAppend(buffer, "  \"map\": {\"name\": ") ||
		!EvoBot_NavBufferAppendJsonString(buffer, evobot_nav.map_name) ||
		!EvoBot_NavBufferAppendFormat(buffer, ", \"checksum\": %" PRIu32 "},\n",
			evobot_nav.map_checksum) ||
		!EvoBot_NavBufferAppend(buffer, "  \"generation\": {\n") ||
		!EvoBot_NavBufferAppendFormat(buffer,
			"    \"coarse_size\": %.0f, \"fine_size\": %.0f, \"minimum_size\": %.0f,\n",
			EVOBOT_NAV_COARSE_SIZE, EVOBOT_NAV_FINE_SIZE, EVOBOT_NAV_MIN_SIZE) ||
		!EvoBot_NavBufferAppendFormat(buffer,
			"    \"step_size\": %.0f, \"minimum_floor_normal_z\": %.3g, \"support_probe\": %.0f,\n",
			EVOBOT_NAV_STEP_SIZE, EVOBOT_NAV_WALKABLE_NORMAL_Z, EVOBOT_NAV_SUPPORT_PROBE) ||
		!EvoBot_NavBufferAppend(buffer, "    \"world_bounds\": ") ||
		!EvoBot_NavWriteBounds(buffer, &evobot_nav.world_bounds) ||
		!EvoBot_NavBufferAppend(buffer, ",\n    \"player_hull\": ") ||
		!EvoBot_NavWriteBounds(buffer, &evobot_nav.player_bounds) ||
		!EvoBot_NavBufferAppendFormat(buffer,
			",\n    \"temporary_samples\": %" PRIu64 ", \"accepted_cells\": %" PRIu64
			", \"rejected_cells\": %" PRIu64 ",\n    \"generation_time\": %.6f\n  },\n",
			evobot_nav.stats.sample_points, evobot_nav.stats.accepted_cells,
			evobot_nav.stats.rejected_cells, evobot_nav.stats.generation_time) ||
		!EvoBot_NavBufferAppend(buffer, "  \"volumes\": [\n"))
		return 0;

	for (i = 0; i < evobot_nav.volume_count; i++)
	{
		evobot_nav_volume_t *volume = &evobot_nav.volumes[i];

		if (!EvoBot_NavBufferAppendFormat(buffer, "    {\"id\": %" PRIu32 ", \"bounds\": ", volume->id) ||
			!EvoBot_NavWriteBounds(buffer, &volume->bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"contents\": ") ||
			!EvoBot_NavBufferAppendJsonString(buffer, EvoBot_NavContentsName(volume->contents)) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"supported\": %s, \"floor_height\": %.6g, \"support_distance\": %.6g, \"support_normal\": ",
				volume->supported ? "true" : "false", volume->floor_height,
				volume->support_distance) ||
			!EvoBot_NavWriteVec3(buffer, &volume->support_normal) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"water_level\": %d, \"dynamic_interactor\": %d}%s\n",
				volume->water_level, volume->dynamic_interactor,
				i + 1 < evobot_nav.volume_count ? "," : ""))
			return 0;
	}

	if (!EvoBot_NavBufferAppend(buffer, "  ],\n  \"boundaries\": [\n"))
		return 0;
	for (i = 0; i < evobot_nav.boundary_count; i++)
	{
		evobot_nav_boundary_t *boundary = &evobot_nav.boundaries[i];

		if (!EvoBot_NavBufferAppendFormat(buffer,
			"    {\"id\": %" PRIu32 ", \"volume_a\": %d, \"volume_b\": %d, \"type\": ",
			boundary->id, boundary->volume_a, boundary->volume_b) ||
			!EvoBot_NavBufferAppendJsonString(buffer, EvoBot_NavBoundaryName(boundary->kind)) ||
			!EvoBot_NavBufferAppend(buffer, ", \"bounds\": ") ||
			!EvoBot_NavWriteBounds(buffer, &boundary->bounds) ||
			!EvoBot_NavBufferAppendFormat(buffer, ", \"axis\": %d}%s\n",
				boundary->axis, i + 1 < evobot_nav.boundary_count ? "," : ""))
			return 0;
	}

	if (!EvoBot_NavBufferAppend(buffer, "  ],\n  \"interactors\": [\n"))
		return 0;
	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		evobot_nav_interactor_t *interactor = &evobot_nav.interactors[i];
		evobot_host_interactor_t *host = &interactor->host;

		if (!EvoBot_NavBufferAppendFormat(buffer, "    {\"id\": %" PRIu32 ", \"type\": ", interactor->id) ||
			!EvoBot_NavBufferAppendJsonString(buffer, EvoBot_NavInteractorName(host->kind)) ||
			!EvoBot_NavBufferAppendFormat(buffer, ", \"dynamic_brush\": %s, \"bounds\": ",
				host->dynamic_brush ? "true" : "false") ||
			!EvoBot_NavWriteBounds(buffer, &host->bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"swept_bounds\": ") ||
			!EvoBot_NavWriteBounds(buffer, &host->swept_bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"origin\": ") ||
			!EvoBot_NavWriteVec3(buffer, &host->origin) ||
			!EvoBot_NavBufferAppend(buffer, ", \"classname\": ") ||
			!EvoBot_NavBufferAppendJsonString(buffer, host->classname) ||
			!EvoBot_NavBufferAppend(buffer, ", \"model\": ") ||
			!EvoBot_NavBufferAppendJsonString(buffer, host->model) ||
			!EvoBot_NavBufferAppend(buffer, ", \"target\": ") ||
			!EvoBot_NavBufferAppendJsonString(buffer, host->target) ||
			!EvoBot_NavBufferAppend(buffer, ", \"targetname\": ") ||
			!EvoBot_NavBufferAppendJsonString(buffer, host->targetname) ||
			!EvoBot_NavBufferAppend(buffer, ", \"destination_map\": ") ||
			!EvoBot_NavBufferAppendJsonString(buffer, host->destination_map) ||
			!EvoBot_NavBufferAppendFormat(buffer, "}%s\n",
				i + 1 < evobot_nav.interactor_count ? "," : ""))
			return 0;
	}

	return EvoBot_NavBufferAppend(buffer, "  ]\n}\n");
}

static void EvoBot_NavParserWhitespace(evobot_nav_parser_t *parser)
{
	while (parser->cursor < parser->end && isspace((unsigned char)*parser->cursor))
		parser->cursor++;
}

static int EvoBot_NavParserConsume(evobot_nav_parser_t *parser, char expected)
{
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor >= parser->end || *parser->cursor != expected)
	{
		parser->failed = 1;
		return 0;
	}
	parser->cursor++;
	return 1;
}

static int EvoBot_NavParserString(evobot_nav_parser_t *parser, char *output, size_t output_size)
{
	size_t length = 0;

	if (!EvoBot_NavParserConsume(parser, '"'))
		return 0;
	while (parser->cursor < parser->end && *parser->cursor != '"')
	{
		char value = *parser->cursor++;

		if (value == '\\')
		{
			if (parser->cursor >= parser->end)
			{
				parser->failed = 1;
				return 0;
			}
			value = *parser->cursor++;
			switch (value)
			{
			case 'n': value = '\n'; break;
			case 'r': value = '\r'; break;
			case 't': value = '\t'; break;
			case '\\': value = '\\'; break;
			case '"': value = '"'; break;
			default:
				parser->failed = 1;
				return 0;
			}
		}
		if (length + 1 < output_size)
			output[length++] = value;
	}
	if (parser->cursor >= parser->end || *parser->cursor != '"')
	{
		parser->failed = 1;
		return 0;
	}
	parser->cursor++;
	if (output_size)
		output[length] = '\0';
	return 1;
}

static int EvoBot_NavParserNumber(evobot_nav_parser_t *parser, double *number)
{
	char *number_end;

	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor >= parser->end)
	{
		parser->failed = 1;
		return 0;
	}
	*number = strtod(parser->cursor, &number_end);
	if (number_end == parser->cursor || number_end > parser->end)
	{
		parser->failed = 1;
		return 0;
	}
	parser->cursor = number_end;
	return 1;
}

static int EvoBot_NavParserBool(evobot_nav_parser_t *parser, int *value)
{
	EvoBot_NavParserWhitespace(parser);
	if (parser->end - parser->cursor >= 4 && !strncmp(parser->cursor, "true", 4))
	{
		parser->cursor += 4;
		*value = 1;
		return 1;
	}
	if (parser->end - parser->cursor >= 5 && !strncmp(parser->cursor, "false", 5))
	{
		parser->cursor += 5;
		*value = 0;
		return 1;
	}
	parser->failed = 1;
	return 0;
}

static int EvoBot_NavParserSkipValue(evobot_nav_parser_t *parser);

static int EvoBot_NavParserSkipObject(evobot_nav_parser_t *parser)
{
	char key[64];

	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == '}')
	{
		parser->cursor++;
		return 1;
	}
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':') || !EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return 1;
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParserSkipArray(evobot_nav_parser_t *parser)
{
	if (!EvoBot_NavParserConsume(parser, '['))
		return 0;
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == ']')
	{
		parser->cursor++;
		return 1;
	}
	while (!parser->failed)
	{
		if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == ']')
		{
			parser->cursor++;
			return 1;
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParserSkipValue(evobot_nav_parser_t *parser)
{
	char scratch[2];
	double number;
	int boolean;

	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor >= parser->end)
		return 0;
	if (*parser->cursor == '{')
		return EvoBot_NavParserSkipObject(parser);
	if (*parser->cursor == '[')
		return EvoBot_NavParserSkipArray(parser);
	if (*parser->cursor == '"')
		return EvoBot_NavParserString(parser, scratch, sizeof(scratch));
	if (*parser->cursor == 't' || *parser->cursor == 'f')
		return EvoBot_NavParserBool(parser, &boolean);
	if (parser->end - parser->cursor >= 4 && !strncmp(parser->cursor, "null", 4))
	{
		parser->cursor += 4;
		return 1;
	}
	return EvoBot_NavParserNumber(parser, &number);
}

static int EvoBot_NavParserVec3(evobot_nav_parser_t *parser, evobot_vec3_t *vector)
{
	double value;
	int axis;

	if (!EvoBot_NavParserConsume(parser, '['))
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		if (!EvoBot_NavParserNumber(parser, &value))
			return 0;
		vector->v[axis] = (float)value;
		if (axis < 2 && !EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParserBounds(evobot_nav_parser_t *parser, evobot_bounds_t *bounds)
{
	char key[64];

	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':'))
			return 0;
		if (!strcmp(key, "mins"))
		{
			if (!EvoBot_NavParserVec3(parser, &bounds->mins))
				return 0;
		}
		else if (!strcmp(key, "maxs"))
		{
			if (!EvoBot_NavParserVec3(parser, &bounds->maxs))
				return 0;
		}
		else if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return 1;
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavDatasetAddVolume(evobot_nav_dataset_t *dataset,
	const evobot_nav_volume_t *volume)
{
	if (!EvoBot_NavReserve((void **)&dataset->volumes, &dataset->volume_capacity,
		dataset->volume_count + 1, sizeof(*dataset->volumes)))
		return 0;
	dataset->volumes[dataset->volume_count++] = *volume;
	return 1;
}

static int EvoBot_NavDatasetAddBoundary(evobot_nav_dataset_t *dataset,
	const evobot_nav_boundary_t *boundary)
{
	if (!EvoBot_NavReserve((void **)&dataset->boundaries, &dataset->boundary_capacity,
		dataset->boundary_count + 1, sizeof(*dataset->boundaries)))
		return 0;
	dataset->boundaries[dataset->boundary_count++] = *boundary;
	return 1;
}

static int EvoBot_NavDatasetAddInteractor(evobot_nav_dataset_t *dataset,
	const evobot_nav_interactor_t *interactor)
{
	if (!EvoBot_NavReserve((void **)&dataset->interactors, &dataset->interactor_capacity,
		dataset->interactor_count + 1, sizeof(*dataset->interactors)))
		return 0;
	dataset->interactors[dataset->interactor_count++] = *interactor;
	return 1;
}

static int EvoBot_NavParseMap(evobot_nav_parser_t *parser, evobot_nav_dataset_t *dataset)
{
	char key[64];
	double number;

	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':'))
			return 0;
		if (!strcmp(key, "name"))
		{
			if (!EvoBot_NavParserString(parser, dataset->map_name, sizeof(dataset->map_name)))
				return 0;
		}
		else if (!strcmp(key, "checksum"))
		{
			if (!EvoBot_NavParserNumber(parser, &number))
				return 0;
			dataset->map_checksum = (uint32_t)number;
		}
		else if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return 1;
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParseGeneration(evobot_nav_parser_t *parser, evobot_nav_dataset_t *dataset)
{
	char key[64];
	double number;

	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':'))
			return 0;
		if (!strcmp(key, "world_bounds"))
		{
			if (!EvoBot_NavParserBounds(parser, &dataset->world_bounds))
				return 0;
		}
		else if (!strcmp(key, "player_hull"))
		{
			if (!EvoBot_NavParserBounds(parser, &dataset->player_bounds))
				return 0;
		}
		else if (!strcmp(key, "temporary_samples") ||
			!strcmp(key, "accepted_cells") || !strcmp(key, "rejected_cells") ||
			!strcmp(key, "generation_time"))
		{
			if (!EvoBot_NavParserNumber(parser, &number))
				return 0;
			if (!strcmp(key, "temporary_samples"))
				dataset->stats.sample_points = (uint64_t)number;
			else if (!strcmp(key, "accepted_cells"))
				dataset->stats.accepted_cells = (uint64_t)number;
			else if (!strcmp(key, "rejected_cells"))
				dataset->stats.rejected_cells = (uint64_t)number;
			else
				dataset->stats.generation_time = number;
		}
		else if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return 1;
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParseVolume(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	evobot_nav_volume_t volume;
	char key[64];
	char text[64];
	double number;

	memset(&volume, 0, sizeof(volume));
	volume.dynamic_interactor = EVOBOT_NAV_INVALID_ID;
	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':'))
			return 0;
		if (!strcmp(key, "bounds"))
		{
			if (!EvoBot_NavParserBounds(parser, &volume.bounds))
				return 0;
		}
		else if (!strcmp(key, "contents"))
		{
			if (!EvoBot_NavParserString(parser, text, sizeof(text)))
				return 0;
			volume.contents = EvoBot_NavContentsFromName(text);
		}
		else if (!strcmp(key, "supported"))
		{
			if (!EvoBot_NavParserBool(parser, &volume.supported))
				return 0;
		}
		else if (!strcmp(key, "support_normal"))
		{
			if (!EvoBot_NavParserVec3(parser, &volume.support_normal))
				return 0;
		}
		else if (!strcmp(key, "id") || !strcmp(key, "floor_height") ||
			!strcmp(key, "support_distance") || !strcmp(key, "water_level") ||
			!strcmp(key, "dynamic_interactor"))
		{
			if (!EvoBot_NavParserNumber(parser, &number))
				return 0;
			if (!strcmp(key, "id")) volume.id = (uint32_t)number;
			else if (!strcmp(key, "floor_height")) volume.floor_height = (float)number;
			else if (!strcmp(key, "support_distance")) volume.support_distance = (float)number;
			else if (!strcmp(key, "water_level")) volume.water_level = (int)number;
			else volume.dynamic_interactor = (int)number;
		}
		else if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return EvoBot_NavDatasetAddVolume(dataset, &volume);
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParseBoundary(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	evobot_nav_boundary_t boundary;
	char key[64];
	char text[64];
	double number;

	memset(&boundary, 0, sizeof(boundary));
	boundary.volume_b = EVOBOT_NAV_INVALID_ID;
	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':'))
			return 0;
		if (!strcmp(key, "bounds"))
		{
			if (!EvoBot_NavParserBounds(parser, &boundary.bounds))
				return 0;
		}
		else if (!strcmp(key, "type"))
		{
			if (!EvoBot_NavParserString(parser, text, sizeof(text)))
				return 0;
			boundary.kind = EvoBot_NavBoundaryFromName(text);
		}
		else if (!strcmp(key, "id") || !strcmp(key, "volume_a") ||
			!strcmp(key, "volume_b") || !strcmp(key, "axis"))
		{
			if (!EvoBot_NavParserNumber(parser, &number))
				return 0;
			if (!strcmp(key, "id")) boundary.id = (uint32_t)number;
			else if (!strcmp(key, "volume_a")) boundary.volume_a = (int)number;
			else if (!strcmp(key, "volume_b")) boundary.volume_b = (int)number;
			else boundary.axis = (int)number;
		}
		else if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return EvoBot_NavDatasetAddBoundary(dataset, &boundary);
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParseInteractor(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	evobot_nav_interactor_t interactor;
	char key[64];
	char text[64];
	double number;

	memset(&interactor, 0, sizeof(interactor));
	if (!EvoBot_NavParserConsume(parser, '{'))
		return 0;
	while (!parser->failed)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':'))
			return 0;
		if (!strcmp(key, "bounds"))
		{
			if (!EvoBot_NavParserBounds(parser, &interactor.host.bounds)) return 0;
		}
		else if (!strcmp(key, "swept_bounds"))
		{
			if (!EvoBot_NavParserBounds(parser, &interactor.host.swept_bounds)) return 0;
		}
		else if (!strcmp(key, "origin"))
		{
			if (!EvoBot_NavParserVec3(parser, &interactor.host.origin)) return 0;
		}
		else if (!strcmp(key, "type"))
		{
			if (!EvoBot_NavParserString(parser, text, sizeof(text))) return 0;
			interactor.host.kind = EvoBot_NavInteractorFromName(text);
		}
		else if (!strcmp(key, "dynamic_brush"))
		{
			if (!EvoBot_NavParserBool(parser, &interactor.host.dynamic_brush)) return 0;
		}
		else if (!strcmp(key, "classname"))
		{
			if (!EvoBot_NavParserString(parser, interactor.host.classname, sizeof(interactor.host.classname))) return 0;
		}
		else if (!strcmp(key, "model"))
		{
			if (!EvoBot_NavParserString(parser, interactor.host.model, sizeof(interactor.host.model))) return 0;
		}
		else if (!strcmp(key, "target"))
		{
			if (!EvoBot_NavParserString(parser, interactor.host.target, sizeof(interactor.host.target))) return 0;
		}
		else if (!strcmp(key, "targetname"))
		{
			if (!EvoBot_NavParserString(parser, interactor.host.targetname, sizeof(interactor.host.targetname))) return 0;
		}
		else if (!strcmp(key, "destination_map"))
		{
			if (!EvoBot_NavParserString(parser, interactor.host.destination_map, sizeof(interactor.host.destination_map))) return 0;
		}
		else if (!strcmp(key, "id"))
		{
			if (!EvoBot_NavParserNumber(parser, &number)) return 0;
			interactor.id = (uint32_t)number;
		}
		else if (!EvoBot_NavParserSkipValue(parser))
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return EvoBot_NavDatasetAddInteractor(dataset, &interactor);
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParseArray(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset, int kind)
{
	if (!EvoBot_NavParserConsume(parser, '['))
		return 0;
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == ']')
	{
		parser->cursor++;
		return 1;
	}
	while (!parser->failed)
	{
		int result;

		if (kind == 0) result = EvoBot_NavParseVolume(parser, dataset);
		else if (kind == 1) result = EvoBot_NavParseBoundary(parser, dataset);
		else result = EvoBot_NavParseInteractor(parser, dataset);
		if (!result)
			return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == ']')
		{
			parser->cursor++;
			return 1;
		}
		if (!EvoBot_NavParserConsume(parser, ','))
			return 0;
	}
	return 0;
}

static int EvoBot_NavParseJson(const char *data, size_t size, evobot_nav_dataset_t *dataset)
{
	evobot_nav_parser_t parser;
	char key[64];
	char format[64] = "";
	double version = 0;

	memset(&parser, 0, sizeof(parser));
	parser.cursor = data;
	parser.end = data + size;
	if (!EvoBot_NavParserConsume(&parser, '{'))
		return 0;
	while (!parser.failed)
	{
		if (!EvoBot_NavParserString(&parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(&parser, ':'))
			return 0;
		if (!strcmp(key, "format"))
		{
			if (!EvoBot_NavParserString(&parser, format, sizeof(format))) return 0;
		}
		else if (!strcmp(key, "version"))
		{
			if (!EvoBot_NavParserNumber(&parser, &version)) return 0;
		}
		else if (!strcmp(key, "map"))
		{
			if (!EvoBot_NavParseMap(&parser, dataset)) return 0;
		}
		else if (!strcmp(key, "generation"))
		{
			if (!EvoBot_NavParseGeneration(&parser, dataset)) return 0;
		}
		else if (!strcmp(key, "volumes"))
		{
			if (!EvoBot_NavParseArray(&parser, dataset, 0)) return 0;
		}
		else if (!strcmp(key, "boundaries"))
		{
			if (!EvoBot_NavParseArray(&parser, dataset, 1)) return 0;
		}
		else if (!strcmp(key, "interactors"))
		{
			if (!EvoBot_NavParseArray(&parser, dataset, 2)) return 0;
		}
		else if (!EvoBot_NavParserSkipValue(&parser))
			return 0;
		EvoBot_NavParserWhitespace(&parser);
		if (parser.cursor < parser.end && *parser.cursor == '}')
		{
			parser.cursor++;
			break;
		}
		if (!EvoBot_NavParserConsume(&parser, ','))
			return 0;
	}

	EvoBot_NavParserWhitespace(&parser);
	if (parser.failed || parser.cursor != parser.end || strcmp(format, "evobot-nav") ||
		(int)version != EVOBOT_NAV_FORMAT_VERSION || !dataset->map_name[0])
		return 0;
	return 1;
}

void EvoBot_NavSave(void)
{
	evobot_nav_buffer_t buffer;
	char path[256];

	if (!EvoBot_NavMapReady())
		return;
	if (evobot_nav.state == EVOBOT_NAV_STATE_EMPTY || !evobot_nav.volume_count)
	{
		EvoBot_NavPrint("EvoBot navigation: no generated or loaded data to save\n");
		return;
	}
	if (strcmp(evobot_nav.map_name, evobot_nav_active_map) ||
		evobot_nav.map_checksum != evobot_nav_active_checksum)
	{
		EvoBot_NavPrint("EvoBot navigation: dataset does not match the active map\n");
		return;
	}

	memset(&buffer, 0, sizeof(buffer));
	if (!EvoBot_NavSerializeJson(&buffer) || buffer.failed || !evobot_nav_host.write_file)
	{
		free(buffer.data);
		EvoBot_NavPrint("EvoBot navigation: could not serialize navigation data\n");
		return;
	}
	EvoBot_NavPath(path, sizeof(path), evobot_nav.map_name, "botnav", 0);
	if (!evobot_nav_host.write_file(path, buffer.data, buffer.length))
	{
		free(buffer.data);
		EvoBot_NavPrintf("EvoBot navigation: could not write %s\n", path);
		return;
	}
	EvoBot_NavPrintf("EvoBot navigation saved: %s (%u bytes)\n", path, (unsigned)buffer.length);
	free(buffer.data);
}

void EvoBot_NavLoad(const char *source_map)
{
	char path[256];
	char *data;
	size_t size;
	evobot_nav_dataset_t loaded;
	const char *file_map;

	if (!EvoBot_NavMapReady())
		return;
	file_map = source_map && source_map[0] ? source_map : evobot_nav_active_map;
	if (!EvoBot_NavSafeMapName(file_map))
	{
		EvoBot_NavPrint("EvoBot navigation: invalid source map name\n");
		return;
	}
	if (!evobot_nav_host.file_size || !evobot_nav_host.read_file)
	{
		EvoBot_NavPrint("EvoBot navigation: file access is unavailable\n");
		return;
	}

	EvoBot_NavPath(path, sizeof(path), file_map, "botnav", 0);
	if (!evobot_nav_host.file_size(path, &size))
	{
		EvoBot_NavPrintf("EvoBot navigation: file not found: %s\n", path);
		return;
	}
	if (!size || size > EVOBOT_NAV_MAX_FILE_SIZE)
	{
		EvoBot_NavPrint("EvoBot navigation: invalid navigation file size\n");
		return;
	}
	data = (char *)malloc(size + 1);
	if (!data || !evobot_nav_host.read_file(path, data, size))
	{
		free(data);
		EvoBot_NavPrintf("EvoBot navigation: could not read %s\n", path);
		return;
	}
	data[size] = '\0';
	memset(&loaded, 0, sizeof(loaded));
	if (!EvoBot_NavParseJson(data, size, &loaded))
	{
		free(data);
		EvoBot_NavFreeDataset(&loaded);
		EvoBot_NavPrintf("EvoBot navigation: invalid file: %s\n", path);
		return;
	}
	free(data);

	if (strcmp(loaded.map_name, evobot_nav_active_map) ||
		loaded.map_checksum != evobot_nav_active_checksum)
	{
		EvoBot_NavPrintf("EvoBot navigation: map mismatch (file %s/%" PRIu32
			", active %s/%" PRIu32 ")\n", loaded.map_name, loaded.map_checksum,
			evobot_nav_active_map, evobot_nav_active_checksum);
		EvoBot_NavFreeDataset(&loaded);
		return;
	}

	loaded.state = EVOBOT_NAV_STATE_LOADED;
	EvoBot_NavFreeDataset(&evobot_nav);
	evobot_nav = loaded;
	EvoBot_NavPrintf("EvoBot navigation loaded: %s\n", path);
}

static int EvoBot_NavObjCube(evobot_nav_buffer_t *buffer, const evobot_bounds_t *bounds,
	uint32_t *vertex_index)
{
	uint32_t first = *vertex_index + 1;
	int x;
	int y;
	int z;

	for (z = 0; z < 2; z++)
	{
		for (y = 0; y < 2; y++)
		{
			for (x = 0; x < 2; x++)
			{
				if (!EvoBot_NavBufferAppendFormat(buffer, "v %.6g %.6g %.6g\n",
					x ? bounds->maxs.v[0] : bounds->mins.v[0],
					y ? bounds->maxs.v[1] : bounds->mins.v[1],
					z ? bounds->maxs.v[2] : bounds->mins.v[2]))
					return 0;
			}
		}
	}
	*vertex_index += 8;
	return EvoBot_NavBufferAppendFormat(buffer,
		"f %u %u %u %u\nf %u %u %u %u\nf %u %u %u %u\n"
		"f %u %u %u %u\nf %u %u %u %u\nf %u %u %u %u\n",
		first, first + 1, first + 3, first + 2,
		first + 4, first + 6, first + 7, first + 5,
		first, first + 4, first + 5, first + 1,
		first + 2, first + 3, first + 7, first + 6,
		first, first + 2, first + 6, first + 4,
		first + 1, first + 5, first + 7, first + 3);
}

static int EvoBot_NavObjBoundary(evobot_nav_buffer_t *buffer,
	const evobot_nav_boundary_t *boundary, uint32_t *vertex_index)
{
	int a = (boundary->axis + 1) % 3;
	int b = (boundary->axis + 2) % 3;
	evobot_vec3_t vertices[4];
	uint32_t first = *vertex_index + 1;
	int i;

	for (i = 0; i < 4; i++)
	{
		vertices[i].v[boundary->axis] = boundary->bounds.mins.v[boundary->axis];
		vertices[i].v[a] = (i & 1) ? boundary->bounds.maxs.v[a] : boundary->bounds.mins.v[a];
		vertices[i].v[b] = (i & 2) ? boundary->bounds.maxs.v[b] : boundary->bounds.mins.v[b];
		if (!EvoBot_NavBufferAppendFormat(buffer, "v %.6g %.6g %.6g\n",
			vertices[i].v[0], vertices[i].v[1], vertices[i].v[2]))
			return 0;
	}
	*vertex_index += 4;
	return EvoBot_NavBufferAppendFormat(buffer, "l %u %u %u %u %u\n",
		first, first + 1, first + 3, first + 2, first);
}

void EvoBot_NavExportObj(void)
{
	evobot_nav_buffer_t buffer;
	char path[256];
	uint32_t vertex_index = 0;
	int group;
	size_t i;

	if (!EvoBot_NavMapReady())
		return;
	if (evobot_nav.state == EVOBOT_NAV_STATE_EMPTY || !evobot_nav.volume_count)
	{
		EvoBot_NavPrint("EvoBot navigation: no generated or loaded data to export\n");
		return;
	}

	memset(&buffer, 0, sizeof(buffer));
	EvoBot_NavBufferAppend(&buffer, "# EvoBot navigation volume export\n");
	for (group = 0; group < 5; group++)
	{
		const char *group_name = group == 0 ? "supported_air" :
			group == 1 ? "unsupported_air" : group == 2 ? "water" :
			group == 3 ? "slime" : "lava";
		EvoBot_NavBufferAppendFormat(&buffer, "g %s\n", group_name);
		for (i = 0; i < evobot_nav.volume_count; i++)
		{
			evobot_nav_volume_t *volume = &evobot_nav.volumes[i];
			int volume_group;

			if (volume->contents == EVOBOT_CONTENTS_WATER) volume_group = 2;
			else if (volume->contents == EVOBOT_CONTENTS_SLIME) volume_group = 3;
			else if (volume->contents == EVOBOT_CONTENTS_LAVA) volume_group = 4;
			else volume_group = volume->supported ? 0 : 1;
			if (volume_group == group && !EvoBot_NavObjCube(&buffer, &volume->bounds, &vertex_index))
				break;
		}
	}
	for (group = 0; group < 3; group++)
	{
		const char *group_name = group == 0 ? "boundaries_ordinary" :
			group == 1 ? "boundaries_ledge" : "boundaries_liquid";
		EvoBot_NavBufferAppendFormat(&buffer, "g %s\n", group_name);
		for (i = 0; i < evobot_nav.boundary_count; i++)
		{
			if ((int)evobot_nav.boundaries[i].kind == group &&
				!EvoBot_NavObjBoundary(&buffer, &evobot_nav.boundaries[i], &vertex_index))
				break;
		}
	}

	if (buffer.failed || !evobot_nav_host.write_file)
	{
		free(buffer.data);
		EvoBot_NavPrint("EvoBot navigation: could not build OBJ export\n");
		return;
	}
	EvoBot_NavPath(path, sizeof(path), evobot_nav.map_name, "obj", 1);
	if (!evobot_nav_host.write_file(path, buffer.data, buffer.length))
	{
		free(buffer.data);
		EvoBot_NavPrintf("EvoBot navigation: could not write %s\n", path);
		return;
	}
	EvoBot_NavPrintf("EvoBot navigation OBJ exported: %s (%u bytes)\n",
		path, (unsigned)buffer.length);
	free(buffer.data);
}
