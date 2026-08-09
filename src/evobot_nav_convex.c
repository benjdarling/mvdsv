#include "evobot/evobot_nav_convex.h"
#include "evobot/evobot_nav_debug.h"
#include "evobot/evobot_nav_reach.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVOBOT_NAV_FORMAT_VERSION 6
#define EVOBOT_NAV_MAX_FILE_SIZE (128u * 1024u * 1024u)
#define EVOBOT_NAV_MAX_PLANES 256
#define EVOBOT_NAV_PLANE_EPSILON 0.05f
#define EVOBOT_NAV_POINT_EPSILON 0.1f
#define EVOBOT_NAV_WALKABLE_NORMAL_Z 0.7f
#define EVOBOT_NAV_SUPPORT_PROBE 256.0f
#define EVOBOT_NAV_STEP_SIZE 18.0f
#define EVOBOT_NAV_WALK_EDGE_MIN (EVOBOT_NAV_POINT_EPSILON * 2.0f)
#define EVOBOT_NAV_WALK_EDGE_INSET 1.0f
#define EVOBOT_NAV_WALK_CROSSING_OFFSET 4.0f
#define EVOBOT_NAV_WALK_CROSSING_RETRY_OFFSET 1.0f
#define EVOBOT_NAV_WALK_REACH_OFFSET 1.0f
#define EVOBOT_NAV_WALK_STEP_EPSILON 0.25f
#define EVOBOT_NAV_WALK_SAMPLE_COUNT 3
#define EVOBOT_NAV_DEGENERATE_SPLIT_EPSILON 1.0f
#define EVOBOT_NAV_INVALID_ID (-1)
#define EVOBOT_NAV_MULTIPLE_DYNAMIC (-2)

typedef enum evobot_nav_state_e
{
	EVOBOT_NAV_STATE_EMPTY,
	EVOBOT_NAV_STATE_GENERATED,
	EVOBOT_NAV_STATE_LOADED
} evobot_nav_state_t;

typedef enum evobot_nav_face_kind_e
{
	EVOBOT_NAV_FACE_SOLID,
	EVOBOT_NAV_FACE_PORTAL,
	EVOBOT_NAV_FACE_LIQUID,
	EVOBOT_NAV_FACE_LEDGE
} evobot_nav_face_kind_t;

typedef struct evobot_nav_plane_s
{
	evobot_vec3_t normal;
	float distance;
} evobot_nav_plane_t;

typedef struct evobot_nav_face_s
{
	int plane;
	evobot_nav_face_kind_t kind;
	int portal;
	evobot_vec3_t *vertices;
	size_t vertex_count;
} evobot_nav_face_t;

typedef struct evobot_nav_area_s
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
	evobot_nav_plane_t *planes;
	size_t plane_count;
	evobot_nav_face_t *faces;
	size_t face_count;
} evobot_nav_area_t;

typedef struct evobot_nav_portal_s
{
	uint32_t id;
	uint32_t area_a;
	uint32_t area_b;
	evobot_nav_face_kind_t kind;
	evobot_nav_plane_t plane;
	evobot_vec3_t *vertices;
	size_t vertex_count;
} evobot_nav_portal_t;

typedef struct evobot_nav_interactor_s
{
	uint32_t id;
	evobot_host_interactor_t host;
} evobot_nav_interactor_t;

typedef struct evobot_nav_stats_s
{
	uint64_t collision_nodes;
	uint64_t initial_areas;
	uint64_t content_splits;
	uint64_t gravity_splits;
	uint64_t merges;
	uint64_t validation_traces;
	uint64_t movement_tests;
	uint64_t teleporter_triggers;
	uint64_t resolved_teleporters;
	uint64_t teleport_links;
	uint64_t edge_drop_seeds;
	uint64_t edge_drop_links;
	uint64_t gap_jump_seeds;
	uint64_t gap_jump_links;
	uint64_t gap_jump_rejected_start;
	uint64_t gap_jump_rejected_air;
	uint64_t gap_jump_rejected_landing;
	uint64_t gap_jump_rejected_edge;
	uint64_t jump_up_ledge_candidates;
	uint64_t jump_up_ledge_validated;
	uint64_t jump_up_ledge_links;
	uint64_t jump_up_ledge_attempts;
	uint64_t water_jump_candidates;
	uint64_t water_jump_validated;
	uint64_t water_jump_links;
	uint64_t water_jump_attempts;
	uint64_t platform_movers;
	uint64_t platform_links;
	uint64_t activation_relationships;
	double generation_time;
	double reachability_time;
	double teleport_time;
	double edge_drop_time;
	double gap_jump_time;
	double jump_up_ledge_time;
	double water_jump_time;
	double platform_time;
	double activation_graph_time;
	double dynamic_analysis_time;
} evobot_nav_stats_t;

typedef struct evobot_nav_dataset_s
{
	evobot_nav_state_t state;
	char map_name[EVOBOT_NAV_MAP_MAX];
	uint32_t map_checksum;
	evobot_bounds_t world_bounds;
	evobot_bounds_t player_bounds;
	evobot_nav_area_t *areas;
	size_t area_count;
	size_t area_capacity;
	evobot_nav_portal_t *portals;
	size_t portal_count;
	size_t portal_capacity;
	evobot_nav_interactor_t *interactors;
	size_t interactor_count;
	size_t interactor_capacity;
	evobot_nav_reachability_t *reachabilities;
	size_t reachability_count;
	size_t reachability_capacity;
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

static evobot_host_api_t evobot_nav_host;
static evobot_nav_dataset_t evobot_nav;
static int evobot_nav_initialized;
static int evobot_nav_map_loaded;
static char evobot_nav_active_map[EVOBOT_NAV_MAP_MAX];
static uint32_t evobot_nav_active_checksum;
static uint64_t evobot_nav_revision;
static evobot_vec3_t evobot_nav_sort_center;
static evobot_vec3_t evobot_nav_sort_u;
static evobot_vec3_t evobot_nav_sort_v;

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

static void EvoBot_NavTopologyChanged(void)
{
	evobot_nav_revision++;
	if (!evobot_nav_revision)
		evobot_nav_revision++;
}

static float EvoBot_NavDot(const evobot_vec3_t *a, const evobot_vec3_t *b)
{
	return a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2];
}

static void EvoBot_NavCross(const evobot_vec3_t *a, const evobot_vec3_t *b,
	evobot_vec3_t *result)
{
	result->v[0] = a->v[1] * b->v[2] - a->v[2] * b->v[1];
	result->v[1] = a->v[2] * b->v[0] - a->v[0] * b->v[2];
	result->v[2] = a->v[0] * b->v[1] - a->v[1] * b->v[0];
}

static void EvoBot_NavSubtract(const evobot_vec3_t *a, const evobot_vec3_t *b,
	evobot_vec3_t *result)
{
	result->v[0] = a->v[0] - b->v[0];
	result->v[1] = a->v[1] - b->v[1];
	result->v[2] = a->v[2] - b->v[2];
}

static float EvoBot_NavLength(const evobot_vec3_t *vector)
{
	return sqrtf(EvoBot_NavDot(vector, vector));
}

static int EvoBot_NavNormalize(evobot_vec3_t *vector)
{
	float length = EvoBot_NavLength(vector);

	if (length < 0.00001f)
		return 0;
	vector->v[0] /= length;
	vector->v[1] /= length;
	vector->v[2] /= length;
	return 1;
}

static int EvoBot_NavReserve(void **items, size_t *capacity, size_t count,
	size_t item_size)
{
	void *memory;
	size_t new_capacity;

	if (count <= *capacity)
		return 1;
	new_capacity = *capacity ? *capacity * 2 : 64;
	while (new_capacity < count)
		new_capacity *= 2;
	memory = realloc(*items, new_capacity * item_size);
	if (!memory)
		return 0;
	*items = memory;
	*capacity = new_capacity;
	return 1;
}

static void EvoBot_NavFreeFaces(evobot_nav_area_t *area)
{
	size_t i;

	for (i = 0; i < area->face_count; i++)
		free(area->faces[i].vertices);
	free(area->faces);
	area->faces = NULL;
	area->face_count = 0;
}

static void EvoBot_NavFreeArea(evobot_nav_area_t *area)
{
	EvoBot_NavFreeFaces(area);
	free(area->planes);
	memset(area, 0, sizeof(*area));
}

static void EvoBot_NavFreeDataset(evobot_nav_dataset_t *dataset)
{
	size_t i;

	for (i = 0; i < dataset->area_count; i++)
		EvoBot_NavFreeArea(&dataset->areas[i]);
	for (i = 0; i < dataset->portal_count; i++)
		free(dataset->portals[i].vertices);
	free(dataset->areas);
	free(dataset->portals);
	free(dataset->interactors);
	free(dataset->reachabilities);
	memset(dataset, 0, sizeof(*dataset));
}

static int EvoBot_NavPlaneEqual(const evobot_nav_plane_t *a,
	const evobot_nav_plane_t *b)
{
	return fabsf(a->normal.v[0] - b->normal.v[0]) <= 0.0001f &&
		fabsf(a->normal.v[1] - b->normal.v[1]) <= 0.0001f &&
		fabsf(a->normal.v[2] - b->normal.v[2]) <= 0.0001f &&
		fabsf(a->distance - b->distance) <= EVOBOT_NAV_PLANE_EPSILON;
}

static evobot_nav_plane_t EvoBot_NavReversePlane(const evobot_nav_plane_t *plane)
{
	evobot_nav_plane_t result = *plane;

	result.normal.v[0] = -result.normal.v[0];
	result.normal.v[1] = -result.normal.v[1];
	result.normal.v[2] = -result.normal.v[2];
	result.distance = -result.distance;
	return result;
}

static float EvoBot_NavPlaneDistance(const evobot_nav_plane_t *plane,
	const evobot_vec3_t *point)
{
	return EvoBot_NavDot(&plane->normal, point) - plane->distance;
}

static int EvoBot_NavPointInside(const evobot_nav_area_t *area,
	const evobot_vec3_t *point, float epsilon)
{
	size_t i;

	for (i = 0; i < area->plane_count; i++)
	{
		if (EvoBot_NavPlaneDistance(&area->planes[i], point) > epsilon)
			return 0;
	}
	return 1;
}

static int EvoBot_NavIntersectPlanes(const evobot_nav_plane_t *a,
	const evobot_nav_plane_t *b, const evobot_nav_plane_t *c, evobot_vec3_t *point)
{
	evobot_vec3_t bc;
	evobot_vec3_t ca;
	evobot_vec3_t ab;
	float determinant;
	int axis;

	EvoBot_NavCross(&b->normal, &c->normal, &bc);
	determinant = EvoBot_NavDot(&a->normal, &bc);
	if (fabsf(determinant) < 0.00001f)
		return 0;
	EvoBot_NavCross(&c->normal, &a->normal, &ca);
	EvoBot_NavCross(&a->normal, &b->normal, &ab);
	for (axis = 0; axis < 3; axis++)
	{
		point->v[axis] = (a->distance * bc.v[axis] +
			b->distance * ca.v[axis] + c->distance * ab.v[axis]) / determinant;
	}
	return 1;
}

static int EvoBot_NavSamePoint(const evobot_vec3_t *a, const evobot_vec3_t *b)
{
	evobot_vec3_t delta;

	EvoBot_NavSubtract(a, b, &delta);
	return EvoBot_NavLength(&delta) <= EVOBOT_NAV_POINT_EPSILON;
}

static int EvoBot_NavVertexCompare(const void *left, const void *right)
{
	const evobot_vec3_t *a = left;
	const evobot_vec3_t *b = right;
	evobot_vec3_t da;
	evobot_vec3_t db;
	float angle_a;
	float angle_b;

	EvoBot_NavSubtract(a, &evobot_nav_sort_center, &da);
	EvoBot_NavSubtract(b, &evobot_nav_sort_center, &db);
	angle_a = atan2f(EvoBot_NavDot(&da, &evobot_nav_sort_v),
		EvoBot_NavDot(&da, &evobot_nav_sort_u));
	angle_b = atan2f(EvoBot_NavDot(&db, &evobot_nav_sort_v),
		EvoBot_NavDot(&db, &evobot_nav_sort_u));
	return angle_a < angle_b ? -1 : angle_a > angle_b;
}

static int EvoBot_NavBuildFaces(evobot_nav_area_t *area)
{
	evobot_vec3_t *vertices = NULL;
	size_t vertex_count = 0;
	size_t vertex_capacity = 0;
	size_t i;
	size_t j;
	size_t k;
	int axis;

	EvoBot_NavFreeFaces(area);
	for (i = 0; i < area->plane_count; i++)
	{
		for (j = i + 1; j < area->plane_count; j++)
		{
			for (k = j + 1; k < area->plane_count; k++)
			{
				evobot_vec3_t point;
				size_t existing;

				if (!EvoBot_NavIntersectPlanes(&area->planes[i], &area->planes[j],
					&area->planes[k], &point) ||
					!EvoBot_NavPointInside(area, &point, EVOBOT_NAV_PLANE_EPSILON))
					continue;
				for (existing = 0; existing < vertex_count; existing++)
				{
					if (EvoBot_NavSamePoint(&vertices[existing], &point))
						break;
				}
				if (existing != vertex_count)
					continue;
				if (!EvoBot_NavReserve((void **)&vertices, &vertex_capacity,
					vertex_count + 1, sizeof(*vertices)))
				{
					free(vertices);
					return 0;
				}
				vertices[vertex_count++] = point;
			}
		}
	}
	if (vertex_count < 4)
	{
		free(vertices);
		return 0;
	}

	for (axis = 0; axis < 3; axis++)
	{
		area->bounds.mins.v[axis] = vertices[0].v[axis];
		area->bounds.maxs.v[axis] = vertices[0].v[axis];
	}
	for (i = 1; i < vertex_count; i++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			if (vertices[i].v[axis] < area->bounds.mins.v[axis])
				area->bounds.mins.v[axis] = vertices[i].v[axis];
			if (vertices[i].v[axis] > area->bounds.maxs.v[axis])
				area->bounds.maxs.v[axis] = vertices[i].v[axis];
		}
	}

	for (i = 0; i < area->plane_count; i++)
	{
		evobot_nav_face_t face;
		size_t face_capacity = 0;
		evobot_vec3_t reference;

		memset(&face, 0, sizeof(face));
		face.plane = (int)i;
		face.kind = EVOBOT_NAV_FACE_SOLID;
		face.portal = EVOBOT_NAV_INVALID_ID;
		for (j = 0; j < vertex_count; j++)
		{
			if (fabsf(EvoBot_NavPlaneDistance(&area->planes[i], &vertices[j])) >
				EVOBOT_NAV_PLANE_EPSILON)
				continue;
			if (!EvoBot_NavReserve((void **)&face.vertices, &face_capacity,
				face.vertex_count + 1, sizeof(*face.vertices)))
			{
				free(face.vertices);
				free(vertices);
				return 0;
			}
			face.vertices[face.vertex_count++] = vertices[j];
		}
		if (face.vertex_count < 3)
		{
			free(face.vertices);
			continue;
		}
		memset(&evobot_nav_sort_center, 0, sizeof(evobot_nav_sort_center));
		for (j = 0; j < face.vertex_count; j++)
		{
			for (axis = 0; axis < 3; axis++)
				evobot_nav_sort_center.v[axis] += face.vertices[j].v[axis];
		}
		for (axis = 0; axis < 3; axis++)
			evobot_nav_sort_center.v[axis] /= (float)face.vertex_count;
		if (fabsf(area->planes[i].normal.v[2]) < 0.9f)
		{
			reference.v[0] = 0;
			reference.v[1] = 0;
			reference.v[2] = 1;
		}
		else
		{
			reference.v[0] = 1;
			reference.v[1] = 0;
			reference.v[2] = 0;
		}
		EvoBot_NavCross(&reference, &area->planes[i].normal, &evobot_nav_sort_u);
		EvoBot_NavNormalize(&evobot_nav_sort_u);
		EvoBot_NavCross(&area->planes[i].normal, &evobot_nav_sort_u,
			&evobot_nav_sort_v);
		qsort(face.vertices, face.vertex_count, sizeof(*face.vertices),
			EvoBot_NavVertexCompare);
		{
			evobot_nav_face_t *faces = realloc(area->faces,
				(area->face_count + 1) * sizeof(*area->faces));

			if (!faces)
			{
				free(face.vertices);
				free(vertices);
				return 0;
			}
			area->faces = faces;
		}
		area->faces[area->face_count++] = face;
	}
	free(vertices);
	return area->face_count >= 4;
}

static void EvoBot_NavAreaCenter(const evobot_nav_area_t *area, evobot_vec3_t *center)
{
	size_t i;
	size_t count = 0;
	int axis;

	memset(center, 0, sizeof(*center));
	for (i = 0; i < area->face_count; i++)
	{
		size_t j;
		for (j = 0; j < area->faces[i].vertex_count; j++)
		{
			for (axis = 0; axis < 3; axis++)
				center->v[axis] += area->faces[i].vertices[j].v[axis];
			count++;
		}
	}
	if (!count)
		return;
	for (axis = 0; axis < 3; axis++)
		center->v[axis] /= (float)count;
}

static int EvoBot_NavCopyArea(const evobot_nav_area_t *source,
	evobot_nav_area_t *destination)
{
	memset(destination, 0, sizeof(*destination));
	*destination = *source;
	destination->planes = NULL;
	destination->faces = NULL;
	destination->face_count = 0;
	if (source->plane_count)
	{
		destination->planes = malloc(source->plane_count * sizeof(*destination->planes));
		if (!destination->planes)
			return 0;
		memcpy(destination->planes, source->planes,
			source->plane_count * sizeof(*destination->planes));
	}
	return EvoBot_NavBuildFaces(destination);
}

static int EvoBot_NavAddPlane(evobot_nav_area_t *area,
	const evobot_nav_plane_t *plane)
{
	evobot_nav_plane_t *planes;
	size_t i;

	for (i = 0; i < area->plane_count; i++)
	{
		if (EvoBot_NavPlaneEqual(&area->planes[i], plane))
			return 1;
	}
	if (area->plane_count >= EVOBOT_NAV_MAX_PLANES)
		return 0;
	planes = realloc(area->planes, (area->plane_count + 1) * sizeof(*planes));
	if (!planes)
		return 0;
	area->planes = planes;
	area->planes[area->plane_count++] = *plane;
	return 1;
}

static int EvoBot_NavAppendArea(evobot_nav_dataset_t *dataset,
	evobot_nav_area_t *area)
{
	if (!EvoBot_NavReserve((void **)&dataset->areas, &dataset->area_capacity,
		dataset->area_count + 1, sizeof(*dataset->areas)))
		return 0;
	dataset->areas[dataset->area_count++] = *area;
	memset(area, 0, sizeof(*area));
	return 1;
}

static void EvoBot_NavWorldPlanes(const evobot_bounds_t *bounds,
	evobot_nav_plane_t planes[6])
{
	memset(planes, 0, sizeof(*planes) * 6);
	planes[0].normal.v[0] = 1;
	planes[0].distance = bounds->maxs.v[0];
	planes[1].normal.v[0] = -1;
	planes[1].distance = -bounds->mins.v[0];
	planes[2].normal.v[1] = 1;
	planes[2].distance = bounds->maxs.v[1];
	planes[3].normal.v[1] = -1;
	planes[3].distance = -bounds->mins.v[1];
	planes[4].normal.v[2] = 1;
	planes[4].distance = bounds->maxs.v[2];
	planes[5].normal.v[2] = -1;
	planes[5].distance = -bounds->mins.v[2];
}

static int EvoBot_NavCreateArea(const evobot_nav_plane_t *path,
	size_t path_count, evobot_nav_area_t *area)
{
	evobot_nav_plane_t world_planes[6];
	size_t i;

	memset(area, 0, sizeof(*area));
	area->contents = EVOBOT_CONTENTS_AIR;
	area->dynamic_interactor = EVOBOT_NAV_INVALID_ID;
	EvoBot_NavWorldPlanes(&evobot_nav.world_bounds, world_planes);
	for (i = 0; i < 6; i++)
	{
		if (!EvoBot_NavAddPlane(area, &world_planes[i]))
			goto failed;
	}
	for (i = 0; i < path_count; i++)
	{
		if (!EvoBot_NavAddPlane(area, &path[i]))
			goto failed;
	}
	if (!EvoBot_NavBuildFaces(area))
		goto failed;
	return 1;

failed:
	EvoBot_NavFreeArea(area);
	return 0;
}

static int EvoBot_NavBuildPlayerLeaves(int node_index,
	evobot_nav_plane_t *path, size_t path_count)
{
	evobot_collision_node_t node;
	int side;

	if (node_index < 0)
	{
		evobot_nav_area_t area;

		if (node_index != EVOBOT_COLLISION_LEAF_AIR)
			return 1;
		if (!EvoBot_NavCreateArea(path, path_count, &area))
			return 1;
		if (!EvoBot_NavAppendArea(&evobot_nav, &area))
		{
			EvoBot_NavFreeArea(&area);
			return 0;
		}
		evobot_nav.stats.initial_areas++;
		return 1;
	}
	if (path_count >= EVOBOT_NAV_MAX_PLANES || !evobot_nav_host.collision_node ||
		!evobot_nav_host.collision_node(EVOBOT_COLLISION_TREE_PLAYER,
			node_index, &node))
		return 0;
	evobot_nav.stats.collision_nodes++;
	for (side = 0; side < 2; side++)
	{
		evobot_nav_plane_t plane;

		plane.normal = node.normal;
		plane.distance = node.distance;
		if (side == 0)
			plane = EvoBot_NavReversePlane(&plane);
		path[path_count] = plane;
		if (!EvoBot_NavBuildPlayerLeaves(node.children[side], path,
			path_count + 1))
			return 0;
	}
	return 1;
}

static void EvoBot_NavAreaPlaneRange(const evobot_nav_area_t *area,
	const evobot_nav_plane_t *plane, float *minimum, float *maximum)
{
	size_t i;
	int found = 0;

	*minimum = 0;
	*maximum = 0;
	for (i = 0; i < area->face_count; i++)
	{
		size_t j;
		for (j = 0; j < area->faces[i].vertex_count; j++)
		{
			float distance = EvoBot_NavPlaneDistance(plane,
				&area->faces[i].vertices[j]);

			if (!found || distance < *minimum)
				*minimum = distance;
			if (!found || distance > *maximum)
				*maximum = distance;
			found = 1;
		}
	}
}

static int EvoBot_NavSplitArea(const evobot_nav_area_t *source,
	const evobot_nav_plane_t *plane, evobot_nav_area_t *back,
	evobot_nav_area_t *front)
{
	evobot_nav_plane_t reverse = EvoBot_NavReversePlane(plane);

	if (!EvoBot_NavCopyArea(source, back) || !EvoBot_NavAddPlane(back, plane) ||
		!EvoBot_NavBuildFaces(back))
		goto failed;
	if (!EvoBot_NavCopyArea(source, front) ||
		!EvoBot_NavAddPlane(front, &reverse) || !EvoBot_NavBuildFaces(front))
		goto failed;
	return 1;

failed:
	EvoBot_NavFreeArea(back);
	EvoBot_NavFreeArea(front);
	return 0;
}

static evobot_contents_t EvoBot_NavLeafContents(int leaf)
{
	switch (leaf)
	{
	case EVOBOT_COLLISION_LEAF_WATER:
		return EVOBOT_CONTENTS_WATER;
	case EVOBOT_COLLISION_LEAF_SLIME:
		return EVOBOT_CONTENTS_SLIME;
	case EVOBOT_COLLISION_LEAF_LAVA:
		return EVOBOT_CONTENTS_LAVA;
	case EVOBOT_COLLISION_LEAF_SOLID:
		return EVOBOT_CONTENTS_SOLID;
	default:
		return EVOBOT_CONTENTS_AIR;
	}
}

static int EvoBot_NavSplitContents(evobot_nav_area_t *area, int node_index,
	evobot_nav_dataset_t *output)
{
	evobot_collision_node_t node;
	evobot_nav_plane_t plane;
	float minimum;
	float maximum;
	float feet_offset = evobot_nav.player_bounds.mins.v[2] + 1.0f;

	if (node_index < 0)
	{
		area->contents = EvoBot_NavLeafContents(node_index);
		if (area->contents == EVOBOT_CONTENTS_SOLID)
		{
			EvoBot_NavFreeArea(area);
			return 1;
		}
		return EvoBot_NavAppendArea(output, area);
	}
	if (!evobot_nav_host.collision_node ||
		!evobot_nav_host.collision_node(EVOBOT_COLLISION_TREE_POINT,
			node_index, &node))
		return 0;
	plane.normal = node.normal;
	plane.distance = node.distance - node.normal.v[2] * feet_offset;
	EvoBot_NavAreaPlaneRange(area, &plane, &minimum, &maximum);
	if (minimum >= -EVOBOT_NAV_PLANE_EPSILON)
		return EvoBot_NavSplitContents(area, node.children[0], output);
	if (maximum <= EVOBOT_NAV_PLANE_EPSILON)
		return EvoBot_NavSplitContents(area, node.children[1], output);
	else
	{
		evobot_nav_area_t back;
		evobot_nav_area_t front;
		int result;

		memset(&back, 0, sizeof(back));
		memset(&front, 0, sizeof(front));
		if (!EvoBot_NavSplitArea(area, &plane, &back, &front))
		{
			if (minimum >= -EVOBOT_NAV_DEGENERATE_SPLIT_EPSILON)
				return EvoBot_NavSplitContents(area, node.children[0], output);
			if (maximum <= EVOBOT_NAV_DEGENERATE_SPLIT_EPSILON)
				return EvoBot_NavSplitContents(area, node.children[1], output);
			EvoBot_NavPrintf("EvoBot navigation: contents area split failed "
				"(node %d, planes %zu, range %.6g..%.6g)\n", node_index,
				area->plane_count, minimum, maximum);
			return 0;
		}
		EvoBot_NavFreeArea(area);
		evobot_nav.stats.content_splits++;
		result = EvoBot_NavSplitContents(&front, node.children[0], output);
		if (!result)
		{
			EvoBot_NavFreeArea(&back);
			return 0;
		}
		return EvoBot_NavSplitContents(&back, node.children[1], output);
	}
}

static int EvoBot_NavApplyContentsTree(void)
{
	evobot_collision_tree_t tree;
	evobot_nav_dataset_t output;
	size_t i;

	if (!evobot_nav_host.collision_tree ||
		!evobot_nav_host.collision_tree(EVOBOT_COLLISION_TREE_POINT, &tree))
		return 0;
	memset(&output, 0, sizeof(output));
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		evobot_nav_area_t area = evobot_nav.areas[i];

		memset(&evobot_nav.areas[i], 0, sizeof(evobot_nav.areas[i]));
		if (!EvoBot_NavSplitContents(&area, tree.root_node, &output))
		{
			EvoBot_NavPrintf("EvoBot navigation: contents subdivision failed "
				"at source area %zu/%zu (%zu output areas)\n", i + 1,
				evobot_nav.area_count, output.area_count);
			EvoBot_NavFreeArea(&area);
			EvoBot_NavFreeDataset(&output);
			return 0;
		}
	}
	free(evobot_nav.areas);
	evobot_nav.areas = output.areas;
	evobot_nav.area_count = output.area_count;
	evobot_nav.area_capacity = output.area_capacity;
	return 1;
}

static int EvoBot_NavPointSupport(const evobot_vec3_t *point, float max_distance,
	float *floor_height, float *support_distance, evobot_vec3_t *normal)
{
	evobot_vec3_t end = *point;
	evobot_trace_t trace;

	end.v[2] -= max_distance;
	memset(&trace, 0, sizeof(trace));
	evobot_nav.stats.validation_traces++;
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

static int EvoBot_NavAreaSupportState(const evobot_nav_area_t *area,
	int *has_support, int *has_gap)
{
	evobot_vec3_t center;
	size_t i;

	*has_support = 0;
	*has_gap = 0;
	EvoBot_NavAreaCenter(area, &center);
	for (i = 0; i <= area->face_count; i++)
	{
		evobot_vec3_t point = center;
		float floor_height;
		float support_distance;
		evobot_vec3_t normal;

		if (i < area->face_count && area->faces[i].vertex_count)
		{
			size_t j;
			memset(&point, 0, sizeof(point));
			for (j = 0; j < area->faces[i].vertex_count; j++)
			{
				point.v[0] += area->faces[i].vertices[j].v[0];
				point.v[1] += area->faces[i].vertices[j].v[1];
				point.v[2] += area->faces[i].vertices[j].v[2];
			}
			point.v[0] /= (float)area->faces[i].vertex_count;
			point.v[1] /= (float)area->faces[i].vertex_count;
			point.v[2] = center.v[2];
			point.v[0] = point.v[0] * 0.98f + center.v[0] * 0.02f;
			point.v[1] = point.v[1] * 0.98f + center.v[1] * 0.02f;
		}
		if (EvoBot_NavPointSupport(&point, EVOBOT_NAV_SUPPORT_PROBE,
			&floor_height, &support_distance, &normal))
			*has_support = 1;
		else
			*has_gap = 1;
	}
	return 1;
}

static int EvoBot_NavVerticalEdgePlane(const evobot_nav_face_t *face,
	const evobot_nav_plane_t *ground, size_t edge, evobot_nav_plane_t *plane)
{
	const evobot_vec3_t *a = &face->vertices[edge];
	const evobot_vec3_t *b = &face->vertices[(edge + 1) % face->vertex_count];
	evobot_vec3_t center;
	evobot_vec3_t edge_vector;
	float side;
	size_t i;

	(void)ground;
	EvoBot_NavSubtract(b, a, &edge_vector);
	edge_vector.v[2] = 0;
	if (EvoBot_NavLength(&edge_vector) < 0.5f)
		return 0;
	plane->normal.v[0] = edge_vector.v[1];
	plane->normal.v[1] = -edge_vector.v[0];
	plane->normal.v[2] = 0;
	EvoBot_NavNormalize(&plane->normal);
	plane->distance = EvoBot_NavDot(&plane->normal, a);
	memset(&center, 0, sizeof(center));
	for (i = 0; i < face->vertex_count; i++)
	{
		center.v[0] += face->vertices[i].v[0];
		center.v[1] += face->vertices[i].v[1];
	}
	center.v[0] /= (float)face->vertex_count;
	center.v[1] /= (float)face->vertex_count;
	side = EvoBot_NavPlaneDistance(plane, &center);
	if (side > 0)
		*plane = EvoBot_NavReversePlane(plane);
	return 1;
}

static void EvoBot_NavClassifyArea(evobot_nav_area_t *area)
{
	evobot_vec3_t center;
	evobot_contents_t feet;
	evobot_contents_t middle;
	evobot_contents_t head;
	evobot_vec3_t sample;
	int liquid_contents;
	size_t i;

	EvoBot_NavAreaCenter(area, &center);
	area->supported = 0;
	area->floor_height = 0;
	area->support_distance = 0;
	memset(&area->support_normal, 0, sizeof(area->support_normal));
	liquid_contents = area->contents == EVOBOT_CONTENTS_WATER ||
		area->contents == EVOBOT_CONTENTS_SLIME ||
		area->contents == EVOBOT_CONTENTS_LAVA;
	if (liquid_contents)
	{
		area->water_level = 1;
		sample = center;
		sample.v[2] += (evobot_nav.player_bounds.mins.v[2] +
			evobot_nav.player_bounds.maxs.v[2]) * 0.5f;
		middle = evobot_nav_host.point_contents ?
			evobot_nav_host.point_contents(&sample) : EVOBOT_CONTENTS_AIR;
		if (middle == area->contents)
		{
			area->water_level = 2;
			sample = center;
			sample.v[2] += 22.0f;
			head = evobot_nav_host.point_contents ?
				evobot_nav_host.point_contents(&sample) : EVOBOT_CONTENTS_AIR;
			if (head == area->contents)
				area->water_level = 3;
		}
		if (area->water_level >= 2)
			return;
	}
	else
	{
		area->contents = EVOBOT_CONTENTS_AIR;
		area->water_level = 0;
	}
	for (i = 0; i < area->face_count; i++)
	{
		evobot_nav_plane_t *plane = &area->planes[area->faces[i].plane];
		evobot_vec3_t point;
		float floor_height;
		float support_distance;
		evobot_vec3_t normal;
		size_t j;

		if (plane->normal.v[2] > -EVOBOT_NAV_WALKABLE_NORMAL_Z)
			continue;
		memset(&point, 0, sizeof(point));
		for (j = 0; j < area->faces[i].vertex_count; j++)
		{
			point.v[0] += area->faces[i].vertices[j].v[0];
			point.v[1] += area->faces[i].vertices[j].v[1];
			point.v[2] += area->faces[i].vertices[j].v[2];
		}
		point.v[0] /= (float)area->faces[i].vertex_count;
		point.v[1] /= (float)area->faces[i].vertex_count;
		point.v[2] /= (float)area->faces[i].vertex_count;
		point.v[2] += 1.0f;
		if (EvoBot_NavPointSupport(&point, EVOBOT_NAV_STEP_SIZE + 2.0f,
			&floor_height, &support_distance, &normal))
		{
			area->supported = 1;
			area->floor_height = floor_height;
			area->support_distance = support_distance;
			area->support_normal = normal;
			break;
		}
	}
	sample = center;
	sample.v[2] += evobot_nav.player_bounds.mins.v[2] + 1.0f;
	feet = evobot_nav_host.point_contents ?
		evobot_nav_host.point_contents(&sample) : EVOBOT_CONTENTS_AIR;
	if (feet == EVOBOT_CONTENTS_WATER || feet == EVOBOT_CONTENTS_SLIME ||
		feet == EVOBOT_CONTENTS_LAVA)
		area->contents = feet;
}

static int EvoBot_NavGravitySubdivideArea(evobot_nav_area_t *source,
	evobot_nav_dataset_t *output)
{
	evobot_nav_plane_t *splits = NULL;
	size_t split_count = 0;
	size_t split_capacity = 0;
	evobot_nav_dataset_t fragments;
	int has_support;
	int has_gap;
	size_t i;

	memset(&fragments, 0, sizeof(fragments));
	if (source->contents == EVOBOT_CONTENTS_WATER ||
		source->contents == EVOBOT_CONTENTS_SLIME ||
		source->contents == EVOBOT_CONTENTS_LAVA)
	{
		EvoBot_NavClassifyArea(source);
		return EvoBot_NavAppendArea(output, source);
	}
	EvoBot_NavAreaSupportState(source, &has_support, &has_gap);
	if (!has_support || !has_gap)
	{
		EvoBot_NavClassifyArea(source);
		return EvoBot_NavAppendArea(output, source);
	}
	for (i = 0; i < source->face_count; i++)
	{
		evobot_nav_face_t *face = &source->faces[i];
		evobot_nav_plane_t *ground = &source->planes[face->plane];
		size_t edge;

		if (ground->normal.v[2] > -EVOBOT_NAV_WALKABLE_NORMAL_Z)
			continue;
		for (edge = 0; edge < face->vertex_count; edge++)
		{
			evobot_nav_plane_t split;
			size_t existing;

			if (!EvoBot_NavVerticalEdgePlane(face, ground, edge, &split))
				continue;
			for (existing = 0; existing < split_count; existing++)
			{
				if (EvoBot_NavPlaneEqual(&splits[existing], &split) ||
					EvoBot_NavPlaneEqual(&splits[existing],
						&(evobot_nav_plane_t){
							{{-split.normal.v[0], -split.normal.v[1], -split.normal.v[2]}},
							-split.distance}))
					break;
			}
			if (existing != split_count)
				continue;
			if (!EvoBot_NavReserve((void **)&splits, &split_capacity,
				split_count + 1, sizeof(*splits)))
			{
				EvoBot_NavPrintf("EvoBot navigation: gravity split-plane "
					"allocation failed (%zu planes)\n", split_count + 1);
				goto failed;
			}
			splits[split_count++] = split;
		}
	}
	if (!EvoBot_NavAppendArea(&fragments, source))
		goto failed;
	for (i = 0; i < split_count; i++)
	{
		size_t j = 0;

		while (j < fragments.area_count)
		{
			evobot_nav_area_t *fragment = &fragments.areas[j];
			float minimum;
			float maximum;

			EvoBot_NavAreaPlaneRange(fragment, &splits[i], &minimum, &maximum);
			/* Plane intersections can leave sub-point-epsilon slivers that cannot
			 * form a valid convex face.  They do not materially change support
			 * classification, so do not attempt to split them. */
			if (minimum < -EVOBOT_NAV_POINT_EPSILON &&
				maximum > EVOBOT_NAV_POINT_EPSILON)
			{
				static const float retry_offsets[] = {
					0.05f, -0.05f, 0.1f, -0.1f, 0.25f, -0.25f, 0.5f, -0.5f
				};
				evobot_nav_area_t back;
				evobot_nav_area_t front;
				int split_succeeded;
				size_t attempt;

				memset(&back, 0, sizeof(back));
				memset(&front, 0, sizeof(front));
				split_succeeded = EvoBot_NavSplitArea(fragment, &splits[i],
					&back, &front);
				for (attempt = 0; !split_succeeded &&
					attempt < sizeof(retry_offsets) / sizeof(retry_offsets[0]);
					attempt++)
				{
					evobot_nav_plane_t adjusted = splits[i];

					adjusted.distance += retry_offsets[attempt];
					split_succeeded = EvoBot_NavSplitArea(fragment, &adjusted,
						&back, &front);
				}
				if (!split_succeeded)
				{
					if (minimum >= -EVOBOT_NAV_DEGENERATE_SPLIT_EPSILON ||
						maximum <= EVOBOT_NAV_DEGENERATE_SPLIT_EPSILON)
					{
						j++;
						continue;
					}
					EvoBot_NavPrintf("EvoBot navigation: gravity area split "
						"failed (split %zu/%zu, fragment %zu/%zu, planes %zu, "
						"range %.6g..%.6g)\n", i + 1, split_count, j + 1,
						fragments.area_count, fragment->plane_count, minimum,
						maximum);
					goto failed;
				}
				EvoBot_NavFreeArea(fragment);
				*fragment = back;
				if (!EvoBot_NavAppendArea(&fragments, &front))
				{
					EvoBot_NavFreeArea(&front);
					goto failed;
				}
				evobot_nav.stats.gravity_splits++;
			}
			j++;
		}
	}
	for (i = 0; i < fragments.area_count; i++)
	{
		evobot_nav_area_t fragment = fragments.areas[i];

		memset(&fragments.areas[i], 0, sizeof(fragments.areas[i]));
		EvoBot_NavClassifyArea(&fragment);
		if (!EvoBot_NavAppendArea(output, &fragment))
		{
			EvoBot_NavFreeArea(&fragment);
			goto failed;
		}
	}
	free(fragments.areas);
	free(splits);
	return 1;

failed:
	EvoBot_NavFreeDataset(&fragments);
	free(splits);
	return 0;
}

static int EvoBot_NavApplyGravitySubdivision(void)
{
	evobot_nav_dataset_t output;
	size_t i;

	memset(&output, 0, sizeof(output));
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		evobot_nav_area_t area = evobot_nav.areas[i];

		memset(&evobot_nav.areas[i], 0, sizeof(evobot_nav.areas[i]));
		if (!EvoBot_NavGravitySubdivideArea(&area, &output))
		{
			EvoBot_NavPrintf("EvoBot navigation: gravity subdivision "
				"failed at source area %zu/%zu (%zu output areas)\n",
				i + 1, evobot_nav.area_count, output.area_count);
			EvoBot_NavFreeArea(&area);
			EvoBot_NavFreeDataset(&output);
			return 0;
		}
	}
	free(evobot_nav.areas);
	evobot_nav.areas = output.areas;
	evobot_nav.area_count = output.area_count;
	evobot_nav.area_capacity = output.area_capacity;
	return 1;
}

static int EvoBot_NavBoundsTouch(const evobot_bounds_t *a,
	const evobot_bounds_t *b)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (a->maxs.v[axis] < b->mins.v[axis] - EVOBOT_NAV_POINT_EPSILON ||
			a->mins.v[axis] > b->maxs.v[axis] + EVOBOT_NAV_POINT_EPSILON)
			return 0;
	}
	return 1;
}

static int EvoBot_NavClipPolygon(const evobot_vec3_t *input, size_t input_count,
	const evobot_nav_plane_t *plane, evobot_vec3_t **output, size_t *output_count)
{
	evobot_vec3_t *vertices;
	size_t capacity = input_count + 2;
	size_t count = 0;
	size_t i;

	vertices = malloc(capacity * sizeof(*vertices));
	if (!vertices)
		return 0;
	for (i = 0; i < input_count; i++)
	{
		const evobot_vec3_t *a = &input[i];
		const evobot_vec3_t *b = &input[(i + 1) % input_count];
		float da = EvoBot_NavPlaneDistance(plane, a);
		float db = EvoBot_NavPlaneDistance(plane, b);
		int inside_a = da <= EVOBOT_NAV_PLANE_EPSILON;
		int inside_b = db <= EVOBOT_NAV_PLANE_EPSILON;

		if (inside_a)
			vertices[count++] = *a;
		if (inside_a != inside_b)
		{
			float fraction = da / (da - db);
			int axis;

			for (axis = 0; axis < 3; axis++)
				vertices[count].v[axis] = a->v[axis] +
					fraction * (b->v[axis] - a->v[axis]);
			count++;
		}
	}
	*output = vertices;
	*output_count = count;
	return 1;
}

static int EvoBot_NavFaceOverlap(const evobot_nav_area_t *a,
	const evobot_nav_face_t *face_a, const evobot_nav_area_t *b,
	evobot_vec3_t **vertices, size_t *vertex_count)
{
	evobot_vec3_t *polygon;
	size_t count = face_a->vertex_count;
	size_t i;

	polygon = malloc(count * sizeof(*polygon));
	if (!polygon)
		return 0;
	memcpy(polygon, face_a->vertices, count * sizeof(*polygon));
	for (i = 0; i < b->plane_count && count >= 3; i++)
	{
		evobot_vec3_t *clipped = NULL;
		size_t clipped_count = 0;

		if (!EvoBot_NavClipPolygon(polygon, count, &b->planes[i],
			&clipped, &clipped_count))
		{
			free(polygon);
			return 0;
		}
		free(polygon);
		polygon = clipped;
		count = clipped_count;
	}
	if (count < 3)
	{
		free(polygon);
		return 0;
	}
	*vertices = polygon;
	*vertex_count = count;
	(void)a;
	return 1;
}

static int EvoBot_NavFindSharedFace(const evobot_nav_area_t *a,
	const evobot_nav_area_t *b, int *face_a_index, int *face_b_index,
	evobot_vec3_t **vertices, size_t *vertex_count)
{
	size_t i;
	size_t j;

	if (!EvoBot_NavBoundsTouch(&a->bounds, &b->bounds))
		return 0;
	for (i = 0; i < a->face_count; i++)
	{
		evobot_nav_plane_t reverse = EvoBot_NavReversePlane(
			&a->planes[a->faces[i].plane]);

		for (j = 0; j < b->face_count; j++)
		{
			if (!EvoBot_NavPlaneEqual(&reverse, &b->planes[b->faces[j].plane]))
				continue;
			if (!EvoBot_NavFaceOverlap(a, &a->faces[i], b, vertices,
				vertex_count))
				continue;
			*face_a_index = (int)i;
			*face_b_index = (int)j;
			return 1;
		}
	}
	return 0;
}

static int EvoBot_NavCollectVertices(const evobot_nav_area_t *area,
	evobot_vec3_t **vertices, size_t *count, size_t *capacity)
{
	size_t i;

	for (i = 0; i < area->face_count; i++)
	{
		size_t j;
		for (j = 0; j < area->faces[i].vertex_count; j++)
		{
			size_t existing;

			for (existing = 0; existing < *count; existing++)
			{
				if (EvoBot_NavSamePoint(&(*vertices)[existing],
					&area->faces[i].vertices[j]))
					break;
			}
			if (existing != *count)
				continue;
			if (!EvoBot_NavReserve((void **)vertices, capacity, *count + 1,
				sizeof(**vertices)))
				return 0;
			(*vertices)[(*count)++] = area->faces[i].vertices[j];
		}
	}
	return 1;
}

static int EvoBot_NavAreaCompatible(const evobot_nav_area_t *a,
	const evobot_nav_area_t *b)
{
	return a->contents == b->contents && a->supported == b->supported &&
		a->water_level == b->water_level &&
		a->dynamic_interactor == b->dynamic_interactor &&
		(!a->supported || fabsf(a->floor_height - b->floor_height) <=
			EVOBOT_NAV_STEP_SIZE);
}

static int EvoBot_NavValidateArea(const evobot_nav_area_t *area)
{
	evobot_vec3_t center;
	size_t i;

	if (!evobot_nav_host.trace_player_world)
		return 0;
	EvoBot_NavAreaCenter(area, &center);
	for (i = 0; i < area->face_count; i++)
	{
		size_t j;
		for (j = 0; j < area->faces[i].vertex_count; j++)
		{
			evobot_vec3_t point;
			evobot_trace_t trace;
			int axis;

			for (axis = 0; axis < 3; axis++)
				point.v[axis] = area->faces[i].vertices[j].v[axis] * 0.98f +
					center.v[axis] * 0.02f;
			memset(&trace, 0, sizeof(trace));
			evobot_nav.stats.validation_traces++;
			if (!evobot_nav_host.trace_player_world(&center, &point, &trace) ||
				trace.start_solid || trace.all_solid || trace.fraction < 0.999f)
				return 0;
		}
	}
	return 1;
}

static int EvoBot_NavTryMerge(const evobot_nav_area_t *a,
	const evobot_nav_area_t *b, evobot_nav_area_t *merged)
{
	evobot_vec3_t *vertices = NULL;
	size_t vertex_count = 0;
	size_t vertex_capacity = 0;
	size_t i;
	int face_a;
	int face_b;
	evobot_vec3_t *shared = NULL;
	size_t shared_count = 0;

	memset(merged, 0, sizeof(*merged));
	if (!EvoBot_NavAreaCompatible(a, b) ||
		!EvoBot_NavFindSharedFace(a, b, &face_a, &face_b, &shared, &shared_count))
		return 0;
	free(shared);
	if (!EvoBot_NavCollectVertices(a, &vertices, &vertex_count, &vertex_capacity) ||
		!EvoBot_NavCollectVertices(b, &vertices, &vertex_count, &vertex_capacity))
		goto failed;
	*merged = *a;
	merged->planes = NULL;
	merged->plane_count = 0;
	merged->faces = NULL;
	merged->face_count = 0;
	for (i = 0; i < a->plane_count + b->plane_count; i++)
	{
		const evobot_nav_plane_t *plane = i < a->plane_count ?
			&a->planes[i] : &b->planes[i - a->plane_count];
		size_t j;

		for (j = 0; j < vertex_count; j++)
		{
			if (EvoBot_NavPlaneDistance(plane, &vertices[j]) >
				EVOBOT_NAV_PLANE_EPSILON)
				break;
		}
		if (j == vertex_count && !EvoBot_NavAddPlane(merged, plane))
			goto failed;
	}
	if (!EvoBot_NavBuildFaces(merged))
		goto failed;
	for (i = 0; i < merged->face_count; i++)
	{
		size_t j;

		for (j = 0; j < merged->faces[i].vertex_count; j++)
		{
			const evobot_vec3_t *point = &merged->faces[i].vertices[j];

			if (!EvoBot_NavPointInside(a, point, EVOBOT_NAV_PLANE_EPSILON) &&
				!EvoBot_NavPointInside(b, point, EVOBOT_NAV_PLANE_EPSILON))
				goto failed;
		}
	}
	if (!EvoBot_NavValidateArea(merged))
		goto failed;
	free(vertices);
	return 1;

failed:
	free(vertices);
	EvoBot_NavFreeArea(merged);
	return 0;
}

static void EvoBot_NavMergeAreas(void)
{
	int changed = 1;
	int pass = 0;

	while (changed && pass++ < 16)
	{
		size_t i;

		changed = 0;
		for (i = 0; i < evobot_nav.area_count; i++)
		{
			size_t j;

			for (j = i + 1; j < evobot_nav.area_count; j++)
			{
				evobot_nav_area_t merged;

				if (!EvoBot_NavTryMerge(&evobot_nav.areas[i],
					&evobot_nav.areas[j], &merged))
					continue;
				EvoBot_NavFreeArea(&evobot_nav.areas[i]);
				EvoBot_NavFreeArea(&evobot_nav.areas[j]);
				evobot_nav.areas[i] = merged;
				if (j + 1 < evobot_nav.area_count)
					memmove(&evobot_nav.areas[j], &evobot_nav.areas[j + 1],
						(evobot_nav.area_count - j - 1) * sizeof(*evobot_nav.areas));
				evobot_nav.area_count--;
				evobot_nav.stats.merges++;
				changed = 1;
				break;
			}
			if (changed)
				break;
		}
	}
}

static evobot_nav_face_kind_t EvoBot_NavPortalKind(const evobot_nav_area_t *a,
	const evobot_nav_area_t *b)
{
	int liquid_a = a->contents == EVOBOT_CONTENTS_WATER ||
		a->contents == EVOBOT_CONTENTS_SLIME || a->contents == EVOBOT_CONTENTS_LAVA;
	int liquid_b = b->contents == EVOBOT_CONTENTS_WATER ||
		b->contents == EVOBOT_CONTENTS_SLIME || b->contents == EVOBOT_CONTENTS_LAVA;

	if ((liquid_a || liquid_b) && a->contents != b->contents)
		return EVOBOT_NAV_FACE_LIQUID;
	if (a->supported != b->supported ||
		(a->supported && b->supported &&
			fabsf(a->floor_height - b->floor_height) > EVOBOT_NAV_STEP_SIZE))
		return EVOBOT_NAV_FACE_LEDGE;
	return EVOBOT_NAV_FACE_PORTAL;
}

static int EvoBot_NavAddPortal(evobot_nav_portal_t *portal)
{
	if (!EvoBot_NavReserve((void **)&evobot_nav.portals,
		&evobot_nav.portal_capacity, evobot_nav.portal_count + 1,
		sizeof(*evobot_nav.portals)))
		return 0;
	evobot_nav.portals[evobot_nav.portal_count++] = *portal;
	memset(portal, 0, sizeof(*portal));
	return 1;
}

static int EvoBot_NavGeneratePortals(void)
{
	size_t i;

	for (i = 0; i < evobot_nav.portal_count; i++)
		free(evobot_nav.portals[i].vertices);
	free(evobot_nav.portals);
	evobot_nav.portals = NULL;
	evobot_nav.portal_count = 0;
	evobot_nav.portal_capacity = 0;
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		size_t face;

		evobot_nav.areas[i].id = (uint32_t)(i + 1);
		for (face = 0; face < evobot_nav.areas[i].face_count; face++)
		{
			evobot_nav.areas[i].faces[face].kind = EVOBOT_NAV_FACE_SOLID;
			evobot_nav.areas[i].faces[face].portal = EVOBOT_NAV_INVALID_ID;
		}
	}
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		size_t j;

		for (j = i + 1; j < evobot_nav.area_count; j++)
		{
			int face_a;
			int face_b;
			evobot_nav_portal_t portal;

			memset(&portal, 0, sizeof(portal));
			if (!EvoBot_NavFindSharedFace(&evobot_nav.areas[i],
				&evobot_nav.areas[j], &face_a, &face_b, &portal.vertices,
				&portal.vertex_count))
				continue;
			portal.id = (uint32_t)(evobot_nav.portal_count + 1);
			portal.area_a = evobot_nav.areas[i].id;
			portal.area_b = evobot_nav.areas[j].id;
			portal.kind = EvoBot_NavPortalKind(&evobot_nav.areas[i],
				&evobot_nav.areas[j]);
			portal.plane = evobot_nav.areas[i].planes[
				evobot_nav.areas[i].faces[face_a].plane];
			if (!EvoBot_NavAddPortal(&portal))
			{
				free(portal.vertices);
				return 0;
			}
			evobot_nav.areas[i].faces[face_a].kind =
				evobot_nav.portals[evobot_nav.portal_count - 1].kind;
			evobot_nav.areas[i].faces[face_a].portal =
				(int)evobot_nav.portal_count;
			evobot_nav.areas[j].faces[face_b].kind =
				evobot_nav.portals[evobot_nav.portal_count - 1].kind;
			evobot_nav.areas[j].faces[face_b].portal =
				(int)evobot_nav.portal_count;
		}
	}
	return 1;
}

static int EvoBot_NavBoundsIntersect(const evobot_bounds_t *a,
	const evobot_bounds_t *b)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (a->maxs.v[axis] <= b->mins.v[axis] ||
			a->mins.v[axis] >= b->maxs.v[axis])
			return 0;
	}
	return 1;
}

static int EvoBot_NavDynamicForArea(const evobot_nav_area_t *area)
{
	int found = EVOBOT_NAV_INVALID_ID;
	size_t i;

	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		const evobot_nav_interactor_t *interactor = &evobot_nav.interactors[i];

		if (!interactor->host.dynamic_brush ||
			!EvoBot_NavBoundsIntersect(&area->bounds,
				&interactor->host.swept_bounds))
			continue;
		if (found == EVOBOT_NAV_INVALID_ID)
			found = (int)interactor->id;
		else if (found != (int)interactor->id)
			return EVOBOT_NAV_MULTIPLE_DYNAMIC;
	}
	return found;
}

static int EvoBot_NavCaptureInteractors(void)
{
	int count;
	int i;

	if (!evobot_nav_host.interactor_count || !evobot_nav_host.get_interactor)
		return 1;
	count = evobot_nav_host.interactor_count();
	for (i = 0; i < count; i++)
	{
		evobot_nav_interactor_t interactor;

		memset(&interactor, 0, sizeof(interactor));
		if (!evobot_nav_host.get_interactor(i, &interactor.host))
			continue;
		if (!EvoBot_NavReserve((void **)&evobot_nav.interactors,
			&evobot_nav.interactor_capacity, evobot_nav.interactor_count + 1,
			sizeof(*evobot_nav.interactors)))
			return 0;
		interactor.id = (uint32_t)(evobot_nav.interactor_count + 1);
		evobot_nav.interactors[evobot_nav.interactor_count++] = interactor;
	}
	return 1;
}

static int EvoBot_NavContentsLiquid(evobot_contents_t contents)
{
	return contents == EVOBOT_CONTENTS_WATER ||
		contents == EVOBOT_CONTENTS_SLIME || contents == EVOBOT_CONTENTS_LAVA;
}

static evobot_nav_area_t *EvoBot_NavAreaById(uint32_t id)
{
	if (!id || id > evobot_nav.area_count || evobot_nav.areas[id - 1].id != id)
		return NULL;
	return &evobot_nav.areas[id - 1];
}

static int EvoBot_NavPortalDirection(const evobot_nav_portal_t *portal,
	uint32_t source_area, evobot_vec3_t *direction, int horizontal)
{
	float length;

	*direction = portal->plane.normal;
	if (portal->area_b == source_area)
	{
		direction->v[0] = -direction->v[0];
		direction->v[1] = -direction->v[1];
		direction->v[2] = -direction->v[2];
	}
	if (horizontal)
		direction->v[2] = 0;
	length = EvoBot_NavLength(direction);
	if (length < 0.0001f)
		return 0;
	direction->v[0] /= length;
	direction->v[1] /= length;
	direction->v[2] /= length;
	return 1;
}

static int EvoBot_NavWalkDirection(const evobot_nav_portal_t *portal,
	const evobot_nav_area_t *source, const evobot_nav_area_t *destination,
	evobot_vec3_t *direction, int *projected_interval)
{
	evobot_vec3_t source_center;
	evobot_vec3_t destination_center;
	evobot_vec3_t center_delta;
	evobot_vec3_t edge_delta;
	evobot_vec3_t interval_start;
	float longest_length = 0;
	size_t i;

	if (projected_interval)
		*projected_interval = 0;
	if (EvoBot_NavPortalDirection(portal, source->id, direction, 1))
		return 1;
	for (i = 0; i < portal->vertex_count; i++)
	{
		evobot_vec3_t candidate;
		float length;

		EvoBot_NavSubtract(&portal->vertices[(i + 1) % portal->vertex_count],
			&portal->vertices[i], &candidate);
		candidate.v[2] = 0;
		length = EvoBot_NavDot(&candidate, &candidate);
		if (length > longest_length)
		{
			interval_start = portal->vertices[i];
			edge_delta = candidate;
			longest_length = length;
		}
	}
	if (longest_length < 16.0f)
		return 0;
	direction->v[0] = -edge_delta.v[1];
	direction->v[1] = edge_delta.v[0];
	direction->v[2] = 0;
	if (!EvoBot_NavNormalize(direction))
		return 0;
	for (i = 0; i < portal->vertex_count; i++)
	{
		evobot_vec3_t offset;

		EvoBot_NavSubtract(&portal->vertices[i], &interval_start, &offset);
		if (fabsf(EvoBot_NavDot(&offset, direction)) >
			EVOBOT_NAV_POINT_EPSILON)
			return 0;
	}
	EvoBot_NavAreaCenter(source, &source_center);
	EvoBot_NavAreaCenter(destination, &destination_center);
	EvoBot_NavSubtract(&destination_center, &source_center, &center_delta);
	center_delta.v[2] = 0;
	if (EvoBot_NavDot(direction, &center_delta) < 0)
	{
		direction->v[0] = -direction->v[0];
		direction->v[1] = -direction->v[1];
	}
	if (fabsf(EvoBot_NavDot(direction, &center_delta)) < 0.001f)
		return 0;
	if (projected_interval)
		*projected_interval = 1;
	return 1;
}

static void EvoBot_NavPortalCenter(const evobot_nav_portal_t *portal,
	evobot_vec3_t *center)
{
	size_t i;

	memset(center, 0, sizeof(*center));
	for (i = 0; i < portal->vertex_count; i++)
	{
		center->v[0] += portal->vertices[i].v[0];
		center->v[1] += portal->vertices[i].v[1];
		center->v[2] += portal->vertices[i].v[2];
	}
	if (portal->vertex_count)
	{
		center->v[0] /= (float)portal->vertex_count;
		center->v[1] /= (float)portal->vertex_count;
		center->v[2] /= (float)portal->vertex_count;
	}
}

static size_t EvoBot_NavPortalLongestEdge(const evobot_nav_portal_t *portal)
{
	size_t longest = 0;
	float longest_length = -1;
	size_t i;

	for (i = 0; i < portal->vertex_count; i++)
	{
		evobot_vec3_t delta;
		float length;

		EvoBot_NavSubtract(&portal->vertices[(i + 1) % portal->vertex_count],
			&portal->vertices[i], &delta);
		length = EvoBot_NavDot(&delta, &delta);
		if (length > longest_length)
		{
			longest_length = length;
			longest = i;
		}
	}
	return longest;
}

static uint32_t EvoBot_NavPortalFace(const evobot_nav_area_t *area,
	const evobot_nav_portal_t *portal)
{
	evobot_nav_plane_t expected = portal->plane;
	size_t i;

	if (portal->area_b == area->id)
		expected = EvoBot_NavReversePlane(&expected);
	for (i = 0; i < area->face_count; i++)
	{
		if (EvoBot_NavPlaneEqual(&area->planes[area->faces[i].plane], &expected))
			return (uint32_t)i;
	}
	return EVOBOT_NAV_REACH_INVALID_INDEX;
}

static int EvoBot_NavSupportOriginInternal(const evobot_vec3_t *point,
	const evobot_player_physics_t *physics, evobot_vec3_t *origin,
	evobot_vec3_t *normal, int count_validation)
{
	evobot_vec3_t start = *point;
	evobot_vec3_t end;
	evobot_trace_t trace;
	int valid;

	start.v[2] += physics->step_height + 1.0f;
	end = start;
	end.v[2] -= physics->step_height * 2.0f + 4.0f;
	memset(&trace, 0, sizeof(trace));
	if (count_validation)
		evobot_nav.stats.validation_traces++;
	valid = evobot_nav_host.trace_player_world &&
		evobot_nav_host.trace_player_world(&start, &end, &trace) &&
		!trace.start_solid && !trace.all_solid && trace.fraction < 1.0f &&
		trace.normal.v[2] >= physics->minimum_ground_normal;
	/* Thin player-space cells can have less than step-height clearance above
	 * their valid standing origin.  Retry from the geometry-derived portal
	 * point so an elevated probe cannot skip the cell by starting in a ceiling. */
	if (!valid)
	{
		start = *point;
		start.v[2] += 0.25f;
		end = start;
		end.v[2] -= physics->step_height + 4.0f;
		memset(&trace, 0, sizeof(trace));
		if (count_validation)
			evobot_nav.stats.validation_traces++;
		valid = evobot_nav_host.trace_player_world &&
			evobot_nav_host.trace_player_world(&start, &end, &trace) &&
			!trace.start_solid && !trace.all_solid && trace.fraction < 1.0f &&
			trace.normal.v[2] >= physics->minimum_ground_normal;
	}
	if (!valid)
		return 0;
	*origin = trace.end;
	origin->v[2] += 0.125f;
	if (normal)
		*normal = trace.normal;
	return 1;
}

static int EvoBot_NavSupportOrigin(const evobot_vec3_t *point,
	const evobot_player_physics_t *physics, evobot_vec3_t *origin,
	evobot_vec3_t *normal)
{
	return EvoBot_NavSupportOriginInternal(point, physics, origin, normal, 1);
}

static int EvoBot_NavMoveStepInternal(const evobot_player_move_state_t *state,
	const evobot_player_move_command_t *command,
	evobot_player_move_result_t *result, int count_validation)
{
	if (!evobot_nav_host.simulate_player_move)
		return 0;
	if (count_validation)
		evobot_nav.stats.movement_tests++;
	return evobot_nav_host.simulate_player_move(state, command, result);
}

static int EvoBot_NavMoveStep(const evobot_player_move_state_t *state,
	const evobot_player_move_command_t *command,
	evobot_player_move_result_t *result)
{
	return EvoBot_NavMoveStepInternal(state, command, result, 1);
}

static int EvoBot_NavPointInArea(const evobot_nav_area_t *area,
	const evobot_vec3_t *point)
{
	return area && EvoBot_NavPointInside(area, point, 0.5f);
}

static int EvoBot_NavSegmentIntersectsArea(const evobot_nav_area_t *area,
	const evobot_vec3_t *start, const evobot_vec3_t *end)
{
	float enter = 0.0f;
	float leave = 1.0f;
	size_t i;

	if (!area || !start || !end)
		return 0;
	for (i = 0; i < area->plane_count; i++)
	{
		float start_distance = EvoBot_NavPlaneDistance(&area->planes[i], start) -
			0.5f;
		float end_distance = EvoBot_NavPlaneDistance(&area->planes[i], end) -
			0.5f;
		float fraction;

		if (start_distance <= 0.0f && end_distance <= 0.0f)
			continue;
		if (start_distance > 0.0f && end_distance > 0.0f)
			return 0;
		fraction = start_distance / (start_distance - end_distance);
		if (start_distance > 0.0f)
		{
			if (fraction > enter)
				enter = fraction;
		}
		else if (fraction < leave)
			leave = fraction;
		if (enter > leave)
			return 0;
	}
	return 1;
}

static int EvoBot_NavPlayerSegmentCrossesPortal(
	const evobot_nav_portal_t *portal, const evobot_vec3_t *portal_point,
	const evobot_vec3_t *direction, const evobot_vec3_t *start,
	const evobot_vec3_t *end)
{
	evobot_vec3_t delta;
	evobot_vec3_t from_portal;
	evobot_vec3_t crossing;
	evobot_bounds_t portal_bounds;
	float start_side;
	float travel;
	float fraction;
	size_t i;
	int axis;

	if (!portal || !portal->vertex_count || !portal_point || !direction ||
		!start || !end)
		return 0;
	EvoBot_NavSubtract(start, portal_point, &from_portal);
	EvoBot_NavSubtract(end, start, &delta);
	start_side = EvoBot_NavDot(&from_portal, direction);
	travel = EvoBot_NavDot(&delta, direction);
	if (start_side > EVOBOT_NAV_POINT_EPSILON || travel <= 0.0001f ||
		start_side + travel < -EVOBOT_NAV_POINT_EPSILON)
		return 0;
	fraction = -start_side / travel;
	if (fraction < 0.0f)
		fraction = 0.0f;
	else if (fraction > 1.0f)
		fraction = 1.0f;
	for (axis = 0; axis < 3; axis++)
		crossing.v[axis] = start->v[axis] + delta.v[axis] * fraction;
	portal_bounds.mins = portal->vertices[0];
	portal_bounds.maxs = portal->vertices[0];
	for (i = 1; i < portal->vertex_count; i++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			if (portal->vertices[i].v[axis] < portal_bounds.mins.v[axis])
				portal_bounds.mins.v[axis] = portal->vertices[i].v[axis];
			if (portal->vertices[i].v[axis] > portal_bounds.maxs.v[axis])
				portal_bounds.maxs.v[axis] = portal->vertices[i].v[axis];
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		float player_min = crossing.v[axis] +
			evobot_nav.player_bounds.mins.v[axis];
		float player_max = crossing.v[axis] +
			evobot_nav.player_bounds.maxs.v[axis];

		if (player_max < portal_bounds.mins.v[axis] - EVOBOT_NAV_POINT_EPSILON ||
			player_min > portal_bounds.maxs.v[axis] + EVOBOT_NAV_POINT_EPSILON)
			return 0;
	}
	return 1;
}

static evobot_nav_area_t *EvoBot_NavRoutingAreaAt(const evobot_vec3_t *point,
	uint32_t excluded_area)
{
	size_t i;

	for (i = 0; i < evobot_nav.area_count; i++)
	{
		evobot_nav_area_t *area = &evobot_nav.areas[i];

		if (area->id == excluded_area ||
			(!area->supported && !(EvoBot_NavContentsLiquid(area->contents) &&
				area->water_level >= 2)))
			continue;
		if (EvoBot_NavPointInArea(area, point))
			return area;
	}
	return NULL;
}

static int EvoBot_NavDynamicForPortal(const evobot_nav_portal_t *portal)
{
	evobot_bounds_t bounds;
	int found = EVOBOT_NAV_INVALID_ID;
	size_t i;
	int axis;

	if (!portal->vertex_count)
		return EVOBOT_NAV_INVALID_ID;
	bounds.mins = portal->vertices[0];
	bounds.maxs = portal->vertices[0];
	for (i = 1; i < portal->vertex_count; i++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			if (portal->vertices[i].v[axis] < bounds.mins.v[axis])
				bounds.mins.v[axis] = portal->vertices[i].v[axis];
			if (portal->vertices[i].v[axis] > bounds.maxs.v[axis])
				bounds.maxs.v[axis] = portal->vertices[i].v[axis];
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		bounds.mins.v[axis] -= 1.0f;
		bounds.maxs.v[axis] += 1.0f;
	}
	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		const evobot_nav_interactor_t *interactor = &evobot_nav.interactors[i];

		if (!interactor->host.dynamic_brush ||
			!EvoBot_NavBoundsIntersect(&bounds, &interactor->host.swept_bounds))
			continue;
		if (found == EVOBOT_NAV_INVALID_ID)
			found = (int)interactor->id;
		else if (found != (int)interactor->id)
			return EVOBOT_NAV_MULTIPLE_DYNAMIC;
	}
	return found;
}

static int EvoBot_NavAddReachability(evobot_nav_reachability_t *reachability)
{
	size_t i;

	for (i = 0; i < evobot_nav.reachability_count; i++)
	{
		const evobot_nav_reachability_t *existing =
			&evobot_nav.reachabilities[i];

		if (existing->source_area == reachability->source_area &&
			existing->destination_area == reachability->destination_area &&
			existing->travel_type == reachability->travel_type &&
			existing->portal_id == reachability->portal_id &&
			(reachability->travel_type != EVOBOT_NAV_TRAVEL_TELEPORT ||
				(existing->source_interactor == reachability->source_interactor &&
				existing->destination_interactor ==
					reachability->destination_interactor)) &&
			(reachability->travel_type != EVOBOT_NAV_TRAVEL_PLATFORM ||
				existing->mover_interactor == reachability->mover_interactor))
			return 1;
	}
	if (!EvoBot_NavReserve((void **)&evobot_nav.reachabilities,
		&evobot_nav.reachability_capacity, evobot_nav.reachability_count + 1,
		sizeof(*evobot_nav.reachabilities)))
		return 0;
	reachability->id = (uint32_t)(evobot_nav.reachability_count + 1);
	evobot_nav.reachabilities[evobot_nav.reachability_count++] = *reachability;
	return 1;
}

static int EvoBot_NavRoutingArea(const evobot_nav_area_t *area)
{
	return area->supported || (EvoBot_NavContentsLiquid(area->contents) &&
		area->water_level >= 2);
}

static const evobot_nav_interactor_t *EvoBot_NavTeleportDestination(
	const evobot_nav_interactor_t *trigger)
{
	size_t i;

	if (!trigger->host.target[0])
		return NULL;
	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		const evobot_nav_interactor_t *destination = &evobot_nav.interactors[i];

		if (destination->host.kind == EVOBOT_INTERACTOR_TELEPORT_DESTINATION &&
			destination->host.targetname[0] &&
			!strcmp(trigger->host.target, destination->host.targetname))
			return destination;
	}
	return NULL;
}

static int EvoBot_NavGenerateTeleporters(void)
{
	double started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	size_t i;

	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		const evobot_nav_interactor_t *trigger = &evobot_nav.interactors[i];
		const evobot_nav_interactor_t *destination;
		evobot_nav_area_t *destination_area;
		evobot_bounds_t entry;
		size_t area_index;
		int axis;

		if (trigger->host.kind != EVOBOT_INTERACTOR_TELEPORTER)
			continue;
		evobot_nav.stats.teleporter_triggers++;
		destination = EvoBot_NavTeleportDestination(trigger);
		if (!destination)
			continue;
		destination_area = EvoBot_NavRoutingAreaAt(&destination->host.origin, 0);
		if (!destination_area)
			continue;
		evobot_nav.stats.resolved_teleporters++;
		for (axis = 0; axis < 3; axis++)
		{
			entry.mins.v[axis] = trigger->host.bounds.mins.v[axis] -
				evobot_nav.player_bounds.maxs.v[axis];
			entry.maxs.v[axis] = trigger->host.bounds.maxs.v[axis] -
				evobot_nav.player_bounds.mins.v[axis];
		}
		for (area_index = 0; area_index < evobot_nav.area_count; area_index++)
		{
			evobot_nav_area_t *source = &evobot_nav.areas[area_index];
			evobot_nav_reachability_t reachability;
			evobot_bounds_t usable;
			evobot_vec3_t point;

			if (source == destination_area || !EvoBot_NavRoutingArea(source) ||
				!EvoBot_NavBoundsIntersect(&source->bounds, &entry))
				continue;
			for (axis = 0; axis < 3; axis++)
			{
				usable.mins.v[axis] = source->bounds.mins.v[axis] > entry.mins.v[axis] ?
					source->bounds.mins.v[axis] : entry.mins.v[axis];
				usable.maxs.v[axis] = source->bounds.maxs.v[axis] < entry.maxs.v[axis] ?
					source->bounds.maxs.v[axis] : entry.maxs.v[axis];
				point.v[axis] = (usable.mins.v[axis] + usable.maxs.v[axis]) * 0.5f;
			}
			if (!EvoBot_NavPointInside(source, &point, 1.5f))
				continue;
			memset(&reachability, 0, sizeof(reachability));
			reachability.source_area = source->id;
			reachability.destination_area = destination_area->id;
			reachability.travel_type = EVOBOT_NAV_TRAVEL_TELEPORT;
			reachability.source_face = EVOBOT_NAV_REACH_INVALID_INDEX;
			reachability.source_edge = EVOBOT_NAV_REACH_INVALID_INDEX;
			reachability.start.start = point;
			reachability.start.end = point;
			reachability.destination.start = destination->host.origin;
			reachability.destination.end = destination->host.origin;
			reachability.source_contents = source->contents;
			reachability.destination_contents = destination_area->contents;
			reachability.dynamic_interactor = EVOBOT_NAV_INVALID_ID;
			reachability.source_interactor = trigger->id;
			reachability.destination_interactor = destination->id;
			reachability.entry_bounds = usable;
			reachability.arrival_origin = destination->host.origin;
			reachability.arrival_angles = destination->host.angles;
			reachability.arrival_velocity = destination->host.velocity;
			reachability.base_travel_time = 0.05f;
			if (destination->host.has_angles)
				reachability.flags |= EVOBOT_NAV_REACH_ARRIVAL_ANGLES;
			if (destination->host.has_velocity)
				reachability.flags |= EVOBOT_NAV_REACH_ARRIVAL_VELOCITY;
			if (!EvoBot_NavAddReachability(&reachability))
				return 0;
			evobot_nav.stats.teleport_links++;
		}
	}
	evobot_nav.stats.teleport_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	return 1;
}

static void EvoBot_NavReachGeometry(const evobot_nav_portal_t *portal,
	size_t edge, const evobot_vec3_t *direction,
	evobot_nav_reachability_t *reachability)
{
	const evobot_vec3_t *a = &portal->vertices[edge];
	const evobot_vec3_t *b = &portal->vertices[(edge + 1) % portal->vertex_count];
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		reachability->start.start.v[axis] = a->v[axis] - direction->v[axis];
		reachability->start.end.v[axis] = b->v[axis] - direction->v[axis];
		reachability->destination.start.v[axis] = a->v[axis] + direction->v[axis];
		reachability->destination.end.v[axis] = b->v[axis] + direction->v[axis];
	}
}

static void EvoBot_NavReachCommon(const evobot_nav_portal_t *portal,
	const evobot_nav_area_t *source, const evobot_nav_area_t *destination,
	size_t edge, const evobot_vec3_t *direction,
	evobot_nav_reachability_t *reachability)
{
	memset(reachability, 0, sizeof(*reachability));
	reachability->source_area = source->id;
	reachability->destination_area = destination->id;
	reachability->portal_id = portal->id;
	reachability->source_face = EvoBot_NavPortalFace(source, portal);
	reachability->source_edge = (uint32_t)edge;
	reachability->source_contents = source->contents;
	reachability->destination_contents = destination->contents;
	reachability->dynamic_interactor = EvoBot_NavDynamicForPortal(portal);
	if (reachability->dynamic_interactor > 0)
		reachability->flags |= EVOBOT_NAV_REACH_DYNAMIC_BLOCKER;
	EvoBot_NavReachGeometry(portal, edge, direction, reachability);
}

static int EvoBot_NavSimulateToArea(const evobot_vec3_t *origin,
	const evobot_vec3_t *direction, const evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics, int starting_water_level,
	evobot_player_move_result_t *final_result, float *elapsed,
	int *used_water_jump)
{
	evobot_player_move_state_t state;
	evobot_player_move_command_t command;
	int i;

	memset(&state, 0, sizeof(state));
	state.origin = *origin;
	state.on_ground = starting_water_level < 2;
	state.water_level = starting_water_level;
	state.angles.v[1] = atan2f(direction->v[1], direction->v[0]) * 57.2957795f;
	memset(&command, 0, sizeof(command));
	command.forward_move = physics->maximum_speed;
	command.msec = 50;
	*elapsed = 0;
	*used_water_jump = 0;
	for (i = 0; i < 30; i++)
	{
		evobot_player_move_result_t result;

		if (!EvoBot_NavMoveStep(&state, &command, &result))
			return 0;
		state = result.state;
		*elapsed += command.msec * 0.001f;
		if (state.water_jump_time > 0)
			*used_water_jump = 1;
		if (EvoBot_NavPointInArea(destination, &state.origin))
		{
			*final_result = result;
			return 1;
		}
	}
	return 0;
}

static int EvoBot_NavWalkEdgeInterval(const evobot_nav_portal_t *portal,
	size_t edge, evobot_nav_segment_t *interval)
{
	const evobot_vec3_t *a = &portal->vertices[edge];
	const evobot_vec3_t *b = &portal->vertices[(edge + 1) % portal->vertex_count];
	evobot_vec3_t delta;
	float horizontal_length;
	float inset_fraction;
	int axis;

	EvoBot_NavSubtract(b, a, &delta);
	horizontal_length = sqrtf(delta.v[0] * delta.v[0] +
		delta.v[1] * delta.v[1]);
	if (horizontal_length < EVOBOT_NAV_WALK_EDGE_MIN)
		return 0;
	inset_fraction = EVOBOT_NAV_WALK_EDGE_INSET / horizontal_length;
	if (inset_fraction > 0.25f)
		inset_fraction = 0.25f;
	for (axis = 0; axis < 3; axis++)
	{
		interval->start.v[axis] = a->v[axis] + delta.v[axis] * inset_fraction;
		interval->end.v[axis] = b->v[axis] - delta.v[axis] * inset_fraction;
	}
	return 1;
}

static void EvoBot_NavWalkIntervalPoint(const evobot_nav_segment_t *interval,
	float fraction, evobot_vec3_t *point)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
		point->v[axis] = interval->start.v[axis] +
			(interval->end.v[axis] - interval->start.v[axis]) * fraction;
}

static int EvoBot_NavWalkSampleInterval(const float *fractions,
	const evobot_vec3_t *points, const int *valid,
	evobot_nav_segment_t *interval)
{
	int first = -1;
	int last = -1;
	int i;

	for (i = 0; i < EVOBOT_NAV_WALK_SAMPLE_COUNT; i++)
	{
		if (!valid[i])
			continue;
		if (first < 0 || fractions[i] < fractions[first])
			first = i;
		if (last < 0 || fractions[i] > fractions[last])
			last = i;
	}
	if (first < 0)
		return 0;
	interval->start = points[first];
	interval->end = points[last];
	return 1;
}

static void EvoBot_NavWalkReachGeometry(
	const evobot_nav_segment_t *interval, const evobot_vec3_t *direction,
	evobot_nav_debug_walk_candidate_t *candidate)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		candidate->start.start.v[axis] = interval->start.v[axis] -
			direction->v[axis] * EVOBOT_NAV_WALK_REACH_OFFSET;
		candidate->start.end.v[axis] = interval->end.v[axis] -
			direction->v[axis] * EVOBOT_NAV_WALK_REACH_OFFSET;
		candidate->destination.start.v[axis] = interval->start.v[axis] +
			direction->v[axis] * EVOBOT_NAV_WALK_REACH_OFFSET;
		candidate->destination.end.v[axis] = interval->end.v[axis] +
			direction->v[axis] * EVOBOT_NAV_WALK_REACH_OFFSET;
	}
}

static int EvoBot_NavEvaluateStackedDecomposition(
	const evobot_nav_portal_t *portal, const evobot_nav_area_t *source,
	const evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics, int count_validation,
	evobot_nav_debug_walk_candidate_t *candidate)
{
	evobot_vec3_t center;
	evobot_vec3_t origin;
	evobot_player_move_state_t state;
	evobot_player_move_command_t command;
	evobot_player_move_result_t result;
	float support_dot;
	float movement_distance;
	size_t edge;
	int axis;

	if (fabsf(portal->plane.normal.v[2]) < 0.99f ||
		fabsf(destination->floor_height - source->floor_height) > 0.5f ||
		source->contents != destination->contents ||
		source->dynamic_interactor != destination->dynamic_interactor)
		return 1;
	support_dot = EvoBot_NavDot(&source->support_normal,
		&destination->support_normal);
	if (support_dot < 0.99f)
		return 1;
	EvoBot_NavPortalCenter(portal, &center);
	if (!EvoBot_NavSupportOriginInternal(&center, physics, &origin, NULL,
		count_validation) || center.v[2] < origin.v[2] +
		evobot_nav.player_bounds.mins.v[2] - EVOBOT_NAV_POINT_EPSILON ||
		center.v[2] > origin.v[2] + evobot_nav.player_bounds.maxs.v[2] +
		EVOBOT_NAV_POINT_EPSILON)
		return 1;
	memset(&state, 0, sizeof(state));
	state.origin = origin;
	state.on_ground = 1;
	memset(&command, 0, sizeof(command));
	command.msec = 50;
	memset(&result, 0, sizeof(result));
	if (!EvoBot_NavMoveStepInternal(&state, &command, &result,
		count_validation))
		return 1;
	movement_distance = 0.0f;
	for (axis = 0; axis < 3; axis++)
	{
		float delta = result.state.origin.v[axis] - origin.v[axis];

		movement_distance += delta * delta;
	}
	if (movement_distance > 1.0f || !result.state.on_ground ||
		result.state.water_level >= 2 || result.state.water_jump_time > 0)
		return 1;
	memset(candidate, 0, sizeof(*candidate));
	candidate->present = 1;
	candidate->portal_id = portal->id;
	candidate->source_area = source->id;
	candidate->destination_area = destination->id;
	candidate->direction_valid = 1;
	candidate->stacked_decomposition = 1;
	candidate->portal_interval_count = 1;
	candidate->sample_count = 1;
	candidate->source_support_samples = 1;
	candidate->destination_support_samples = 1;
	candidate->overlap_samples = 1;
	candidate->source_valid = 1;
	candidate->destination_valid = 1;
	candidate->source_in_area = EvoBot_NavPointInArea(source, &origin);
	candidate->destination_in_area = EvoBot_NavPointInArea(destination, &origin);
	candidate->height_valid = 1;
	candidate->step_limit = physics->step_height;
	candidate->start_origin = origin;
	candidate->desired_destination = origin;
	candidate->initial_state = state;
	candidate->command = command;
	candidate->result = result;
	candidate->move_steps = 1;
	candidate->movement_succeeded = 1;
	candidate->reached_destination = 1;
	candidate->hull_crossed_portal = 1;
	candidate->supported_arrival = 1;
	candidate->pm_validation_passed = 1;
	candidate->final_walk = 1;
	edge = EvoBot_NavPortalLongestEdge(portal);
	candidate->edge_index = (uint32_t)edge;
	candidate->portal_interval.start = portal->vertices[edge];
	candidate->portal_interval.end =
		portal->vertices[(edge + 1) % portal->vertex_count];
	candidate->overlap_interval = candidate->portal_interval;
	candidate->overlap_interval.start.v[2] = origin.v[2];
	candidate->overlap_interval.end.v[2] = origin.v[2];
	candidate->source_support_interval = candidate->overlap_interval;
	candidate->destination_support_interval = candidate->overlap_interval;
	candidate->start = candidate->overlap_interval;
	candidate->destination = candidate->overlap_interval;
	return 1;
}

static int EvoBot_NavEvaluateWalkDirection(const evobot_nav_portal_t *portal,
	const evobot_nav_area_t *source, const evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics, int allow_liquid,
	int count_validation,
	evobot_nav_debug_walk_candidate_t *candidate)
{
	static const float sample_fractions[EVOBOT_NAV_WALK_SAMPLE_COUNT] = {
		0.5f, 0.0f, 1.0f
	};
	evobot_vec3_t direction;
	unsigned int interval_count = 0;
	int projected_interval = 0;
	int point_interval = 0;
	int sample_count;
	size_t edge;

	memset(candidate, 0, sizeof(*candidate));
	candidate->portal_id = portal->id;
	candidate->source_area = source->id;
	candidate->destination_area = destination->id;
	candidate->step_limit = physics->step_height;
	if (!source->supported || !destination->supported ||
		(!allow_liquid &&
		 (source->water_level >= 2 || destination->water_level >= 2)) ||
		portal->kind == EVOBOT_NAV_FACE_LEDGE)
		return 1;
	if (!EvoBot_NavWalkDirection(portal, source, destination, &direction,
		&projected_interval))
		return EvoBot_NavEvaluateStackedDecomposition(portal, source,
			destination, physics, count_validation, candidate);
	candidate->direction_valid = 1;
	candidate->projected_interval_direction = projected_interval;
	candidate->direction = direction;
	sample_count = projected_interval ? EVOBOT_NAV_WALK_SAMPLE_COUNT : 1;
	for (edge = 0; edge < portal->vertex_count; edge++)
	{
		evobot_nav_segment_t interval;

		if (EvoBot_NavWalkEdgeInterval(portal, edge, &interval))
			interval_count++;
	}
	if (!projected_interval && !interval_count && portal->vertex_count)
	{
		point_interval = 1;
		interval_count = 1;
	}
	for (edge = 0; edge < portal->vertex_count; edge++)
	{
		evobot_nav_debug_walk_candidate_t current;
		evobot_nav_segment_t interval;
		evobot_vec3_t portal_points[EVOBOT_NAV_WALK_SAMPLE_COUNT];
		evobot_vec3_t source_origins[EVOBOT_NAV_WALK_SAMPLE_COUNT];
		evobot_vec3_t destination_origins[EVOBOT_NAV_WALK_SAMPLE_COUNT];
		int source_valid[EVOBOT_NAV_WALK_SAMPLE_COUNT] = { 0 };
		int destination_valid[EVOBOT_NAV_WALK_SAMPLE_COUNT] = { 0 };
		int overlap_valid[EVOBOT_NAV_WALK_SAMPLE_COUNT] = { 0 };
		float best_result_distance = 3.402823466e+38F;
		int sample;

		if (point_interval)
		{
			if (edge)
				break;
			EvoBot_NavPortalCenter(portal, &interval.start);
			interval.end = interval.start;
		}
		else if (!EvoBot_NavWalkEdgeInterval(portal, edge, &interval))
			continue;
		memset(&current, 0, sizeof(current));
		current.present = 1;
		current.portal_id = portal->id;
		current.source_area = source->id;
		current.destination_area = destination->id;
		current.edge_index = (uint32_t)edge;
		current.direction_valid = 1;
		current.projected_interval_direction = projected_interval;
		current.direction = direction;
		current.portal_interval = interval;
		current.portal_interval_count = interval_count;
		current.sample_count = (unsigned int)sample_count;
		current.step_limit = physics->step_height;
		for (sample = 0; sample < sample_count; sample++)
		{
			evobot_vec3_t source_sample;
			evobot_vec3_t destination_sample;
			evobot_player_move_state_t state;
			evobot_player_move_state_t initial_state;
			evobot_player_move_command_t command;
			evobot_player_move_result_t result;
			float height_delta;
			float result_distance;
			int height_valid;
			int source_in_area;
			int destination_in_area;
			int movement_succeeded = 0;
			int reached_destination = 0;
			int segment_reached_destination = 0;
			int hull_crossed_portal = 0;
			int supported_arrival = 0;
			int water_jump = 0;
			int move_steps = 0;
			int axis;
			int step;

			EvoBot_NavWalkIntervalPoint(&interval, sample_fractions[sample],
				&portal_points[sample]);
			for (axis = 0; axis < 3; axis++)
			{
				source_sample.v[axis] = portal_points[sample].v[axis] -
					direction.v[axis] * EVOBOT_NAV_WALK_CROSSING_OFFSET;
				destination_sample.v[axis] = portal_points[sample].v[axis] +
					direction.v[axis] * EVOBOT_NAV_WALK_CROSSING_OFFSET;
			}
			source_valid[sample] = EvoBot_NavSupportOriginInternal(&source_sample,
				physics, &source_origins[sample], NULL, count_validation);
			destination_valid[sample] = EvoBot_NavSupportOriginInternal(
				&destination_sample, physics, &destination_origins[sample], NULL,
				count_validation);
			if (!source_valid[sample] || !destination_valid[sample])
			{
				for (axis = 0; axis < 3; axis++)
				{
					source_sample.v[axis] = portal_points[sample].v[axis] -
						direction.v[axis] *
						EVOBOT_NAV_WALK_CROSSING_RETRY_OFFSET;
					destination_sample.v[axis] = portal_points[sample].v[axis] +
						direction.v[axis] *
						EVOBOT_NAV_WALK_CROSSING_RETRY_OFFSET;
				}
				source_valid[sample] = EvoBot_NavSupportOriginInternal(
					&source_sample, physics, &source_origins[sample], NULL,
					count_validation);
				destination_valid[sample] = EvoBot_NavSupportOriginInternal(
					&destination_sample, physics, &destination_origins[sample], NULL,
					count_validation);
			}
			if (source_valid[sample])
				current.source_support_samples++;
			if (destination_valid[sample])
				current.destination_support_samples++;
			if (!source_valid[sample] || !destination_valid[sample])
				continue;
			height_delta = destination_origins[sample].v[2] -
				source_origins[sample].v[2];
			height_valid = fabsf(height_delta) <= physics->step_height +
				EVOBOT_NAV_WALK_STEP_EPSILON;
			source_in_area = EvoBot_NavPointInArea(source,
				&source_origins[sample]);
			destination_in_area = EvoBot_NavPointInArea(destination,
				&destination_origins[sample]);
			current.height_delta = height_delta;
			current.height_valid = height_valid;
			current.source_valid = source_valid[sample];
			current.destination_valid = destination_valid[sample];
			current.source_in_area = source_in_area;
			current.destination_in_area = destination_in_area;
			overlap_valid[sample] = height_valid &&
				(!projected_interval ||
					(source_in_area && destination_in_area));
			if (!overlap_valid[sample])
				continue;
			current.overlap_samples++;
			if (current.final_walk)
				continue;
			memset(&state, 0, sizeof(state));
			state.origin = source_origins[sample];
			state.on_ground = 1;
			state.angles.v[1] = atan2f(direction.v[1], direction.v[0]) *
				57.2957795f;
			initial_state = state;
			memset(&command, 0, sizeof(command));
			command.forward_move = physics->maximum_speed;
			command.msec = 50;
			memset(&result, 0, sizeof(result));
			for (step = 0; step < 30; step++)
			{
				if (!EvoBot_NavMoveStepInternal(&state, &command, &result,
					count_validation))
					break;
				movement_succeeded = 1;
				move_steps++;
				segment_reached_destination =
					EvoBot_NavSegmentIntersectsArea(destination, &state.origin,
						&result.state.origin);
				hull_crossed_portal = !projected_interval &&
					!destination_in_area && EvoBot_NavPlayerSegmentCrossesPortal(
						portal, &portal_points[sample], &direction, &state.origin,
						&result.state.origin);
				state = result.state;
				if (state.water_jump_time > 0)
					water_jump = 1;
				if (EvoBot_NavPointInArea(destination, &state.origin) ||
					segment_reached_destination || hull_crossed_portal)
				{
					reached_destination = 1;
					break;
				}
			}
			/* A downhill crossing can enter the destination area one movement
			 * frame before PM_CategorizePosition marks the player grounded.  The
			 * support traces above already prove that both sides of this portal
			 * have walkable floor within the configured step height, so accept an
			 * arrival that remains close to that destination support for either
			 * portal-direction path. */
			if (reached_destination)
				supported_arrival = fabsf(result.state.origin.v[2] -
					destination_origins[sample].v[2]) <= physics->step_height +
					EVOBOT_NAV_WALK_STEP_EPSILON;
			result_distance = 3.402823466e+38F;
			if (movement_succeeded)
			{
				evobot_vec3_t delta;

				EvoBot_NavSubtract(&result.state.origin,
					&destination_origins[sample], &delta);
				result_distance = EvoBot_NavDot(&delta, &delta);
			}
			if (!current.movement_succeeded ||
				result_distance < best_result_distance ||
				(reached_destination && !water_jump &&
				 (result.state.on_ground || supported_arrival) &&
				 (allow_liquid ? result.state.water_level >= 2 :
					result.state.water_level < 2)))
			{
				current.start_origin = source_origins[sample];
				current.desired_destination = destination_origins[sample];
				current.initial_state = initial_state;
				current.command = command;
				current.result = result;
				current.move_steps = (unsigned int)move_steps;
				current.height_delta = height_delta;
				current.movement_succeeded = movement_succeeded;
				current.reached_destination = reached_destination;
				current.segment_reached_destination =
					segment_reached_destination;
				current.hull_crossed_portal = hull_crossed_portal;
				current.supported_arrival = supported_arrival;
				current.water_jump = water_jump;
				current.pm_validation_passed = reached_destination && !water_jump &&
					(result.state.on_ground || supported_arrival) &&
					(allow_liquid ? result.state.water_level >= 2 :
						result.state.water_level < 2);
				current.final_walk = current.pm_validation_passed;
				best_result_distance = result_distance;
			}
		}
		EvoBot_NavWalkSampleInterval(sample_fractions, source_origins,
			source_valid, &current.source_support_interval);
		EvoBot_NavWalkSampleInterval(sample_fractions, destination_origins,
			destination_valid, &current.destination_support_interval);
		if (EvoBot_NavWalkSampleInterval(sample_fractions, portal_points,
			overlap_valid, &current.overlap_interval))
			EvoBot_NavWalkReachGeometry(&current.overlap_interval, &direction,
				&current);
		if (current.final_walk)
		{
			*candidate = current;
			return 1;
		}
		if (!candidate->present ||
			current.overlap_samples > candidate->overlap_samples ||
			(current.overlap_samples == candidate->overlap_samples &&
			 current.source_support_samples + current.destination_support_samples >
			 candidate->source_support_samples +
				candidate->destination_support_samples))
			*candidate = current;
	}
	candidate->portal_interval_count = interval_count;
	candidate->direction_valid = 1;
	candidate->projected_interval_direction = projected_interval;
	candidate->direction = direction;
	return 1;
}

static int EvoBot_NavGenerateWalkDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics)
{
	evobot_nav_debug_walk_candidate_t candidate;
	evobot_nav_reachability_t reachability;

	if (!EvoBot_NavEvaluateWalkDirection(portal, source, destination, physics, 0, 1,
		&candidate) || !candidate.final_walk)
		return 1;
	EvoBot_NavReachCommon(portal, source, destination, candidate.edge_index,
		&candidate.direction, &reachability);
	if (candidate.projected_interval_direction)
	{
		reachability.start = candidate.start;
		reachability.destination = candidate.destination;
	}
	else if (candidate.hull_crossed_portal)
	{
		reachability.start = candidate.stacked_decomposition ?
			candidate.portal_interval : candidate.overlap_interval;
		reachability.destination = reachability.start;
	}
	reachability.travel_type = EVOBOT_NAV_TRAVEL_WALK;
	reachability.height_delta = candidate.height_delta;
	reachability.horizontal_distance = 8.0f;
	reachability.travel_distance = sqrtf(64.0f +
		candidate.height_delta * candidate.height_delta);
	reachability.base_travel_time = candidate.move_steps * 0.001f *
		candidate.command.msec;
	if (candidate.height_delta > 0.5f)
		reachability.flags |= EVOBOT_NAV_REACH_STEP_UP;
	else if (candidate.height_delta < -0.5f)
		reachability.flags |= EVOBOT_NAV_REACH_STEP_DOWN;
	return EvoBot_NavAddReachability(&reachability);
}

static int EvoBot_NavGenerateSwimDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics)
{
	evobot_nav_debug_walk_candidate_t candidate;
	evobot_nav_reachability_t reachability;
	evobot_vec3_t direction;
	size_t edge;

	if (!EvoBot_NavContentsLiquid(source->contents) ||
		!EvoBot_NavContentsLiquid(destination->contents) ||
		!EvoBot_NavPortalDirection(portal, source->id, &direction, 0))
		return 1;
	if (source->water_level < 2 || destination->water_level < 2)
	{
		if (!source->supported || !destination->supported ||
			!EvoBot_NavEvaluateWalkDirection(portal, source, destination, physics,
				1, 1, &candidate) || !candidate.final_walk)
			return 1;
		edge = candidate.edge_index;
		EvoBot_NavReachCommon(portal, source, destination, edge,
			&candidate.direction, &reachability);
		if (candidate.projected_interval_direction)
		{
			reachability.start = candidate.start;
			reachability.destination = candidate.destination;
		}
		else if (candidate.hull_crossed_portal)
		{
			reachability.start = candidate.stacked_decomposition ?
				candidate.portal_interval : candidate.overlap_interval;
			reachability.destination = reachability.start;
		}
		reachability.travel_type = EVOBOT_NAV_TRAVEL_SWIM;
		reachability.height_delta = candidate.height_delta;
		reachability.horizontal_distance = 8.0f;
		reachability.travel_distance = sqrtf(64.0f +
			candidate.height_delta * candidate.height_delta);
		reachability.base_travel_time = candidate.move_steps * 0.001f *
			candidate.command.msec;
		return EvoBot_NavAddReachability(&reachability);
	}
	edge = EvoBot_NavPortalLongestEdge(portal);
	EvoBot_NavReachCommon(portal, source, destination, edge, &direction,
		&reachability);
	reachability.travel_type = EVOBOT_NAV_TRAVEL_SWIM;
	reachability.height_delta = direction.v[2] * 2.0f;
	reachability.horizontal_distance = 2.0f * sqrtf(direction.v[0] * direction.v[0] +
		direction.v[1] * direction.v[1]);
	reachability.travel_distance = 2.0f;
	reachability.base_travel_time = physics->maximum_speed > 0 ?
		reachability.travel_distance / physics->maximum_speed : 0;
	return EvoBot_NavAddReachability(&reachability);
}

static int EvoBot_NavEvaluateWaterJumpDirection(
	const evobot_nav_portal_t *portal, evobot_nav_area_t *source,
	evobot_nav_area_t *destination, const evobot_player_physics_t *physics,
	int count_validation, evobot_nav_debug_air_candidate_t *best);

static int EvoBot_NavEvaluateWaterExitDirection(
	const evobot_nav_portal_t *portal, evobot_nav_area_t *source,
	evobot_nav_area_t *destination, const evobot_player_physics_t *physics,
	int count_validation, evobot_nav_debug_air_candidate_t *best);

static int EvoBot_NavGenerateWaterTransition(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics)
{
	evobot_vec3_t direction;
	evobot_vec3_t center;
	evobot_player_move_result_t result;
	evobot_nav_debug_air_candidate_t candidate;
	evobot_nav_area_t *landing;
	evobot_nav_reachability_t reachability;
	float elapsed;
	int used_water_jump;
	size_t edge;
	size_t before;
	int axis;

	if (!EvoBot_NavPortalDirection(portal, source->id, &direction, 0))
		return 1;
	edge = EvoBot_NavPortalLongestEdge(portal);
	EvoBot_NavPortalCenter(portal, &center);
	if (source->supported && source->water_level < 2 &&
		EvoBot_NavContentsLiquid(destination->contents) &&
		destination->water_level >= 2)
	{
		EvoBot_NavReachCommon(portal, source, destination, edge, &direction,
			&reachability);
		reachability.travel_type = EVOBOT_NAV_TRAVEL_WATER_ENTRY;
		reachability.height_delta = destination->bounds.maxs.v[2] -
			source->floor_height;
		reachability.horizontal_distance = 2.0f;
		reachability.travel_distance = EvoBot_NavLength(&direction) * 2.0f;
		reachability.base_travel_time = physics->maximum_speed > 0 ?
			reachability.travel_distance / physics->maximum_speed : 0;
		return EvoBot_NavAddReachability(&reachability);
	}
	if (!EvoBot_NavContentsLiquid(source->contents) || source->water_level < 2 ||
		!destination->supported || destination->water_level >= 2)
		return 1;
	for (axis = 0; axis < 3; axis++)
		center.v[axis] -= direction.v[axis] * 4.0f;
	if (EvoBot_NavPointInArea(source, &center) &&
		EvoBot_NavSimulateToArea(&center, &direction, destination, physics,
			source->water_level, &result, &elapsed, &used_water_jump) &&
		!used_water_jump)
	{
		EvoBot_NavReachCommon(portal, source, destination, edge, &direction,
			&reachability);
		reachability.travel_type = EVOBOT_NAV_TRAVEL_WATER_EXIT;
		reachability.height_delta = destination->floor_height - center.v[2];
		reachability.horizontal_distance = 8.0f;
		reachability.travel_distance = sqrtf(64.0f +
			reachability.height_delta * reachability.height_delta);
		reachability.base_travel_time = elapsed;
		return EvoBot_NavAddReachability(&reachability);
	}
	if (!EvoBot_NavEvaluateWaterExitDirection(portal, source, destination,
		physics, 1, &candidate))
		return 0;
	landing = candidate.valid ? EvoBot_NavAreaById(candidate.landing_area) : NULL;
	if (landing)
	{
		EvoBot_NavReachCommon(portal, source, landing, candidate.edge_index,
			&candidate.direction, &reachability);
		reachability.travel_type = EVOBOT_NAV_TRAVEL_WATER_EXIT;
		reachability.start.start = candidate.start_origin;
		reachability.start.end = candidate.start_origin;
		reachability.destination.start = candidate.landing_origin;
		reachability.destination.end = candidate.landing_origin;
		reachability.height_delta = candidate.height_delta;
		reachability.horizontal_distance = candidate.horizontal_displacement;
		reachability.travel_distance = sqrtf(
			reachability.horizontal_distance * reachability.horizontal_distance +
			reachability.height_delta * reachability.height_delta);
		reachability.base_travel_time = candidate.elapsed;
		return EvoBot_NavAddReachability(&reachability);
	}
	{
		double started = evobot_nav_host.monotonic_time ?
			evobot_nav_host.monotonic_time() : 0;

		evobot_nav.stats.water_jump_candidates++;
		if (!EvoBot_NavEvaluateWaterJumpDirection(portal, source, destination,
			physics, 1, &candidate))
			return 0;
		evobot_nav.stats.water_jump_attempts += candidate.attempts;
		if (evobot_nav_host.monotonic_time)
			evobot_nav.stats.water_jump_time +=
				evobot_nav_host.monotonic_time() - started;
		landing = candidate.valid ? EvoBot_NavAreaById(candidate.landing_area) : NULL;
		if (landing)
		{
			evobot_nav.stats.water_jump_validated++;
			EvoBot_NavReachCommon(portal, source, landing, candidate.edge_index,
				&candidate.direction, &reachability);
			reachability.travel_type = EVOBOT_NAV_TRAVEL_WATER_JUMP;
			reachability.start.start = candidate.start_origin;
			reachability.start.end = candidate.start_origin;
			reachability.destination.start = candidate.landing_origin;
			reachability.destination.end = candidate.landing_origin;
			reachability.height_delta = candidate.height_delta;
			reachability.horizontal_distance = candidate.horizontal_displacement;
			reachability.travel_distance = sqrtf(
				reachability.horizontal_distance *
					reachability.horizontal_distance +
				reachability.height_delta * reachability.height_delta);
			reachability.base_travel_time = candidate.elapsed;
			before = evobot_nav.reachability_count;
			if (!EvoBot_NavAddReachability(&reachability))
				return 0;
			if (before != evobot_nav.reachability_count)
				evobot_nav.stats.water_jump_links++;
			return 1;
		}
	}
	EvoBot_NavReachCommon(portal, source, destination, edge, &direction,
		&reachability);
	reachability.travel_type = EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP;
	reachability.height_delta = destination->floor_height -
		source->bounds.maxs.v[2];
	reachability.horizontal_distance = 8.0f;
	reachability.travel_distance = sqrtf(64.0f +
		reachability.height_delta * reachability.height_delta);
	reachability.base_travel_time = 0;
	return EvoBot_NavAddReachability(&reachability);
}

static int EvoBot_NavEvaluateAirDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics, evobot_nav_debug_air_kind_t kind,
	int count_validation, evobot_nav_debug_air_candidate_t *best);

static int EvoBot_NavGenerateDropDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *adjacent,
	const evobot_player_physics_t *physics)
{
	evobot_nav_debug_air_candidate_t candidate;
	evobot_nav_area_t *landing;
	evobot_nav_reachability_t reachability;

	if (!source->supported || portal->kind != EVOBOT_NAV_FACE_LEDGE ||
		(adjacent->supported &&
		 adjacent->floor_height >= source->floor_height - 0.5f))
		return 1;
	if (!EvoBot_NavEvaluateAirDirection(portal, source, adjacent, physics,
		EVOBOT_NAV_DEBUG_AIR_DROP, 1, &candidate))
		return 0;
	if (!candidate.valid || !candidate.landing_area)
		return 1;
	landing = EvoBot_NavAreaById(candidate.landing_area);
	if (!landing)
		return 1;
	EvoBot_NavReachCommon(portal, source, landing, candidate.edge_index,
		&candidate.direction, &reachability);
	reachability.travel_type = landing->water_level >= 2 ?
		EVOBOT_NAV_TRAVEL_WATER_ENTRY : EVOBOT_NAV_TRAVEL_DROP;
	reachability.start.start = candidate.start_origin;
	reachability.start.end = candidate.start_origin;
	reachability.destination.start = candidate.landing_origin;
	reachability.destination.end = candidate.landing_origin;
	reachability.height_delta = candidate.landing_origin.v[2] -
		candidate.start_origin.v[2];
	reachability.horizontal_distance = candidate.horizontal_displacement;
	reachability.travel_distance = sqrtf(
		reachability.horizontal_distance * reachability.horizontal_distance +
		reachability.height_delta * reachability.height_delta);
	reachability.base_travel_time = candidate.elapsed;
	if (EvoBot_NavContentsLiquid(landing->contents))
		reachability.flags |= EVOBOT_NAV_REACH_LIQUID_LANDING;
	return EvoBot_NavAddReachability(&reachability);
}

static float EvoBot_NavAirHeadroom(const evobot_vec3_t *origin,
	int count_validation)
{
	evobot_vec3_t end = *origin;
	evobot_trace_t trace;

	end.v[2] += 128.0f;
	memset(&trace, 0, sizeof(trace));
	if (count_validation)
		evobot_nav.stats.validation_traces++;
	if (!evobot_nav_host.trace_player_world ||
		!evobot_nav_host.trace_player_world(origin, &end, &trace) ||
		trace.start_solid || trace.all_solid)
		return 0;
	return 128.0f * trace.fraction;
}

static int EvoBot_NavAirOriginClear(const evobot_vec3_t *origin,
	int count_validation)
{
	evobot_trace_t trace;

	memset(&trace, 0, sizeof(trace));
	if (count_validation)
		evobot_nav.stats.validation_traces++;
	return evobot_nav_host.trace_player_world &&
		evobot_nav_host.trace_player_world(origin, origin, &trace) &&
		!trace.start_solid && !trace.all_solid;
}

static int EvoBot_NavAirCandidateBetter(
	const evobot_nav_debug_air_candidate_t *candidate,
	const evobot_nav_debug_air_candidate_t *best)
{
	int candidate_stage;
	int best_stage;

	if (!best->present)
		return 1;
	if (candidate->valid != best->valid)
		return candidate->valid;
	if (candidate->valid)
	{
		if ((candidate->landing_area == candidate->requested_destination_area) !=
			(best->landing_area == best->requested_destination_area))
			return candidate->landing_area == candidate->requested_destination_area;
		return candidate->elapsed < best->elapsed;
	}
	candidate_stage = candidate->landed ? 4 : candidate->entered_destination ? 3 :
		candidate->airborne ? 2 : candidate->left_source ? 1 : 0;
	best_stage = best->landed ? 4 : best->entered_destination ? 3 :
		best->airborne ? 2 : best->left_source ? 1 : 0;
	if (candidate_stage != best_stage)
		return candidate_stage > best_stage;
	return candidate->move_steps > best->move_steps;
}

static int EvoBot_NavEvaluateWaterExitDirection(
	const evobot_nav_portal_t *portal, evobot_nav_area_t *source,
	evobot_nav_area_t *destination, const evobot_player_physics_t *physics,
	int count_validation, evobot_nav_debug_air_candidate_t *best)
{
	static const float edge_fractions[] = { 0.5f, 0.2f, 0.8f };
	static const float start_offsets[] = { 4.0f, 12.0f, 20.0f };
	static const float height_offsets[] = { -4.0f, -12.0f, -20.0f, -28.0f,
		-36.0f };
	static const float angle_offsets[] = { 0.0f, -15.0f, 15.0f };
	evobot_vec3_t portal_direction;
	evobot_vec3_t source_center;
	evobot_vec3_t destination_center;
	evobot_vec3_t base_direction;
	const evobot_vec3_t *a;
	const evobot_vec3_t *b;
	evobot_vec3_t edge_delta;
	float width;
	float direction_length;
	size_t edge;
	size_t fraction_count;
	size_t fraction_index;
	unsigned int attempts = 0;
	int upward_exit;

	memset(best, 0, sizeof(*best));
	best->kind = EVOBOT_NAV_DEBUG_AIR_WATER_EXIT;
	best->portal_id = portal->id;
	best->source_area = source->id;
	best->requested_destination_area = destination->id;
	if (!EvoBot_NavContentsLiquid(source->contents) || source->water_level < 2 ||
		!destination->supported || destination->water_level >= 2)
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NOT_WATER_EXIT;
		return 1;
	}
	if (!EvoBot_NavPortalDirection(portal, source->id, &portal_direction, 0))
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_DIRECTION;
		return 1;
	}
	upward_exit = portal_direction.v[2] > 0.25f;
	base_direction = portal_direction;
	base_direction.v[2] = 0;
	direction_length = EvoBot_NavLength(&base_direction);
	if (direction_length < 0.01f)
	{
		EvoBot_NavAreaCenter(source, &source_center);
		EvoBot_NavAreaCenter(destination, &destination_center);
		EvoBot_NavSubtract(&destination_center, &source_center, &base_direction);
		base_direction.v[2] = 0;
		direction_length = EvoBot_NavLength(&base_direction);
	}
	if (direction_length >= 0.01f)
	{
		base_direction.v[0] /= direction_length;
		base_direction.v[1] /= direction_length;
	}
	else
	{
		base_direction.v[0] = 1;
		base_direction.v[1] = 0;
	}
	edge = EvoBot_NavPortalLongestEdge(portal);
	a = &portal->vertices[edge];
	b = &portal->vertices[(edge + 1) % portal->vertex_count];
	EvoBot_NavSubtract(b, a, &edge_delta);
	edge_delta.v[2] = 0;
	width = EvoBot_NavLength(&edge_delta);
	fraction_count = width >= 64.0f ?
		sizeof(edge_fractions) / sizeof(edge_fractions[0]) : 1;
	for (fraction_index = 0; fraction_index < fraction_count; fraction_index++)
	{
		size_t offset_index;

		for (offset_index = 0;
			offset_index < sizeof(start_offsets) / sizeof(start_offsets[0]);
			offset_index++)
		{
			size_t height_index;

			for (height_index = 0;
				height_index < sizeof(height_offsets) / sizeof(height_offsets[0]);
				height_index++)
			{
				size_t angle_index;

				for (angle_index = 0;
					angle_index < sizeof(angle_offsets) / sizeof(angle_offsets[0]);
					angle_index++)
				{
					evobot_nav_debug_air_candidate_t current;
					evobot_player_move_state_t state;
					evobot_player_move_command_t command;
					evobot_player_move_result_t result;
					evobot_vec3_t edge_point;
					evobot_nav_area_t *landing = NULL;
					float radians = angle_offsets[angle_index] * 0.0174532925f;
					float cosine = cosf(radians);
					float sine = sinf(radians);
					int exited_water = 0;
					int used_water_jump = 0;
					int step;
					int axis;

					memset(&current, 0, sizeof(current));
					current.present = 1;
					current.kind = EVOBOT_NAV_DEBUG_AIR_WATER_EXIT;
					current.portal_id = portal->id;
					current.source_area = source->id;
					current.requested_destination_area = destination->id;
					current.edge_index = (uint32_t)edge;
					current.portal_edge.start = *a;
					current.portal_edge.end = *b;
					current.portal_width = width;
					current.source_supported = source->supported;
					current.destination_supported = destination->supported;
					current.source_floor = source->bounds.maxs.v[2];
					current.destination_floor = destination->floor_height;
					current.direction.v[0] = base_direction.v[0] * cosine -
						base_direction.v[1] * sine;
					current.direction.v[1] = base_direction.v[0] * sine +
						base_direction.v[1] * cosine;
					current.command_speed = physics->maximum_speed;
					for (axis = 0; axis < 3; axis++)
						edge_point.v[axis] = a->v[axis] +
							(b->v[axis] - a->v[axis]) *
							edge_fractions[fraction_index];
					current.start_origin = edge_point;
					current.start_origin.v[0] -= current.direction.v[0] *
						start_offsets[offset_index];
					current.start_origin.v[1] -= current.direction.v[1] *
						start_offsets[offset_index];
					current.start_origin.v[2] = edge_point.v[2] +
						height_offsets[height_index];
					attempts++;
					if (!EvoBot_NavPointInArea(source, &current.start_origin) ||
						!EvoBot_NavAirOriginClear(&current.start_origin,
							count_validation))
					{
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_START;
						if (EvoBot_NavAirCandidateBetter(&current, best))
							*best = current;
						continue;
					}
					memset(&state, 0, sizeof(state));
					state.origin = current.start_origin;
					state.water_level = source->water_level;
					state.angles.v[1] = atan2f(current.direction.v[1],
						current.direction.v[0]) * 57.2957795f;
					memset(&command, 0, sizeof(command));
					command.forward_move = physics->maximum_speed;
					command.up_move = upward_exit ? physics->maximum_speed : 0;
					command.msec = 50;
					current.trajectory[0] = state.origin;
					current.trajectory_count = 1;
					for (step = 0; step < EVOBOT_NAV_DEBUG_AIR_PATH_MAX - 1; step++)
					{
						evobot_vec3_t previous = state.origin;

						if (!EvoBot_NavMoveStepInternal(&state, &command, &result,
							count_validation))
						{
							current.rejection =
								EVOBOT_NAV_DEBUG_AIR_REJECT_PM_FAILURE;
							break;
						}
						state = result.state;
						current.move_steps++;
						current.elapsed += command.msec * 0.001f;
						if (result.blocked)
							current.blocked_steps++;
						current.trajectory[current.trajectory_count] = state.origin;
						current.trajectory_blocked[current.trajectory_count] =
							result.blocked != 0;
						current.trajectory_count++;
						if (!EvoBot_NavPointInArea(source, &state.origin))
							current.left_source = 1;
						if (state.water_jump_time > 0)
						{
							used_water_jump = 1;
							break;
						}
						if (state.water_level < 2)
						{
							if (!exited_water)
								current.launch_origin = state.origin;
							exited_water = 1;
							current.airborne = 1;
							command.up_move = 0;
						}
						if (exited_water &&
							(EvoBot_NavPointInArea(destination, &state.origin) ||
							 EvoBot_NavSegmentIntersectsArea(destination, &previous,
								&state.origin)))
							current.entered_destination = 1;
						if (exited_water && state.on_ground)
						{
							landing = EvoBot_NavRoutingAreaAt(&state.origin,
								source->id);
							current.landed = 1;
							current.landing_origin = state.origin;
							current.final_origin = state.origin;
							if (landing)
								current.landing_area = landing->id;
							current.landing_hull_clear =
								EvoBot_NavAirOriginClear(&state.origin,
									count_validation);
							break;
						}
					}
					current.attempts = attempts;
					current.height_delta = current.landing_origin.v[2] -
						current.start_origin.v[2];
					current.horizontal_displacement = sqrtf(
						(current.final_origin.v[0] - current.start_origin.v[0]) *
						(current.final_origin.v[0] - current.start_origin.v[0]) +
						(current.final_origin.v[1] - current.start_origin.v[1]) *
						(current.final_origin.v[1] - current.start_origin.v[1]));
					if (used_water_jump)
						current.rejection =
							EVOBOT_NAV_DEBUG_AIR_REJECT_NO_WATER_EXIT;
					else if (!exited_water)
						current.rejection =
							EVOBOT_NAV_DEBUG_AIR_REJECT_NO_WATER_EXIT;
					else if (!current.landed || !landing)
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_LANDING;
					else if (!current.landing_hull_clear)
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_HEADROOM;
					else if (!landing->supported || landing->water_level >= 2 ||
						(!current.entered_destination && landing != destination))
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_WRONG_LANDING;
					else
					{
						current.valid = 1;
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NONE;
					}
					if (EvoBot_NavAirCandidateBetter(&current, best))
						*best = current;
					if (current.valid)
					{
						best->attempts = attempts;
						return 1;
					}
				}
			}
		}
	}
	best->attempts = attempts;
	return 1;
}

static int EvoBot_NavEvaluateWaterJumpDirection(
	const evobot_nav_portal_t *portal, evobot_nav_area_t *source,
	evobot_nav_area_t *destination, const evobot_player_physics_t *physics,
	int count_validation, evobot_nav_debug_air_candidate_t *best)
{
	static const float edge_fractions[] = { 0.5f, 0.2f, 0.8f };
	static const float start_offsets[] = { 4.0f, 12.0f, 20.0f };
	static const float height_offsets[] = { 0.0f, -4.0f, -12.0f, -20.0f, -28.0f };
	static const float angle_offsets[] = { 0.0f, -15.0f, 15.0f };
	evobot_vec3_t portal_direction;
	evobot_vec3_t source_center;
	evobot_vec3_t destination_center;
	evobot_vec3_t base_direction;
	const evobot_vec3_t *a;
	const evobot_vec3_t *b;
	evobot_vec3_t edge_delta;
	float width;
	float direction_length;
	evobot_vec3_t portal_mins;
	evobot_vec3_t portal_maxs;
	size_t edge;
	size_t fraction_count;
	size_t fraction_index;
	unsigned int attempts = 0;

	memset(best, 0, sizeof(*best));
	best->kind = EVOBOT_NAV_DEBUG_AIR_WATER_JUMP;
	best->portal_id = portal->id;
	best->source_area = source->id;
	best->requested_destination_area = destination->id;
	if (!EvoBot_NavContentsLiquid(source->contents) || source->water_level < 2 ||
		!destination->supported || destination->water_level >= 2)
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NOT_WATER_EXIT;
		return 1;
	}
	if (!EvoBot_NavPortalDirection(portal, source->id, &portal_direction, 0))
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_DIRECTION;
		return 1;
	}
	base_direction = portal_direction;
	base_direction.v[2] = 0;
	direction_length = EvoBot_NavLength(&base_direction);
	if (direction_length < 0.01f)
	{
		EvoBot_NavAreaCenter(source, &source_center);
		EvoBot_NavAreaCenter(destination, &destination_center);
		EvoBot_NavSubtract(&destination_center, &source_center, &base_direction);
		base_direction.v[2] = 0;
		direction_length = EvoBot_NavLength(&base_direction);
	}
	if (direction_length < 0.01f)
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_DIRECTION;
		return 1;
	}
	base_direction.v[0] /= direction_length;
	base_direction.v[1] /= direction_length;
	edge = EvoBot_NavPortalLongestEdge(portal);
	portal_mins = portal->vertices[0];
	portal_maxs = portal->vertices[0];
	for (fraction_index = 1; fraction_index < portal->vertex_count; fraction_index++)
	{
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			if (portal->vertices[fraction_index].v[axis] < portal_mins.v[axis])
				portal_mins.v[axis] = portal->vertices[fraction_index].v[axis];
			if (portal->vertices[fraction_index].v[axis] > portal_maxs.v[axis])
				portal_maxs.v[axis] = portal->vertices[fraction_index].v[axis];
		}
	}
	a = &portal->vertices[edge];
	b = &portal->vertices[(edge + 1) % portal->vertex_count];
	EvoBot_NavSubtract(b, a, &edge_delta);
	edge_delta.v[2] = 0;
	width = EvoBot_NavLength(&edge_delta);
	fraction_count = width >= 64.0f ?
		sizeof(edge_fractions) / sizeof(edge_fractions[0]) : 1;
	for (fraction_index = 0; fraction_index < fraction_count; fraction_index++)
	{
		size_t offset_index;

		for (offset_index = 0;
			offset_index < sizeof(start_offsets) / sizeof(start_offsets[0]);
			offset_index++)
		{
			size_t height_index;

			for (height_index = 0;
				height_index < sizeof(height_offsets) / sizeof(height_offsets[0]);
				height_index++)
			{
				size_t angle_index;

				for (angle_index = 0;
					angle_index < sizeof(angle_offsets) / sizeof(angle_offsets[0]);
					angle_index++)
				{
					evobot_nav_debug_air_candidate_t current;
					evobot_player_move_state_t state;
					evobot_player_move_command_t command;
					evobot_player_move_result_t result;
					evobot_vec3_t edge_point;
					evobot_nav_area_t *landing = NULL;
					float radians = angle_offsets[angle_index] * 0.0174532925f;
					float cosine = cosf(radians);
					float sine = sinf(radians);
					float rise = 0;
					int activated = 0;
					int exited_water = 0;
					int step;
					int axis;

					memset(&current, 0, sizeof(current));
					current.present = 1;
					current.kind = EVOBOT_NAV_DEBUG_AIR_WATER_JUMP;
					current.portal_id = portal->id;
					current.source_area = source->id;
					current.requested_destination_area = destination->id;
					current.edge_index = (uint32_t)edge;
					current.portal_edge.start = *a;
					current.portal_edge.end = *b;
					current.portal_width = width;
					current.source_supported = source->supported;
					current.destination_supported = destination->supported;
					current.source_floor = source->bounds.maxs.v[2];
					current.destination_floor = destination->floor_height;
					current.direction.v[0] = base_direction.v[0] * cosine -
						base_direction.v[1] * sine;
					current.direction.v[1] = base_direction.v[0] * sine +
						base_direction.v[1] * cosine;
					current.command_speed = physics->maximum_speed;
					for (axis = 0; axis < 3; axis++)
						edge_point.v[axis] = a->v[axis] +
							(b->v[axis] - a->v[axis]) *
							edge_fractions[fraction_index];
					current.start_origin = edge_point;
					current.start_origin.v[0] -= current.direction.v[0] *
						start_offsets[offset_index];
					current.start_origin.v[1] -= current.direction.v[1] *
						start_offsets[offset_index];
					current.start_origin.v[2] = source->bounds.maxs.v[2] +
						height_offsets[height_index];
					attempts++;
					if (!EvoBot_NavPointInArea(source, &current.start_origin) ||
						!EvoBot_NavAirOriginClear(&current.start_origin,
							count_validation))
					{
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_START;
						if (EvoBot_NavAirCandidateBetter(&current, best))
							*best = current;
						continue;
					}
					memset(&state, 0, sizeof(state));
					state.origin = current.start_origin;
					state.water_level = source->water_level;
					state.angles.v[1] = atan2f(current.direction.v[1],
						current.direction.v[0]) * 57.2957795f;
					memset(&command, 0, sizeof(command));
					command.forward_move = physics->maximum_speed;
					command.msec = 50;
					current.trajectory[0] = state.origin;
					current.trajectory_count = 1;
					for (step = 0; step < EVOBOT_NAV_DEBUG_AIR_PATH_MAX - 1; step++)
					{
						evobot_vec3_t previous = state.origin;

						if (!EvoBot_NavMoveStepInternal(&state, &command, &result,
							count_validation))
						{
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_PM_FAILURE;
							break;
						}
						state = result.state;
						current.move_steps++;
						current.elapsed += command.msec * 0.001f;
						if (result.blocked)
							current.blocked_steps++;
						current.trajectory[current.trajectory_count] = state.origin;
						current.trajectory_blocked[current.trajectory_count] =
							result.blocked != 0;
						current.trajectory_count++;
						if (!EvoBot_NavPointInArea(source, &state.origin))
							current.left_source = 1;
						if (activated && (EvoBot_NavPointInArea(destination, &state.origin) ||
							EvoBot_NavSegmentIntersectsArea(destination, &previous,
								&state.origin)))
							current.entered_destination = 1;
						if (!activated && state.water_jump_time > 0)
						{
							activated = 1;
							current.airborne = 1;
							current.launch_origin = state.origin;
							command.forward_move = 0;
						}
						if (activated && state.water_level < 2)
							exited_water = 1;
						rise = state.origin.v[2] - current.start_origin.v[2];
						if (rise > current.maximum_rise)
							current.maximum_rise = rise;
						if (activated && exited_water && state.on_ground)
						{
							landing = EvoBot_NavRoutingAreaAt(&state.origin,
								source->id);
							current.landed = 1;
							current.landing_origin = state.origin;
							current.final_origin = state.origin;
							if (landing)
								current.landing_area = landing->id;
							current.landing_hull_clear =
								EvoBot_NavAirOriginClear(&state.origin,
									count_validation);
							break;
						}
					}
					current.attempts = attempts;
					current.height_delta = current.landing_origin.v[2] -
						current.start_origin.v[2];
					current.horizontal_displacement = sqrtf(
						(current.final_origin.v[0] - current.start_origin.v[0]) *
						(current.final_origin.v[0] - current.start_origin.v[0]) +
						(current.final_origin.v[1] - current.start_origin.v[1]) *
						(current.final_origin.v[1] - current.start_origin.v[1]));
					if (!activated)
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_WATER_JUMP;
					else if (current.launch_origin.v[0] < portal_mins.v[0] - 32.0f ||
						current.launch_origin.v[0] > portal_maxs.v[0] + 32.0f ||
						current.launch_origin.v[1] < portal_mins.v[1] - 32.0f ||
						current.launch_origin.v[1] > portal_maxs.v[1] + 32.0f ||
						current.launch_origin.v[2] < portal_mins.v[2] - 48.0f ||
						current.launch_origin.v[2] > portal_maxs.v[2] + 48.0f)
						current.rejection =
							EVOBOT_NAV_DEBUG_AIR_REJECT_WATER_JUMP_OFF_PORTAL;
					else if (!exited_water)
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_WATER_EXIT;
					else if (!current.landed || !landing)
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_LANDING;
					else if (!current.landing_hull_clear)
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_HEADROOM;
					else if (!landing->supported || landing->water_level >= 2 ||
						(!current.entered_destination && landing != destination))
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_WRONG_LANDING;
					else
					{
						current.valid = 1;
						current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NONE;
					}
					if (EvoBot_NavAirCandidateBetter(&current, best))
						*best = current;
					if (current.valid)
					{
						best->attempts = attempts;
						return 1;
					}
				}
			}
		}
	}
	best->attempts = attempts;
	return 1;
}

static int EvoBot_NavEvaluateAirDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics, evobot_nav_debug_air_kind_t kind,
	int count_validation, evobot_nav_debug_air_candidate_t *best)
{
	static const float edge_fractions[] = { 0.5f, 0.2f, 0.8f };
	static const float drop_start_offsets[] = { 8.0f };
	static const float jump_start_offsets[] = { 8.0f, 24.0f, 48.0f };
	static const float drop_speed_factors[] = { 1.0f, 0.75f, 0.5f, 0.25f };
	static const float jump_speed_factors[] = { 1.0f, 0.75f, 0.5f, 0.25f };
	static const unsigned int drop_approach_frames[] = { 0 };
	static const unsigned int jump_approach_frames[] = {
		0, 1, 2, 4, 0, 4, 8, 12, 16
	};
	static const unsigned int jump_approach_msecs[] = {
		50, 50, 50, 50, 13, 13, 13, 13, 13
	};
	evobot_vec3_t direction;
	unsigned int attempts = 0;
	size_t horizontal_edge_count = 0;
	size_t edge;

	memset(best, 0, sizeof(*best));
	best->kind = kind;
	best->portal_id = portal->id;
	best->source_area = source->id;
	best->requested_destination_area = destination->id;
	best->source_supported = source->supported;
	best->destination_supported = destination->supported;
	best->source_floor = source->floor_height;
	best->destination_floor = destination->floor_height;
	best->height_delta = destination->floor_height - source->floor_height;
	if (portal->kind != EVOBOT_NAV_FACE_LEDGE)
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NOT_LEDGE;
		return 1;
	}
	if (!source->supported)
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_UNSUPPORTED_SOURCE;
		return 1;
	}
	if (!EvoBot_NavWalkDirection(portal, source, destination, &direction, NULL))
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_DIRECTION;
		return 1;
	}
	best->direction = direction;
	if ((kind == EVOBOT_NAV_DEBUG_AIR_DROP && destination->supported &&
		 best->height_delta >= -0.5f) ||
		(kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP &&
		 (!destination->supported || best->height_delta <= physics->step_height)))
	{
		best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_HEIGHT;
		return 1;
	}
	for (edge = 0; edge < portal->vertex_count; edge++)
	{
		evobot_vec3_t edge_delta;

		EvoBot_NavSubtract(&portal->vertices[(edge + 1) % portal->vertex_count],
			&portal->vertices[edge], &edge_delta);
		edge_delta.v[2] = 0;
		if (EvoBot_NavLength(&edge_delta) >= 4.0f)
			horizontal_edge_count++;
	}
	best->rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_EDGE;
	for (edge = 0; edge < (horizontal_edge_count ? portal->vertex_count : 1); edge++)
	{
		evobot_vec3_t point_a;
		evobot_vec3_t point_b;
		const evobot_vec3_t *a;
		const evobot_vec3_t *b;
		evobot_vec3_t edge_delta;
		float width;
		const float *start_offsets;
		const float *speed_factors;
		const unsigned int *approach_frames;
		size_t start_offset_count;
		size_t speed_factor_count;
		size_t approach_count;
		size_t fraction_index;
		size_t fraction_count;

		if (horizontal_edge_count)
		{
			a = &portal->vertices[edge];
			b = &portal->vertices[(edge + 1) % portal->vertex_count];
		}
		else
		{
			EvoBot_NavPortalCenter(portal, &point_a);
			point_b = point_a;
			a = &point_a;
			b = &point_b;
		}
		EvoBot_NavSubtract(b, a, &edge_delta);
		edge_delta.v[2] = 0;
		width = EvoBot_NavLength(&edge_delta);
		if (horizontal_edge_count && width < 4.0f)
			continue;
		fraction_count = kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP && width >= 64.0f ?
			sizeof(edge_fractions) / sizeof(edge_fractions[0]) : 1;
		if (kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP)
		{
			start_offsets = jump_start_offsets;
			start_offset_count = sizeof(jump_start_offsets) /
				sizeof(jump_start_offsets[0]);
			speed_factors = jump_speed_factors;
			speed_factor_count = sizeof(jump_speed_factors) /
				sizeof(jump_speed_factors[0]);
			approach_frames = jump_approach_frames;
			approach_count = sizeof(jump_approach_frames) /
				sizeof(jump_approach_frames[0]);
		}
		else
		{
			start_offsets = drop_start_offsets;
			start_offset_count = sizeof(drop_start_offsets) /
				sizeof(drop_start_offsets[0]);
			speed_factors = drop_speed_factors;
			speed_factor_count = sizeof(drop_speed_factors) /
				sizeof(drop_speed_factors[0]);
			approach_frames = drop_approach_frames;
			approach_count = sizeof(drop_approach_frames) /
				sizeof(drop_approach_frames[0]);
		}
		for (fraction_index = 0;
			fraction_index < fraction_count;
			fraction_index++)
		{
			size_t offset_index;

			for (offset_index = 0;
				offset_index < start_offset_count;
				offset_index++)
			{
				size_t speed_index;

				for (speed_index = 0;
					speed_index < speed_factor_count;
					speed_index++)
				{
					size_t approach_index;

					for (approach_index = 0; approach_index < approach_count;
						approach_index++)
					{
						unsigned int approach = approach_frames[approach_index];
						evobot_nav_debug_air_candidate_t current;
						evobot_vec3_t edge_point;
						evobot_vec3_t start_sample;
						evobot_player_move_state_t state;
						evobot_player_move_command_t command;
						evobot_player_move_result_t result;
						evobot_nav_area_t *landing = NULL;
						float start_z;
						int step;
						int axis;

						memset(&current, 0, sizeof(current));
						current.present = 1;
						current.kind = kind;
						current.portal_id = portal->id;
						current.source_area = source->id;
						current.requested_destination_area = destination->id;
						current.edge_index = (uint32_t)edge;
						current.direction = direction;
						current.portal_edge.start = *a;
						current.portal_edge.end = *b;
						current.portal_width = width;
						current.source_supported = source->supported;
						current.destination_supported = destination->supported;
						current.source_floor = source->floor_height;
						current.destination_floor = destination->floor_height;
						current.height_delta = destination->floor_height -
							source->floor_height;
						current.command_speed = physics->maximum_speed *
							speed_factors[speed_index];
						current.approach_frames = approach;
						for (axis = 0; axis < 3; axis++)
						{
							edge_point.v[axis] = a->v[axis] +
								(b->v[axis] - a->v[axis]) *
								edge_fractions[fraction_index];
							start_sample.v[axis] = edge_point.v[axis] -
									direction.v[axis] * start_offsets[offset_index];
						}
						start_sample.v[2] = source->floor_height -
							evobot_nav.player_bounds.mins.v[2];
						attempts++;
						if (!EvoBot_NavSupportOriginInternal(&start_sample, physics,
							&current.start_origin, NULL, count_validation) ||
							!EvoBot_NavPointInArea(source, &current.start_origin))
						{
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_START;
							if (EvoBot_NavAirCandidateBetter(&current, best))
								*best = current;
							continue;
						}
						current.headroom = EvoBot_NavAirHeadroom(&current.start_origin,
							count_validation);
						current.trajectory[0] = current.start_origin;
						current.trajectory_count = 1;
						memset(&state, 0, sizeof(state));
						state.origin = current.start_origin;
						state.on_ground = 1;
						state.angles.v[1] = atan2f(direction.v[1], direction.v[0]) *
							57.2957795f;
						memset(&command, 0, sizeof(command));
						command.forward_move = current.command_speed;
						/* QW jump height is sensitive to user-command duration.  Preserve
						 * valid 50 ms transitions and also test a representative 13 ms client
						 * cadence, which does not lose several units from the natural apex.
						 * Drops retain the cheaper coarse cadence. */
						command.msec = kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP ?
							jump_approach_msecs[approach_index] : 50;
						start_z = state.origin.v[2];
						for (step = 0; step < (kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP ?
							(count_validation ? 120 : 240) :
							(count_validation ? 40 : 80)); step++)
						{
							command.buttons = kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP &&
								(unsigned int)step == approach ?
								EVOBOT_PLAYER_BUTTON_JUMP : 0;
							if (!EvoBot_NavMoveStepInternal(&state, &command, &result,
								count_validation))
							{
								current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_PM_FAILURE;
								break;
							}
							state = result.state;
							current.move_steps++;
							current.elapsed += command.msec * 0.001f;
							if (result.blocked)
								current.blocked_steps++;
							if (current.trajectory_count < EVOBOT_NAV_DEBUG_AIR_PATH_MAX)
							{
								current.trajectory[current.trajectory_count] = state.origin;
								current.trajectory_blocked[current.trajectory_count] =
									result.blocked != 0;
								current.trajectory_count++;
							}
							if (state.origin.v[2] - start_z > current.maximum_rise)
								current.maximum_rise = state.origin.v[2] - start_z;
							if (!EvoBot_NavPointInArea(source, &state.origin))
								current.left_source = 1;
							if (EvoBot_NavPointInArea(destination, &state.origin))
								current.entered_destination = 1;
							if (!state.on_ground)
							{
								if (!current.airborne)
									current.launch_origin = state.origin;
								current.airborne = 1;
							}
							if (current.left_source && state.water_level >= 2)
								landing = EvoBot_NavRoutingAreaAt(&state.origin, source->id);
							else if (current.left_source && state.on_ground &&
								(kind == EVOBOT_NAV_DEBUG_AIR_DROP || current.airborne))
								landing = EvoBot_NavRoutingAreaAt(&state.origin, source->id);
							if (landing)
							{
								current.landed = 1;
								current.landing_area = landing->id;
								current.landing_origin = state.origin;
								current.landing_hull_clear =
									EvoBot_NavAirOriginClear(&state.origin,
										count_validation);
								current.liquid_landing =
									EvoBot_NavContentsLiquid(landing->contents);
								break;
							}
							if (kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP &&
								(unsigned int)step < approach && current.left_source)
								break;
						}
						current.final_origin = state.origin;
						current.horizontal_displacement = sqrtf(
							(state.origin.v[0] - current.start_origin.v[0]) *
							(state.origin.v[0] - current.start_origin.v[0]) +
							(state.origin.v[1] - current.start_origin.v[1]) *
							(state.origin.v[1] - current.start_origin.v[1]));
						if (current.landed && current.landing_hull_clear &&
							(kind == EVOBOT_NAV_DEBUG_AIR_DROP ?
							 (!destination->supported ||
							  current.landing_area == destination->id) :
							 current.landing_area == destination->id) &&
							(kind == EVOBOT_NAV_DEBUG_AIR_DROP ?
							 current.landing_origin.v[2] < current.start_origin.v[2] - 0.5f :
							 current.airborne))
						{
							current.valid = 1;
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NONE;
						}
						else if (kind == EVOBOT_NAV_DEBUG_AIR_JUMP_UP && !current.airborne)
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_AIRBORNE;
						else if (!current.landed)
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_LANDING;
						else if (!current.landing_hull_clear)
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_NO_HEADROOM;
						else
							current.rejection = EVOBOT_NAV_DEBUG_AIR_REJECT_WRONG_LANDING;
						if (EvoBot_NavAirCandidateBetter(&current, best))
							*best = current;
					}
				}
			}
		}
	}
	best->attempts = attempts;
	return 1;
}

static int EvoBot_NavGenerateJumpDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *destination,
	const evobot_player_physics_t *physics)
{
	evobot_nav_debug_air_candidate_t candidate;
	evobot_nav_reachability_t reachability;
	float area_height_delta;
	size_t before;

	area_height_delta = destination->floor_height - source->floor_height;
	if (!source->supported || !destination->supported ||
		source->water_level >= 2 || destination->water_level >= 2 ||
		portal->kind != EVOBOT_NAV_FACE_LEDGE ||
		area_height_delta <= physics->step_height)
		return 1;
	evobot_nav.stats.jump_up_ledge_candidates++;
	if (!EvoBot_NavEvaluateAirDirection(portal, source, destination, physics,
		EVOBOT_NAV_DEBUG_AIR_JUMP_UP, 1, &candidate))
		return 0;
	evobot_nav.stats.jump_up_ledge_attempts += candidate.attempts;
	if (!candidate.valid || candidate.landing_area != destination->id)
		return 1;
	evobot_nav.stats.jump_up_ledge_validated++;
	EvoBot_NavReachCommon(portal, source, destination, candidate.edge_index,
		&candidate.direction, &reachability);
	reachability.travel_type = EVOBOT_NAV_TRAVEL_JUMP;
	reachability.start.start = candidate.start_origin;
	reachability.start.end = candidate.start_origin;
	reachability.destination.start = candidate.landing_origin;
	reachability.destination.end = candidate.landing_origin;
	reachability.height_delta = candidate.landing_origin.v[2] -
		candidate.start_origin.v[2];
	reachability.horizontal_distance = candidate.horizontal_displacement;
	reachability.travel_distance = sqrtf(
		reachability.horizontal_distance * reachability.horizontal_distance +
		reachability.height_delta * reachability.height_delta);
	reachability.base_travel_time = candidate.elapsed;
	if (reachability.height_delta > 0.5f)
		reachability.flags |= EVOBOT_NAV_REACH_STEP_UP;
	else if (reachability.height_delta < -0.5f)
		reachability.flags |= EVOBOT_NAV_REACH_STEP_DOWN;
	before = evobot_nav.reachability_count;
	if (!EvoBot_NavAddReachability(&reachability))
		return 0;
	if (before != evobot_nav.reachability_count)
		evobot_nav.stats.jump_up_ledge_links++;
	return 1;
}

static int EvoBot_NavGenerateJumpUpLedges(
	const evobot_player_physics_t *physics)
{
	double started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	size_t i;

	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		evobot_nav_portal_t *portal = &evobot_nav.portals[i];
		evobot_nav_area_t *a = EvoBot_NavAreaById(portal->area_a);
		evobot_nav_area_t *b = EvoBot_NavAreaById(portal->area_b);

		if (!a || !b ||
			!EvoBot_NavGenerateJumpDirection(portal, a, b, physics) ||
			!EvoBot_NavGenerateJumpDirection(portal, b, a, physics))
			return 0;
	}
	evobot_nav.stats.jump_up_ledge_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	return 1;
}

static int EvoBot_NavGenerateEdgeDropDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *adjacent,
	const evobot_player_physics_t *physics)
{
	static const float speed_factors[] = { 0.25f, 0.5f };
	evobot_vec3_t direction;
	size_t edge;

	if (!source->supported || source->water_level >= 2 ||
		portal->kind != EVOBOT_NAV_FACE_LEDGE ||
		!EvoBot_NavWalkDirection(portal, source, adjacent, &direction, NULL))
		return 1;
	for (edge = 0; edge < portal->vertex_count; edge++)
	{
		const evobot_vec3_t *a = &portal->vertices[edge];
		const evobot_vec3_t *b =
			&portal->vertices[(edge + 1) % portal->vertex_count];
		evobot_vec3_t delta;
		float fractions[3];
		float width;
		size_t fraction_index;

		EvoBot_NavSubtract(b, a, &delta);
		delta.v[2] = 0;
		width = EvoBot_NavLength(&delta);
		if (width < 8.0f)
			continue;
		fractions[0] = width > 24.0f ? 8.0f / width : 0.25f;
		fractions[1] = 0.5f;
		fractions[2] = 1.0f - fractions[0];
		for (fraction_index = 0; fraction_index < 3; fraction_index++)
		{
			size_t speed_index;

			for (speed_index = 0; speed_index < 2; speed_index++)
			{
				evobot_vec3_t sample;
				evobot_vec3_t start;
				evobot_player_move_state_t state;
				evobot_player_move_command_t command;
				evobot_player_move_result_t result;
				evobot_nav_area_t *landing = NULL;
				int step;
				int axis;

				for (axis = 0; axis < 3; axis++)
					sample.v[axis] = a->v[axis] +
						(b->v[axis] - a->v[axis]) * fractions[fraction_index] -
						direction.v[axis] * 12.0f;
				sample.v[2] = source->floor_height -
					evobot_nav.player_bounds.mins.v[2];
				evobot_nav.stats.edge_drop_seeds++;
				if (!EvoBot_NavSupportOriginInternal(&sample, physics, &start, NULL, 1) ||
					!EvoBot_NavPointInArea(source, &start))
					continue;
				memset(&state, 0, sizeof(state));
				state.origin = start;
				state.on_ground = 1;
				state.angles.v[1] = atan2f(direction.v[1], direction.v[0]) *
					57.2957795f;
				memset(&command, 0, sizeof(command));
				command.forward_move = physics->maximum_speed *
					speed_factors[speed_index];
				command.msec = 50;
				for (step = 0; step < 80; step++)
				{
					if (!EvoBot_NavMoveStepInternal(&state, &command, &result, 1))
						break;
					state = result.state;
					if (!EvoBot_NavPointInArea(source, &state.origin) &&
						(state.on_ground || state.water_level >= 2))
					{
						landing = EvoBot_NavRoutingAreaAt(&state.origin, source->id);
						if (landing)
							break;
					}
				}
				if (landing && state.origin.v[2] < start.v[2] - 0.5f)
				{
					evobot_nav_reachability_t reachability;
					size_t before = evobot_nav.reachability_count;

					EvoBot_NavReachCommon(portal, source, landing, edge, &direction,
						&reachability);
					reachability.travel_type = landing->water_level >= 2 ?
						EVOBOT_NAV_TRAVEL_WATER_ENTRY : EVOBOT_NAV_TRAVEL_DROP;
					reachability.start.start = start;
					reachability.start.end = start;
					reachability.destination.start = state.origin;
					reachability.destination.end = state.origin;
					reachability.height_delta = state.origin.v[2] - start.v[2];
					reachability.horizontal_distance = sqrtf(
						(state.origin.v[0] - start.v[0]) *
						(state.origin.v[0] - start.v[0]) +
						(state.origin.v[1] - start.v[1]) *
						(state.origin.v[1] - start.v[1]));
					reachability.travel_distance = sqrtf(
						reachability.horizontal_distance *
						reachability.horizontal_distance +
						reachability.height_delta * reachability.height_delta);
					reachability.base_travel_time = step * 0.05f;
					if (EvoBot_NavContentsLiquid(landing->contents))
						reachability.flags |= EVOBOT_NAV_REACH_LIQUID_LANDING;
					if (!EvoBot_NavAddReachability(&reachability))
						return 0;
					if (evobot_nav.reachability_count != before)
						evobot_nav.stats.edge_drop_links++;
				}
			}
		}
	}
	return 1;
}

static int EvoBot_NavGenerateEdgeDrops(const evobot_player_physics_t *physics)
{
	double started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	size_t i;

	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		evobot_nav_portal_t *portal = &evobot_nav.portals[i];
		evobot_nav_area_t *a = EvoBot_NavAreaById(portal->area_a);
		evobot_nav_area_t *b = EvoBot_NavAreaById(portal->area_b);

		if (!a || !b || !EvoBot_NavGenerateEdgeDropDirection(portal, a, b, physics) ||
			!EvoBot_NavGenerateEdgeDropDirection(portal, b, a, physics))
			return 0;
	}
	evobot_nav.stats.edge_drop_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	return 1;
}

static float EvoBot_NavHorizontalEdgeDistance(const evobot_nav_area_t *area,
	const evobot_vec3_t *point)
{
	float distance = fabsf(point->v[0] - area->bounds.mins.v[0]);
	float value = fabsf(point->v[0] - area->bounds.maxs.v[0]);

	if (value < distance) distance = value;
	value = fabsf(point->v[1] - area->bounds.mins.v[1]);
	if (value < distance) distance = value;
	value = fabsf(point->v[1] - area->bounds.maxs.v[1]);
	if (value < distance) distance = value;
	return distance;
}

static int EvoBot_NavGenerateGapJumpDirection(const evobot_nav_portal_t *portal,
	evobot_nav_area_t *source, evobot_nav_area_t *adjacent,
	const evobot_player_physics_t *physics)
{
	static const float speed_factors[] = { 1.0f, 0.8f };
	static const float steer_angles[] = { -12.0f, 0.0f, 12.0f };
	static const unsigned int command_msecs[] = { 50, 13 };
	static const unsigned int jump_frames[] = { 3, 12 };
	evobot_vec3_t outward;
	size_t edge;

	if (!source->supported || source->water_level >= 2 ||
		portal->kind != EVOBOT_NAV_FACE_LEDGE ||
		!EvoBot_NavWalkDirection(portal, source, adjacent, &outward, NULL))
		return 1;
	for (edge = 0; edge < portal->vertex_count; edge++)
	{
		const evobot_vec3_t *a = &portal->vertices[edge];
		const evobot_vec3_t *b =
			&portal->vertices[(edge + 1) % portal->vertex_count];
		evobot_vec3_t delta;
		float fractions[3];
		float width;
		size_t fraction_index;

		EvoBot_NavSubtract(b, a, &delta);
		delta.v[2] = 0;
		width = EvoBot_NavLength(&delta);
		if (width < 12.0f)
			continue;
		fractions[0] = width > 32.0f ? 10.0f / width : 0.25f;
		fractions[1] = 0.5f;
		fractions[2] = 1.0f - fractions[0];
		for (fraction_index = 0; fraction_index < 3; fraction_index++)
		{
			size_t speed_index;

			for (speed_index = 0; speed_index < 2; speed_index++)
			{
				size_t steer_index;

				for (steer_index = 0; steer_index < 3; steer_index++)
				{
					size_t msec_index;

					for (msec_index = 0; msec_index < 2; msec_index++)
					{
					evobot_vec3_t sample;
					evobot_vec3_t start;
					evobot_player_move_state_t state;
					evobot_player_move_command_t command;
					evobot_player_move_result_t result;
					evobot_nav_area_t *landing = NULL;
					float base_yaw = atan2f(outward.v[1], outward.v[0]) *
						57.2957795f;
					int airborne = 0;
					int step;
					int axis;

					for (axis = 0; axis < 3; axis++)
						sample.v[axis] = a->v[axis] +
							(b->v[axis] - a->v[axis]) * fractions[fraction_index] -
							outward.v[axis] * 40.0f;
					sample.v[2] = source->floor_height -
						evobot_nav.player_bounds.mins.v[2];
					evobot_nav.stats.gap_jump_seeds++;
					if (!EvoBot_NavSupportOriginInternal(&sample, physics, &start, NULL, 1) ||
						!EvoBot_NavPointInArea(source, &start))
					{
						evobot_nav.stats.gap_jump_rejected_start++;
						continue;
					}
					memset(&state, 0, sizeof(state));
					state.origin = start;
					state.on_ground = 1;
					memset(&command, 0, sizeof(command));
					command.forward_move = physics->maximum_speed *
						speed_factors[speed_index];
					command.msec = command_msecs[msec_index];
					for (step = 0; step < (msec_index ? 160 : 50); step++)
					{
						float steering = steer_angles[steer_index] *
							(airborne ? 0.35f : 1.0f);

						state.angles.v[1] = base_yaw + steering;
						command.buttons = (unsigned int)step == jump_frames[msec_index] ?
							EVOBOT_PLAYER_BUTTON_JUMP : 0;
						if (!EvoBot_NavMoveStepInternal(&state, &command, &result, 1))
							break;
						state = result.state;
						if (!state.on_ground)
							airborne = 1;
						if (airborne && !EvoBot_NavPointInArea(source, &state.origin) &&
							state.on_ground)
						{
							landing = EvoBot_NavRoutingAreaAt(&state.origin, source->id);
							if (landing)
								break;
						}
					}
					if (!airborne)
					{
						evobot_nav.stats.gap_jump_rejected_air++;
						continue;
					}
					if (!landing || landing == adjacent || !landing->supported ||
						landing->water_level >= 2)
					{
						evobot_nav.stats.gap_jump_rejected_landing++;
						continue;
					}
					{
						float horizontal = sqrtf(
							(state.origin.v[0] - start.v[0]) *
							(state.origin.v[0] - start.v[0]) +
							(state.origin.v[1] - start.v[1]) *
							(state.origin.v[1] - start.v[1]));

						if (horizontal < 32.0f ||
							EvoBot_NavHorizontalEdgeDistance(landing, &state.origin) > 64.0f)
						{
							evobot_nav.stats.gap_jump_rejected_edge++;
							continue;
						}
						{
							evobot_nav_reachability_t reachability;
							size_t before = evobot_nav.reachability_count;

							EvoBot_NavReachCommon(portal, source, landing, edge, &outward,
								&reachability);
							reachability.travel_type = EVOBOT_NAV_TRAVEL_JUMP;
							reachability.start.start = start;
							reachability.start.end = start;
							reachability.destination.start = state.origin;
							reachability.destination.end = state.origin;
							reachability.height_delta = state.origin.v[2] - start.v[2];
							reachability.horizontal_distance = horizontal;
							reachability.travel_distance = sqrtf(horizontal * horizontal +
								reachability.height_delta * reachability.height_delta);
							reachability.base_travel_time = step *
								command.msec * 0.001f;
							if (reachability.height_delta > 0.5f)
								reachability.flags |= EVOBOT_NAV_REACH_STEP_UP;
							else if (reachability.height_delta < -0.5f)
								reachability.flags |= EVOBOT_NAV_REACH_STEP_DOWN;
							if (!EvoBot_NavAddReachability(&reachability))
								return 0;
							if (evobot_nav.reachability_count != before)
								evobot_nav.stats.gap_jump_links++;
						}
					}
					}
				}
			}
		}
	}
	return 1;
}

static int EvoBot_NavGenerateGapJumps(const evobot_player_physics_t *physics)
{
	double started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	size_t i;

	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		evobot_nav_portal_t *portal = &evobot_nav.portals[i];
		evobot_nav_area_t *a = EvoBot_NavAreaById(portal->area_a);
		evobot_nav_area_t *b = EvoBot_NavAreaById(portal->area_b);

		if (!a || !b || !EvoBot_NavGenerateGapJumpDirection(portal, a, b, physics) ||
			!EvoBot_NavGenerateGapJumpDirection(portal, b, a, physics))
			return 0;
	}
	evobot_nav.stats.gap_jump_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	return 1;
}

static void EvoBot_NavPlatformAccessRegion(const evobot_bounds_t *platform,
	evobot_bounds_t *region)
{
	int axis;

	for (axis = 0; axis < 2; axis++)
	{
		region->mins.v[axis] = platform->mins.v[axis] -
			evobot_nav.player_bounds.maxs.v[axis] - 8.0f;
		region->maxs.v[axis] = platform->maxs.v[axis] -
			evobot_nav.player_bounds.mins.v[axis] + 8.0f;
	}
	region->mins.v[2] = platform->maxs.v[2] -
		evobot_nav.player_bounds.mins.v[2] - 4.0f;
	region->maxs.v[2] = region->mins.v[2] + 8.0f;
}

static int EvoBot_NavPlatformAreaPoint(const evobot_nav_area_t *area,
	const evobot_bounds_t *region, evobot_vec3_t *point)
{
	int axis;

	if (!EvoBot_NavRoutingArea(area) ||
		!EvoBot_NavBoundsIntersect(&area->bounds, region))
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		float minimum = area->bounds.mins.v[axis] > region->mins.v[axis] ?
			area->bounds.mins.v[axis] : region->mins.v[axis];
		float maximum = area->bounds.maxs.v[axis] < region->maxs.v[axis] ?
			area->bounds.maxs.v[axis] : region->maxs.v[axis];

		point->v[axis] = (minimum + maximum) * 0.5f;
	}
	return EvoBot_NavPointInside(area, point, 1.5f);
}

static int EvoBot_NavAddPlatformDirection(
	const evobot_nav_interactor_t *platform,
	const evobot_bounds_t *board, const evobot_bounds_t *exit)
{
	evobot_bounds_t board_region;
	evobot_bounds_t exit_region;
	size_t source_index;

	EvoBot_NavPlatformAccessRegion(board, &board_region);
	EvoBot_NavPlatformAccessRegion(exit, &exit_region);
	for (source_index = 0; source_index < evobot_nav.area_count; source_index++)
	{
		evobot_nav_area_t *source = &evobot_nav.areas[source_index];
		evobot_vec3_t start;
		size_t destination_index;

		if (!EvoBot_NavPlatformAreaPoint(source, &board_region, &start))
			continue;
		for (destination_index = 0;
			destination_index < evobot_nav.area_count; destination_index++)
		{
			evobot_nav_area_t *destination =
				&evobot_nav.areas[destination_index];
			evobot_vec3_t end;
			evobot_nav_reachability_t reachability;
			size_t before;

			if (source == destination ||
				!EvoBot_NavPlatformAreaPoint(destination, &exit_region, &end))
				continue;
			memset(&reachability, 0, sizeof(reachability));
			reachability.source_area = source->id;
			reachability.destination_area = destination->id;
			reachability.travel_type = EVOBOT_NAV_TRAVEL_PLATFORM;
			reachability.source_face = EVOBOT_NAV_REACH_INVALID_INDEX;
			reachability.source_edge = EVOBOT_NAV_REACH_INVALID_INDEX;
			reachability.start.start = start;
			reachability.start.end = start;
			reachability.destination.start = end;
			reachability.destination.end = end;
			reachability.height_delta = end.v[2] - start.v[2];
			reachability.horizontal_distance = sqrtf(
				(end.v[0] - start.v[0]) * (end.v[0] - start.v[0]) +
				(end.v[1] - start.v[1]) * (end.v[1] - start.v[1]));
			reachability.travel_distance = sqrtf(
				reachability.horizontal_distance *
				reachability.horizontal_distance +
				reachability.height_delta * reachability.height_delta);
			reachability.source_contents = source->contents;
			reachability.destination_contents = destination->contents;
			reachability.dynamic_interactor = EVOBOT_NAV_INVALID_ID;
			reachability.mover_interactor = platform->id;
			reachability.board_region = board_region;
			reachability.standing_region = board_region;
			reachability.exit_region = exit_region;
			reachability.expected_wait_time = platform->host.wait > 0 ?
				platform->host.wait * 0.5f : 0.5f;
			reachability.ride_time = platform->host.travel_time;
			reachability.base_travel_time = reachability.expected_wait_time +
				reachability.ride_time + 0.5f;
			before = evobot_nav.reachability_count;
			if (!EvoBot_NavAddReachability(&reachability))
				return 0;
			if (before != evobot_nav.reachability_count)
				evobot_nav.stats.platform_links++;
		}
	}
	return 1;
}

static int EvoBot_NavGeneratePlatforms(void)
{
	double started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	size_t i;

	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		const evobot_nav_interactor_t *platform = &evobot_nav.interactors[i];
		const evobot_bounds_t *low;
		const evobot_bounds_t *high;

		if (platform->host.kind != EVOBOT_INTERACTOR_PLATFORM ||
			!platform->host.has_movement)
			continue;
		evobot_nav.stats.platform_movers++;
		low = platform->host.endpoint_a_bounds.maxs.v[2] <
			platform->host.endpoint_b_bounds.maxs.v[2] ?
			&platform->host.endpoint_a_bounds : &platform->host.endpoint_b_bounds;
		high = low == &platform->host.endpoint_a_bounds ?
			&platform->host.endpoint_b_bounds : &platform->host.endpoint_a_bounds;
		if (!EvoBot_NavAddPlatformDirection(platform, low, high) ||
			!EvoBot_NavAddPlatformDirection(platform, high, low))
			return 0;
	}
	evobot_nav.stats.platform_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	return 1;
}

static void EvoBot_NavAnalyzeActivationGraph(void)
{
	double started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	size_t source;

	for (source = 0; source < evobot_nav.interactor_count; source++)
	{
		size_t destination;
		if (!evobot_nav.interactors[source].host.target[0])
			continue;
		for (destination = 0; destination < evobot_nav.interactor_count; destination++)
			if (evobot_nav.interactors[destination].host.targetname[0] &&
				!strcmp(evobot_nav.interactors[source].host.target,
					evobot_nav.interactors[destination].host.targetname))
				evobot_nav.stats.activation_relationships++;
	}
	evobot_nav.stats.activation_graph_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
}

static int EvoBot_NavGenerateReachabilities(void)
{
	evobot_player_physics_t physics;
	double started;
	size_t i;

	if (!evobot_nav_host.player_physics ||
		!evobot_nav_host.player_physics(&physics) ||
		!evobot_nav_host.simulate_player_move || physics.step_height <= 0 ||
		physics.minimum_ground_normal <= 0 || physics.gravity <= 0 ||
		physics.maximum_speed <= 0)
		return 0;
	started = evobot_nav_host.monotonic_time ? evobot_nav_host.monotonic_time() : 0;
	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		evobot_nav_portal_t *portal = &evobot_nav.portals[i];
		evobot_nav_area_t *a = EvoBot_NavAreaById(portal->area_a);
		evobot_nav_area_t *b = EvoBot_NavAreaById(portal->area_b);

		if (!a || !b ||
			!EvoBot_NavGenerateWalkDirection(portal, a, b, &physics) ||
			!EvoBot_NavGenerateWalkDirection(portal, b, a, &physics) ||
			!EvoBot_NavGenerateSwimDirection(portal, a, b, &physics) ||
			!EvoBot_NavGenerateSwimDirection(portal, b, a, &physics) ||
			!EvoBot_NavGenerateWaterTransition(portal, a, b, &physics) ||
			!EvoBot_NavGenerateWaterTransition(portal, b, a, &physics) ||
			!EvoBot_NavGenerateDropDirection(portal, a, b, &physics) ||
			!EvoBot_NavGenerateDropDirection(portal, b, a, &physics))
			return 0;
	}
	if (!EvoBot_NavGenerateEdgeDrops(&physics))
		return 0;
	if (!EvoBot_NavGenerateJumpUpLedges(&physics))
		return 0;
	if (!EvoBot_NavGenerateGapJumps(&physics))
		return 0;
	if (!EvoBot_NavGeneratePlatforms())
		return 0;
	EvoBot_NavAnalyzeActivationGraph();
	evobot_nav.stats.dynamic_analysis_time = evobot_nav.stats.platform_time +
		evobot_nav.stats.activation_graph_time;
	if (!EvoBot_NavGenerateTeleporters())
		return 0;
	evobot_nav.stats.reachability_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	return 1;
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
		return "empty";
	}
	return "empty";
}

static const char *EvoBot_NavContentsName(evobot_contents_t contents)
{
	switch (contents)
	{
	case EVOBOT_CONTENTS_WATER:
		return "water";
	case EVOBOT_CONTENTS_SLIME:
		return "slime";
	case EVOBOT_CONTENTS_LAVA:
		return "lava";
	case EVOBOT_CONTENTS_SOLID:
		return "solid";
	case EVOBOT_CONTENTS_OTHER:
		return "other";
	case EVOBOT_CONTENTS_AIR:
		return "air";
	}
	return "air";
}

static evobot_contents_t EvoBot_NavContentsFromName(const char *name)
{
	if (!strcmp(name, "water"))
		return EVOBOT_CONTENTS_WATER;
	if (!strcmp(name, "slime"))
		return EVOBOT_CONTENTS_SLIME;
	if (!strcmp(name, "lava"))
		return EVOBOT_CONTENTS_LAVA;
	if (!strcmp(name, "solid"))
		return EVOBOT_CONTENTS_SOLID;
	if (!strcmp(name, "other"))
		return EVOBOT_CONTENTS_OTHER;
	return EVOBOT_CONTENTS_AIR;
}

static const char *EvoBot_NavFaceName(evobot_nav_face_kind_t kind)
{
	switch (kind)
	{
	case EVOBOT_NAV_FACE_PORTAL:
		return "portal";
	case EVOBOT_NAV_FACE_LIQUID:
		return "liquid";
	case EVOBOT_NAV_FACE_LEDGE:
		return "ledge";
	case EVOBOT_NAV_FACE_SOLID:
		return "solid";
	}
	return "solid";
}

static evobot_nav_face_kind_t EvoBot_NavFaceFromName(const char *name)
{
	if (!strcmp(name, "portal"))
		return EVOBOT_NAV_FACE_PORTAL;
	if (!strcmp(name, "liquid"))
		return EVOBOT_NAV_FACE_LIQUID;
	if (!strcmp(name, "ledge"))
		return EVOBOT_NAV_FACE_LEDGE;
	return EVOBOT_NAV_FACE_SOLID;
}

static int EvoBot_NavMapReady(void)
{
	if (!evobot_nav_initialized || !evobot_nav_map_loaded ||
		!evobot_nav_active_map[0])
	{
		EvoBot_NavPrint("EvoBot navigation: no map is loaded\n");
		return 0;
	}
	return 1;
}

void EvoBot_NavConvexInit(const evobot_host_api_t *host)
{
	if (evobot_nav_initialized)
		return;
	memset(&evobot_nav_host, 0, sizeof(evobot_nav_host));
	if (host)
		evobot_nav_host = *host;
	evobot_nav_initialized = 1;
}

void EvoBot_NavConvexMapLoaded(const char *map_name, uint32_t map_checksum)
{
	EvoBot_NavFreeDataset(&evobot_nav);
	EvoBot_NavTopologyChanged();
	snprintf(evobot_nav_active_map, sizeof(evobot_nav_active_map), "%s",
		map_name ? map_name : "");
	evobot_nav_active_checksum = map_checksum;
	evobot_nav_map_loaded = map_name && map_name[0];
}

void EvoBot_NavConvexMapCleared(void)
{
	EvoBot_NavFreeDataset(&evobot_nav);
	EvoBot_NavTopologyChanged();
	evobot_nav_map_loaded = 0;
	evobot_nav_active_map[0] = '\0';
	evobot_nav_active_checksum = 0;
}

void EvoBot_NavConvexShutdown(void)
{
	EvoBot_NavConvexMapCleared();
	memset(&evobot_nav_host, 0, sizeof(evobot_nav_host));
	evobot_nav_initialized = 0;
}

void EvoBot_NavConvexClear(void)
{
	if (!EvoBot_NavMapReady())
		return;
	EvoBot_NavFreeDataset(&evobot_nav);
	EvoBot_NavTopologyChanged();
	EvoBot_NavPrint("EvoBot navigation cleared\n");
}

void EvoBot_NavConvexGenerate(void)
{
	evobot_collision_tree_t player_tree;
	evobot_nav_plane_t path[EVOBOT_NAV_MAX_PLANES];
	double started;
	size_t i;

	if (!EvoBot_NavMapReady())
		return;
	if (!evobot_nav_host.world_bounds || !evobot_nav_host.player_bounds ||
		!evobot_nav_host.collision_tree || !evobot_nav_host.collision_node ||
		!evobot_nav_host.trace_player_world || !evobot_nav_host.point_contents ||
		!evobot_nav_host.player_physics ||
		!evobot_nav_host.simulate_player_move)
	{
		EvoBot_NavPrint("EvoBot navigation: collision geometry is unavailable\n");
		return;
	}
	EvoBot_NavFreeDataset(&evobot_nav);
	snprintf(evobot_nav.map_name, sizeof(evobot_nav.map_name), "%s",
		evobot_nav_active_map);
	evobot_nav.map_checksum = evobot_nav_active_checksum;
	if (!evobot_nav_host.world_bounds(&evobot_nav.world_bounds) ||
		!evobot_nav_host.player_bounds(&evobot_nav.player_bounds) ||
		!evobot_nav_host.collision_tree(EVOBOT_COLLISION_TREE_PLAYER,
			&player_tree))
	{
		EvoBot_NavPrint("EvoBot navigation: world collision is unavailable\n");
		EvoBot_NavFreeDataset(&evobot_nav);
		EvoBot_NavTopologyChanged();
		return;
	}
	started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	EvoBot_NavPrint("EvoBot navigation: building convex player-space areas\n");
	if (!EvoBot_NavCaptureInteractors())
	{
		EvoBot_NavPrint("EvoBot navigation: interactor capture failed\n");
		goto failed;
	}
	if (!EvoBot_NavBuildPlayerLeaves(player_tree.root_node, path, 0))
	{
		EvoBot_NavPrint("EvoBot navigation: player-leaf construction failed\n");
		goto failed;
	}
	if (!EvoBot_NavApplyContentsTree())
	{
		EvoBot_NavPrint("EvoBot navigation: contents subdivision failed\n");
		goto failed;
	}
	if (!EvoBot_NavApplyGravitySubdivision())
	{
		EvoBot_NavPrint("EvoBot navigation: gravity subdivision failed\n");
		goto failed;
	}
	for (i = 0; i < evobot_nav.area_count; i++)
		evobot_nav.areas[i].dynamic_interactor =
			EvoBot_NavDynamicForArea(&evobot_nav.areas[i]);
	EvoBot_NavMergeAreas();
	if (!EvoBot_NavGeneratePortals())
	{
		EvoBot_NavPrint("EvoBot navigation: portal generation failed\n");
		goto failed;
	}
	if (!EvoBot_NavGenerateReachabilities())
	{
		EvoBot_NavPrint("EvoBot navigation: reachability generation failed\n");
		goto failed;
	}
	evobot_nav.stats.generation_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	evobot_nav.state = EVOBOT_NAV_STATE_GENERATED;
	EvoBot_NavTopologyChanged();
	EvoBot_NavPrintf("EvoBot navigation generated\nconvex areas: %zu\nportals: %zu\n"
		"reachabilities: %zu\n"
		"initial collision areas: %" PRIu64 "\ngravity splits: %" PRIu64 "\n"
		"convex merges: %" PRIu64 "\ngeneration time: %.3f seconds\n",
		evobot_nav.area_count, evobot_nav.portal_count,
		evobot_nav.reachability_count,
		evobot_nav.stats.initial_areas, evobot_nav.stats.gravity_splits,
		evobot_nav.stats.merges, evobot_nav.stats.generation_time);
	return;

failed:
	EvoBot_NavPrint("EvoBot navigation: convex generation failed\n");
	EvoBot_NavFreeDataset(&evobot_nav);
	EvoBot_NavTopologyChanged();
}

void EvoBot_NavConvexPrintStatus(void)
{
	size_t supported = 0;
	size_t water = 0;
	size_t slime = 0;
	size_t lava = 0;
	size_t solid_faces = 0;
	size_t ledges = 0;
	size_t liquid_portals = 0;
	size_t i;

	if (!EvoBot_NavMapReady())
		return;
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		size_t face;

		if (evobot_nav.areas[i].supported)
			supported++;
		if (evobot_nav.areas[i].contents == EVOBOT_CONTENTS_WATER)
			water++;
		else if (evobot_nav.areas[i].contents == EVOBOT_CONTENTS_SLIME)
			slime++;
		else if (evobot_nav.areas[i].contents == EVOBOT_CONTENTS_LAVA)
			lava++;
		for (face = 0; face < evobot_nav.areas[i].face_count; face++)
		{
			if (evobot_nav.areas[i].faces[face].kind == EVOBOT_NAV_FACE_SOLID)
				solid_faces++;
		}
	}
	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		if (evobot_nav.portals[i].kind == EVOBOT_NAV_FACE_LEDGE)
			ledges++;
		else if (evobot_nav.portals[i].kind == EVOBOT_NAV_FACE_LIQUID)
			liquid_portals++;
	}
	EvoBot_NavPrintf("EvoBot navigation status\nmap: %s\nchecksum: %" PRIu32
		"\nformat version: %d\nstate: %s\nconvex areas: %zu\nportals: %zu\n"
		"solid boundary faces: %zu\nledge boundaries: %zu\nliquid boundaries: %zu\n"
		"interactors: %zu\nsupported areas: %zu\nunsupported areas: %zu\n"
		"water areas: %zu\nslime areas: %zu\nlava areas: %zu\n"
		"generation time: %.3f seconds\n",
		evobot_nav_active_map, evobot_nav_active_checksum,
		EVOBOT_NAV_FORMAT_VERSION, EvoBot_NavStateName(evobot_nav.state),
		evobot_nav.area_count, evobot_nav.portal_count, solid_faces, ledges,
		liquid_portals, evobot_nav.interactor_count, supported,
		evobot_nav.area_count - supported, water, slime, lava,
		evobot_nav.stats.generation_time);
}

static const char *EvoBot_NavTravelName(evobot_nav_travel_type_t type)
{
	switch (type)
	{
	case EVOBOT_NAV_TRAVEL_WALK:
		return "walk";
	case EVOBOT_NAV_TRAVEL_DROP:
		return "drop";
	case EVOBOT_NAV_TRAVEL_SWIM:
		return "swim";
	case EVOBOT_NAV_TRAVEL_WATER_ENTRY:
		return "water_entry";
	case EVOBOT_NAV_TRAVEL_WATER_EXIT:
		return "water_exit";
	case EVOBOT_NAV_TRAVEL_WATER_JUMP:
		return "water_jump";
	case EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP:
		return "unresolved_water_jump";
	case EVOBOT_NAV_TRAVEL_JUMP:
		return "jump";
	case EVOBOT_NAV_TRAVEL_TELEPORT:
		return "teleport";
	case EVOBOT_NAV_TRAVEL_PLATFORM:
		return "platform";
	}
	return "walk";
}

static evobot_nav_travel_type_t EvoBot_NavTravelFromName(const char *name)
{
	if (!strcmp(name, "drop")) return EVOBOT_NAV_TRAVEL_DROP;
	if (!strcmp(name, "swim")) return EVOBOT_NAV_TRAVEL_SWIM;
	if (!strcmp(name, "water_entry")) return EVOBOT_NAV_TRAVEL_WATER_ENTRY;
	if (!strcmp(name, "water_exit")) return EVOBOT_NAV_TRAVEL_WATER_EXIT;
	if (!strcmp(name, "water_jump")) return EVOBOT_NAV_TRAVEL_WATER_JUMP;
	if (!strcmp(name, "unresolved_water_jump"))
		return EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP;
	if (!strcmp(name, "jump")) return EVOBOT_NAV_TRAVEL_JUMP;
	if (!strcmp(name, "teleport")) return EVOBOT_NAV_TRAVEL_TELEPORT;
	if (!strcmp(name, "platform")) return EVOBOT_NAV_TRAVEL_PLATFORM;
	return EVOBOT_NAV_TRAVEL_WALK;
}

void EvoBot_NavReachPrintStatus(void)
{
	size_t counts[EVOBOT_NAV_TRAVEL_PLATFORM + 1] = { 0 };
	unsigned char *incoming;
	unsigned char *outgoing;
	unsigned char *grounded;
	unsigned char *liquid;
	unsigned char *pairs;
	size_t routing_areas = 0;
	size_t no_incoming = 0;
	size_t no_outgoing = 0;
	size_t grounded_participating = 0;
	size_t liquid_participating = 0;
	size_t bidirectional = 0;
	size_t one_way = 0;
	size_t i;

	if (!EvoBot_NavMapReady())
		return;
	incoming = calloc(evobot_nav.area_count, 1);
	outgoing = calloc(evobot_nav.area_count, 1);
	grounded = calloc(evobot_nav.area_count, 1);
	liquid = calloc(evobot_nav.area_count, 1);
	pairs = calloc(evobot_nav.area_count * evobot_nav.area_count, 1);
	if ((!incoming || !outgoing || !grounded || !liquid || !pairs) &&
		evobot_nav.area_count)
	{
		free(incoming);
		free(outgoing);
		free(grounded);
		free(liquid);
		free(pairs);
		EvoBot_NavPrint("EvoBot reachabilities: could not allocate status data\n");
		return;
	}
	for (i = 0; i < evobot_nav.reachability_count; i++)
	{
		const evobot_nav_reachability_t *reachability =
			&evobot_nav.reachabilities[i];
		size_t source = reachability->source_area - 1;
		size_t destination = reachability->destination_area - 1;
		size_t low;
		size_t high;

		if ((size_t)reachability->travel_type < sizeof(counts) / sizeof(counts[0]))
			counts[reachability->travel_type]++;
		if (source >= evobot_nav.area_count || destination >= evobot_nav.area_count ||
			reachability->travel_type == EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP)
			continue;
		outgoing[source] = 1;
		incoming[destination] = 1;
		if (evobot_nav.areas[source].supported)
			grounded[source] = 1;
		if (evobot_nav.areas[destination].supported)
			grounded[destination] = 1;
		if (EvoBot_NavContentsLiquid(evobot_nav.areas[source].contents))
			liquid[source] = 1;
		if (EvoBot_NavContentsLiquid(evobot_nav.areas[destination].contents))
			liquid[destination] = 1;
		low = source < destination ? source : destination;
		high = source < destination ? destination : source;
		pairs[low * evobot_nav.area_count + high] |= source == low ? 1 : 2;
	}
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		size_t j;
		int routing = evobot_nav.areas[i].supported ||
			(EvoBot_NavContentsLiquid(evobot_nav.areas[i].contents) &&
				evobot_nav.areas[i].water_level >= 2);

		if (routing)
		{
			routing_areas++;
			if (!incoming[i]) no_incoming++;
			if (!outgoing[i]) no_outgoing++;
		}
		if (grounded[i]) grounded_participating++;
		if (liquid[i]) liquid_participating++;
		for (j = i + 1; j < evobot_nav.area_count; j++)
		{
			unsigned char directions = pairs[i * evobot_nav.area_count + j];

			if (directions == 3) bidirectional++;
			else if (directions) one_way++;
		}
	}
	EvoBot_NavPrintf("EvoBot reachabilities\ntotal: %zu\nwalk: %zu\ndrop: %zu\njump: %zu\nteleport: %zu\nplatform: %zu\n"
		"swim: %zu\nwater entry: %zu\nwater exit: %zu\nwater jump: %zu\n"
		"unresolved water jump: %zu\n\nrouting areas: %zu\n"
		"grounded areas participating: %zu\nliquid areas participating: %zu\n"
		"areas with no outgoing reachability: %zu\n"
		"areas with no incoming reachability: %zu\n"
		"bidirectional area pairs: %zu\none-way area pairs: %zu\n"
		"teleporter triggers/resolved/links: %" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n"
		"edge drop seeds/links: %" PRIu64 "/%" PRIu64 "\n"
		"gap jump seeds/links: %" PRIu64 "/%" PRIu64 "\n"
		"gap jump rejected start/air/landing/edge: %" PRIu64 "/%" PRIu64
		"/%" PRIu64 "/%" PRIu64 "\n"
		"jump-up ledge candidates/validated/links/attempts: %" PRIu64
		"/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n"
		"water-jump candidates/validated/links/attempts: %" PRIu64
		"/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n"
		"reachability/teleport/drop/gap-jump/jump-up-ledge/water-jump time: "
		"%.3f / %.3f / %.3f / %.3f / %.3f / %.3f seconds\n"
		"dynamic relationships/platform/graph: %" PRIu64 " / %.3f / %.6f seconds\n"
		"dynamic analysis time: %.3f seconds\n",
		evobot_nav.reachability_count, counts[EVOBOT_NAV_TRAVEL_WALK],
		counts[EVOBOT_NAV_TRAVEL_DROP], counts[EVOBOT_NAV_TRAVEL_JUMP],
		counts[EVOBOT_NAV_TRAVEL_TELEPORT],
		counts[EVOBOT_NAV_TRAVEL_PLATFORM],
		counts[EVOBOT_NAV_TRAVEL_SWIM],
		counts[EVOBOT_NAV_TRAVEL_WATER_ENTRY],
		counts[EVOBOT_NAV_TRAVEL_WATER_EXIT],
		counts[EVOBOT_NAV_TRAVEL_WATER_JUMP],
		counts[EVOBOT_NAV_TRAVEL_UNRESOLVED_WATER_JUMP], routing_areas,
		grounded_participating, liquid_participating, no_outgoing, no_incoming,
		bidirectional, one_way, evobot_nav.stats.teleporter_triggers,
		evobot_nav.stats.resolved_teleporters, evobot_nav.stats.teleport_links,
		evobot_nav.stats.edge_drop_seeds, evobot_nav.stats.edge_drop_links,
		evobot_nav.stats.gap_jump_seeds, evobot_nav.stats.gap_jump_links,
		evobot_nav.stats.gap_jump_rejected_start,
		evobot_nav.stats.gap_jump_rejected_air,
		evobot_nav.stats.gap_jump_rejected_landing,
		evobot_nav.stats.gap_jump_rejected_edge,
		evobot_nav.stats.jump_up_ledge_candidates,
		evobot_nav.stats.jump_up_ledge_validated,
		evobot_nav.stats.jump_up_ledge_links,
		evobot_nav.stats.jump_up_ledge_attempts,
		evobot_nav.stats.water_jump_candidates,
		evobot_nav.stats.water_jump_validated,
		evobot_nav.stats.water_jump_links,
		evobot_nav.stats.water_jump_attempts,
		evobot_nav.stats.reachability_time, evobot_nav.stats.teleport_time,
		evobot_nav.stats.edge_drop_time, evobot_nav.stats.gap_jump_time,
		evobot_nav.stats.jump_up_ledge_time,
		evobot_nav.stats.water_jump_time,
		evobot_nav.stats.activation_relationships, evobot_nav.stats.platform_time,
		evobot_nav.stats.activation_graph_time,
		evobot_nav.stats.dynamic_analysis_time);
	free(incoming);
	free(outgoing);
	free(grounded);
	free(liquid);
	free(pairs);
}

static void EvoBot_NavSegmentCenter(const evobot_nav_segment_t *segment,
	evobot_vec3_t *center)
{
	int axis;

	for (axis = 0; axis < 3; axis++)
		center->v[axis] = (segment->start.v[axis] + segment->end.v[axis]) * 0.5f;
}

void EvoBot_NavReachValidate(void)
{
	size_t errors = 0;
	size_t i;

	if (!EvoBot_NavMapReady())
		return;
	for (i = 0; i < evobot_nav.reachability_count; i++)
	{
		const evobot_nav_reachability_t *reachability =
			&evobot_nav.reachabilities[i];
		evobot_nav_area_t *source = EvoBot_NavAreaById(reachability->source_area);
		evobot_nav_area_t *destination =
			EvoBot_NavAreaById(reachability->destination_area);
		const evobot_nav_portal_t *portal = NULL;
		evobot_vec3_t start;
		evobot_vec3_t end;
		const char *reason = NULL;
		size_t j;

		if (reachability->id != i + 1)
			reason = "id is not unique/sequential";
		else if (!source || !destination)
			reason = "source or destination area is invalid";
		else if (source == destination)
			reason = "self link";
		else if (reachability->travel_type != EVOBOT_NAV_TRAVEL_TELEPORT &&
			reachability->travel_type != EVOBOT_NAV_TRAVEL_PLATFORM &&
			(!reachability->portal_id ||
			reachability->portal_id > evobot_nav.portal_count))
			reason = "portal is invalid";
		else if (reachability->travel_type != EVOBOT_NAV_TRAVEL_TELEPORT &&
			reachability->travel_type != EVOBOT_NAV_TRAVEL_PLATFORM)
			portal = &evobot_nav.portals[reachability->portal_id - 1];
		if (!reason && reachability->travel_type != EVOBOT_NAV_TRAVEL_TELEPORT &&
			reachability->travel_type != EVOBOT_NAV_TRAVEL_PLATFORM)
		{
			evobot_nav_plane_t expected = portal->plane;

			if (portal->area_b == source->id)
				expected = EvoBot_NavReversePlane(&expected);
			if (reachability->source_face >= source->face_count ||
				!EvoBot_NavPlaneEqual(
					&source->planes[source->faces[reachability->source_face].plane],
					&expected) || reachability->source_edge >= portal->vertex_count)
				reason = "face or edge reference is invalid";
		}
		EvoBot_NavSegmentCenter(&reachability->start, &start);
		EvoBot_NavSegmentCenter(&reachability->destination, &end);
		if (!reason && !EvoBot_NavPointInside(source, &start, 1.5f))
			reason = "start geometry is outside source area";
		else if (!reason && !EvoBot_NavPointInside(destination, &end, 2.5f))
			reason = "destination geometry is outside destination area";
		else if (!reason && reachability->travel_type == EVOBOT_NAV_TRAVEL_WALK &&
			portal->kind == EVOBOT_NAV_FACE_LEDGE)
			reason = "walk crosses a ledge boundary";
		else if (!reason && reachability->travel_type == EVOBOT_NAV_TRAVEL_DROP &&
			reachability->height_delta >= -0.5f)
			reason = "drop does not move downward";
		else if (!reason && reachability->travel_type == EVOBOT_NAV_TRAVEL_SWIM &&
			(!EvoBot_NavContentsLiquid(source->contents) ||
				!EvoBot_NavContentsLiquid(destination->contents)))
			reason = "swim does not connect liquid areas";
		else if (!reason && reachability->travel_type ==
			EVOBOT_NAV_TRAVEL_WATER_JUMP &&
			(!EvoBot_NavContentsLiquid(source->contents) ||
			 source->water_level < 2 || !destination->supported ||
			 destination->water_level >= 2 ||
			 reachability->base_travel_time <= 0))
			reason = "water jump metadata is invalid";
		else if (!reason && reachability->travel_type == EVOBOT_NAV_TRAVEL_TELEPORT &&
			(!reachability->source_interactor ||
			 !reachability->destination_interactor))
			reason = "teleport interactor metadata is invalid";
		else if (!reason && reachability->travel_type == EVOBOT_NAV_TRAVEL_PLATFORM &&
			!reachability->mover_interactor)
			reason = "platform mover metadata is invalid";
		for (j = 0; !reason && j < i; j++)
		{
			const evobot_nav_reachability_t *other = &evobot_nav.reachabilities[j];

			if (other->source_area == reachability->source_area &&
				other->destination_area == reachability->destination_area &&
				other->travel_type == reachability->travel_type &&
				other->portal_id == reachability->portal_id &&
				(reachability->travel_type != EVOBOT_NAV_TRAVEL_TELEPORT ||
					(other->source_interactor == reachability->source_interactor &&
					 other->destination_interactor ==
						reachability->destination_interactor)) &&
				(reachability->travel_type != EVOBOT_NAV_TRAVEL_PLATFORM ||
				 other->mover_interactor == reachability->mover_interactor))
				reason = "duplicate directed link";
			else if (reachability->travel_type == EVOBOT_NAV_TRAVEL_DROP &&
				other->travel_type == EVOBOT_NAV_TRAVEL_DROP &&
				other->portal_id == reachability->portal_id &&
				other->source_area == reachability->destination_area &&
				other->destination_area == reachability->source_area)
				reason = "drop was accidentally mirrored";
		}
		if (reason)
		{
			EvoBot_NavPrintf("reachability %" PRIu32 ": %s\n",
				reachability->id, reason);
			errors++;
		}
	}
	if (errors)
		EvoBot_NavPrintf("EvoBot reachability validation: %zu error(s)\n", errors);
	else
		EvoBot_NavPrintf("EvoBot reachability validation: ok (%zu links)\n",
			evobot_nav.reachability_count);
}

static int EvoBot_NavBufferReserve(evobot_nav_buffer_t *buffer, size_t extra)
{
	char *memory;
	size_t required = buffer->length + extra + 1;
	size_t capacity;

	if (required <= buffer->capacity)
		return 1;
	capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
	while (capacity < required)
		capacity *= 2;
	memory = realloc(buffer->data, capacity);
	if (!memory)
	{
		buffer->failed = 1;
		return 0;
	}
	buffer->data = memory;
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

static int EvoBot_NavBufferAppendFormat(evobot_nav_buffer_t *buffer,
	const char *format, ...)
{
	char stack[512];
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(stack, sizeof(stack), format, args);
	va_end(args);
	if (length < 0)
	{
		buffer->failed = 1;
		return 0;
	}
	if ((size_t)length < sizeof(stack))
		return EvoBot_NavBufferAppend(buffer, stack);
	else
	{
		char *dynamic = malloc((size_t)length + 1);
		int result;

		if (!dynamic)
			return 0;
		va_start(args, format);
		vsnprintf(dynamic, (size_t)length + 1, format, args);
		va_end(args);
		result = EvoBot_NavBufferAppend(buffer, dynamic);
		free(dynamic);
		return result;
	}
}

static int EvoBot_NavBufferJsonString(evobot_nav_buffer_t *buffer,
	const char *text)
{
	const unsigned char *cursor = (const unsigned char *)text;

	if (!EvoBot_NavBufferAppend(buffer, "\""))
		return 0;
	while (*cursor)
	{
		char escaped[8];

		switch (*cursor)
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
		default:
			if (*cursor < 32)
			{
				snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
				if (!EvoBot_NavBufferAppend(buffer, escaped))
					return 0;
			}
			else
			{
				escaped[0] = (char)*cursor;
				escaped[1] = '\0';
				if (!EvoBot_NavBufferAppend(buffer, escaped))
					return 0;
			}
			break;
		}
		cursor++;
	}
	return EvoBot_NavBufferAppend(buffer, "\"");
}

static int EvoBot_NavWriteVec3(evobot_nav_buffer_t *buffer,
	const evobot_vec3_t *vector)
{
	return EvoBot_NavBufferAppendFormat(buffer, "[%.9g, %.9g, %.9g]",
		vector->v[0], vector->v[1], vector->v[2]);
}

static int EvoBot_NavWriteBounds(evobot_nav_buffer_t *buffer,
	const evobot_bounds_t *bounds)
{
	return EvoBot_NavBufferAppend(buffer, "{\"mins\": ") &&
		EvoBot_NavWriteVec3(buffer, &bounds->mins) &&
		EvoBot_NavBufferAppend(buffer, ", \"maxs\": ") &&
		EvoBot_NavWriteVec3(buffer, &bounds->maxs) &&
		EvoBot_NavBufferAppend(buffer, "}");
}

static int EvoBot_NavWriteSegment(evobot_nav_buffer_t *buffer,
	const evobot_nav_segment_t *segment)
{
	return EvoBot_NavBufferAppend(buffer, "[") &&
		EvoBot_NavWriteVec3(buffer, &segment->start) &&
		EvoBot_NavBufferAppend(buffer, ", ") &&
		EvoBot_NavWriteVec3(buffer, &segment->end) &&
		EvoBot_NavBufferAppend(buffer, "]");
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
	case EVOBOT_INTERACTOR_TRIGGER:
		return "trigger";
	case EVOBOT_INTERACTOR_LOGIC:
		return "logic";
	case EVOBOT_INTERACTOR_OTHER:
		return "other";
	}
	return "other";
}

static evobot_interactor_kind_t EvoBot_NavInteractorFromName(const char *name)
{
	if (!strcmp(name, "door")) return EVOBOT_INTERACTOR_DOOR;
	if (!strcmp(name, "button")) return EVOBOT_INTERACTOR_BUTTON;
	if (!strcmp(name, "platform")) return EVOBOT_INTERACTOR_PLATFORM;
	if (!strcmp(name, "train")) return EVOBOT_INTERACTOR_TRAIN;
	if (!strcmp(name, "teleporter")) return EVOBOT_INTERACTOR_TELEPORTER;
	if (!strcmp(name, "teleport_destination")) return EVOBOT_INTERACTOR_TELEPORT_DESTINATION;
	if (!strcmp(name, "level_exit")) return EVOBOT_INTERACTOR_LEVEL_EXIT;
	if (!strcmp(name, "trigger")) return EVOBOT_INTERACTOR_TRIGGER;
	if (!strcmp(name, "logic")) return EVOBOT_INTERACTOR_LOGIC;
	return EVOBOT_INTERACTOR_OTHER;
}

static int EvoBot_NavSerializeJson(evobot_nav_buffer_t *buffer)
{
	size_t i;

	if (!EvoBot_NavBufferAppend(buffer, "{\n  \"format\": \"evobot-nav\",\n") ||
		!EvoBot_NavBufferAppendFormat(buffer, "  \"version\": %d,\n",
			EVOBOT_NAV_FORMAT_VERSION) ||
		!EvoBot_NavBufferAppend(buffer, "  \"map\": {\"name\": ") ||
		!EvoBot_NavBufferJsonString(buffer, evobot_nav.map_name) ||
		!EvoBot_NavBufferAppendFormat(buffer, ", \"checksum\": %" PRIu32 "},\n",
			evobot_nav.map_checksum) ||
		!EvoBot_NavBufferAppend(buffer, "  \"world_bounds\": ") ||
		!EvoBot_NavWriteBounds(buffer, &evobot_nav.world_bounds) ||
		!EvoBot_NavBufferAppend(buffer, ",\n  \"player_bounds\": ") ||
		!EvoBot_NavWriteBounds(buffer, &evobot_nav.player_bounds) ||
		!EvoBot_NavBufferAppendFormat(buffer,
			",\n  \"generation\": {\"initial_areas\": %" PRIu64
			", \"content_splits\": %" PRIu64 ", \"gravity_splits\": %" PRIu64
			", \"merges\": %" PRIu64 ", \"validation_traces\": %" PRIu64
			", \"movement_tests\": %" PRIu64
			", \"teleporter_triggers\": %" PRIu64
			", \"resolved_teleporters\": %" PRIu64
			", \"teleport_links\": %" PRIu64
			", \"edge_drop_seeds\": %" PRIu64
			", \"edge_drop_links\": %" PRIu64
			", \"gap_jump_seeds\": %" PRIu64
			", \"gap_jump_links\": %" PRIu64
			", \"gap_jump_rejected_start\": %" PRIu64
			", \"gap_jump_rejected_air\": %" PRIu64
			", \"gap_jump_rejected_landing\": %" PRIu64
			", \"gap_jump_rejected_edge\": %" PRIu64
			", \"jump_up_ledge_candidates\": %" PRIu64
			", \"jump_up_ledge_validated\": %" PRIu64
			", \"jump_up_ledge_links\": %" PRIu64
			", \"jump_up_ledge_attempts\": %" PRIu64
			", \"water_jump_candidates\": %" PRIu64
			", \"water_jump_validated\": %" PRIu64
			", \"water_jump_links\": %" PRIu64
			", \"water_jump_attempts\": %" PRIu64
			", \"platform_movers\": %" PRIu64
			", \"platform_links\": %" PRIu64
			", \"activation_relationships\": %" PRIu64
			", \"seconds\": %.9g, \"reachability_seconds\": %.9g"
			", \"teleport_seconds\": %.9g, \"edge_drop_seconds\": %.9g"
			", \"gap_jump_seconds\": %.9g, \"jump_up_ledge_seconds\": %.9g"
			", \"water_jump_seconds\": %.9g"
			", \"platform_seconds\": %.9g"
			", \"activation_graph_seconds\": %.9g, \"dynamic_analysis_seconds\": %.9g},\n  \"areas\": [\n",
			evobot_nav.stats.initial_areas, evobot_nav.stats.content_splits,
			evobot_nav.stats.gravity_splits, evobot_nav.stats.merges,
			evobot_nav.stats.validation_traces, evobot_nav.stats.movement_tests,
			evobot_nav.stats.teleporter_triggers,
			evobot_nav.stats.resolved_teleporters,
			evobot_nav.stats.teleport_links,
			evobot_nav.stats.edge_drop_seeds,
			evobot_nav.stats.edge_drop_links,
			evobot_nav.stats.gap_jump_seeds,
			evobot_nav.stats.gap_jump_links,
			evobot_nav.stats.gap_jump_rejected_start,
			evobot_nav.stats.gap_jump_rejected_air,
			evobot_nav.stats.gap_jump_rejected_landing,
			evobot_nav.stats.gap_jump_rejected_edge,
			evobot_nav.stats.jump_up_ledge_candidates,
			evobot_nav.stats.jump_up_ledge_validated,
			evobot_nav.stats.jump_up_ledge_links,
			evobot_nav.stats.jump_up_ledge_attempts,
			evobot_nav.stats.water_jump_candidates,
			evobot_nav.stats.water_jump_validated,
			evobot_nav.stats.water_jump_links,
			evobot_nav.stats.water_jump_attempts,
			evobot_nav.stats.platform_movers,
			evobot_nav.stats.platform_links,
			evobot_nav.stats.activation_relationships,
			evobot_nav.stats.generation_time,
			evobot_nav.stats.reachability_time, evobot_nav.stats.teleport_time,
			evobot_nav.stats.edge_drop_time, evobot_nav.stats.gap_jump_time,
			evobot_nav.stats.jump_up_ledge_time,
			evobot_nav.stats.water_jump_time,
			evobot_nav.stats.platform_time,
			evobot_nav.stats.activation_graph_time,
			evobot_nav.stats.dynamic_analysis_time))
		return 0;
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		const evobot_nav_area_t *area = &evobot_nav.areas[i];
		size_t j;

		if (!EvoBot_NavBufferAppendFormat(buffer, "    {\"id\": %" PRIu32
			", \"bounds\": ", area->id) ||
			!EvoBot_NavWriteBounds(buffer, &area->bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"contents\": ") ||
			!EvoBot_NavBufferJsonString(buffer, EvoBot_NavContentsName(area->contents)) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"supported\": %s, \"floor_height\": %.9g, \"support_distance\": %.9g, \"support_normal\": ",
				area->supported ? "true" : "false", area->floor_height,
				area->support_distance) ||
			!EvoBot_NavWriteVec3(buffer, &area->support_normal) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"water_level\": %d, \"dynamic_interactor\": %d, \"planes\": [",
				area->water_level, area->dynamic_interactor))
			return 0;
		for (j = 0; j < area->plane_count; j++)
		{
			if (j && !EvoBot_NavBufferAppend(buffer, ", ")) return 0;
			if (!EvoBot_NavBufferAppend(buffer, "{\"normal\": ") ||
				!EvoBot_NavWriteVec3(buffer, &area->planes[j].normal) ||
				!EvoBot_NavBufferAppendFormat(buffer, ", \"distance\": %.9g}",
					area->planes[j].distance)) return 0;
		}
		if (!EvoBot_NavBufferAppend(buffer, "], \"faces\": [")) return 0;
		for (j = 0; j < area->face_count; j++)
		{
			const evobot_nav_face_t *face = &area->faces[j];
			size_t k;

			if (j && !EvoBot_NavBufferAppend(buffer, ", ")) return 0;
			if (!EvoBot_NavBufferAppendFormat(buffer,
				"{\"plane\": %d, \"type\": ", face->plane) ||
				!EvoBot_NavBufferJsonString(buffer, EvoBot_NavFaceName(face->kind)) ||
				!EvoBot_NavBufferAppendFormat(buffer,
					", \"portal\": %d, \"vertices\": [", face->portal)) return 0;
			for (k = 0; k < face->vertex_count; k++)
			{
				if (k && !EvoBot_NavBufferAppend(buffer, ", ")) return 0;
				if (!EvoBot_NavWriteVec3(buffer, &face->vertices[k])) return 0;
			}
			if (!EvoBot_NavBufferAppend(buffer, "]}")) return 0;
		}
		if (!EvoBot_NavBufferAppendFormat(buffer, "]}%s\n",
			i + 1 == evobot_nav.area_count ? "" : ",")) return 0;
	}
	if (!EvoBot_NavBufferAppend(buffer, "  ],\n  \"portals\": [\n")) return 0;
	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		const evobot_nav_portal_t *portal = &evobot_nav.portals[i];
		size_t j;

		if (!EvoBot_NavBufferAppendFormat(buffer,
			"    {\"id\": %" PRIu32 ", \"area_a\": %" PRIu32
			", \"area_b\": %" PRIu32 ", \"type\": ", portal->id,
			portal->area_a, portal->area_b) ||
			!EvoBot_NavBufferJsonString(buffer, EvoBot_NavFaceName(portal->kind)) ||
			!EvoBot_NavBufferAppend(buffer, ", \"plane\": {\"normal\": ") ||
			!EvoBot_NavWriteVec3(buffer, &portal->plane.normal) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"distance\": %.9g}, \"vertices\": [", portal->plane.distance)) return 0;
		for (j = 0; j < portal->vertex_count; j++)
		{
			if (j && !EvoBot_NavBufferAppend(buffer, ", ")) return 0;
			if (!EvoBot_NavWriteVec3(buffer, &portal->vertices[j])) return 0;
		}
		if (!EvoBot_NavBufferAppendFormat(buffer, "]}%s\n",
			i + 1 == evobot_nav.portal_count ? "" : ",")) return 0;
	}
	if (!EvoBot_NavBufferAppend(buffer, "  ],\n  \"interactors\": [\n")) return 0;
	for (i = 0; i < evobot_nav.interactor_count; i++)
	{
		const evobot_nav_interactor_t *interactor = &evobot_nav.interactors[i];
		const evobot_host_interactor_t *host = &interactor->host;

		if (!EvoBot_NavBufferAppendFormat(buffer, "    {\"id\": %" PRIu32
			", \"type\": ", interactor->id) ||
			!EvoBot_NavBufferJsonString(buffer, EvoBot_NavInteractorName(host->kind)) ||
			!EvoBot_NavBufferAppendFormat(buffer, ", \"dynamic_brush\": %s, \"bounds\": ",
				host->dynamic_brush ? "true" : "false") ||
			!EvoBot_NavWriteBounds(buffer, &host->bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"swept_bounds\": ") ||
			!EvoBot_NavWriteBounds(buffer, &host->swept_bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"origin\": ") ||
			!EvoBot_NavWriteVec3(buffer, &host->origin) ||
			!EvoBot_NavBufferAppend(buffer, ", \"angles\": ") ||
			!EvoBot_NavWriteVec3(buffer, &host->angles) ||
			!EvoBot_NavBufferAppend(buffer, ", \"velocity\": ") ||
			!EvoBot_NavWriteVec3(buffer, &host->velocity) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"has_angles\": %s, \"has_velocity\": %s, \"has_movement\": %s",
				host->has_angles ? "true" : "false",
				host->has_velocity ? "true" : "false",
				host->has_movement ? "true" : "false") ||
			!EvoBot_NavBufferAppend(buffer, ", \"endpoint_a\": ") ||
			!EvoBot_NavWriteVec3(buffer, &host->endpoint_a) ||
			!EvoBot_NavBufferAppend(buffer, ", \"endpoint_b\": ") ||
			!EvoBot_NavWriteVec3(buffer, &host->endpoint_b) ||
			!EvoBot_NavBufferAppend(buffer, ", \"endpoint_a_bounds\": ") ||
			!EvoBot_NavWriteBounds(buffer, &host->endpoint_a_bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"endpoint_b_bounds\": ") ||
			!EvoBot_NavWriteBounds(buffer, &host->endpoint_b_bounds) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"speed\": %.9g, \"wait\": %.9g, \"travel_time\": %.9g"
				", \"spawnflags\": %d, \"health\": %.9g, \"activation\": %d"
				", \"lifetime\": %d, \"activation_required_count\": %d",
				host->speed, host->wait, host->travel_time,
				host->spawnflags, host->health, (int)host->activation,
				(int)host->lifetime, host->activation_required_count) ||
			!EvoBot_NavBufferAppend(buffer, ", \"classname\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->classname) ||
			!EvoBot_NavBufferAppend(buffer, ", \"model\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->model) ||
			!EvoBot_NavBufferAppend(buffer, ", \"target\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->target) ||
			!EvoBot_NavBufferAppend(buffer, ", \"targetname\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->targetname) ||
			!EvoBot_NavBufferAppend(buffer, ", \"killtarget\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->killtarget) ||
			!EvoBot_NavBufferAppend(buffer, ", \"destination_map\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->destination_map) ||
			!EvoBot_NavBufferAppendFormat(buffer, "}%s\n",
				i + 1 == evobot_nav.interactor_count ? "" : ",")) return 0;
	}
	if (!EvoBot_NavBufferAppend(buffer, "  ],\n  \"reachabilities\": [\n"))
		return 0;
	for (i = 0; i < evobot_nav.reachability_count; i++)
	{
		const evobot_nav_reachability_t *reachability =
			&evobot_nav.reachabilities[i];

		if (!EvoBot_NavBufferAppendFormat(buffer, "    {\"id\": %" PRIu32
			", \"source_area\": %" PRIu32 ", \"destination_area\": %" PRIu32
			", \"travel_type\": ", reachability->id, reachability->source_area,
			reachability->destination_area) ||
			!EvoBot_NavBufferJsonString(buffer,
				EvoBot_NavTravelName(reachability->travel_type)) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"portal_id\": %" PRIu32 ", \"source_face\": %" PRIu32
				", \"source_edge\": %" PRIu32 ", \"start\": ",
				reachability->portal_id, reachability->source_face,
				reachability->source_edge) ||
			!EvoBot_NavWriteSegment(buffer, &reachability->start) ||
			!EvoBot_NavBufferAppend(buffer, ", \"destination\": ") ||
			!EvoBot_NavWriteSegment(buffer, &reachability->destination) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"height_delta\": %.9g, \"horizontal_distance\": %.9g"
				", \"travel_distance\": %.9g, \"base_travel_time\": %.9g"
				", \"source_contents\": ", reachability->height_delta,
				reachability->horizontal_distance, reachability->travel_distance,
				reachability->base_travel_time) ||
			!EvoBot_NavBufferJsonString(buffer,
				EvoBot_NavContentsName(reachability->source_contents)) ||
			!EvoBot_NavBufferAppend(buffer, ", \"destination_contents\": ") ||
			!EvoBot_NavBufferJsonString(buffer,
				EvoBot_NavContentsName(reachability->destination_contents)) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"flags\": %" PRIu32 ", \"dynamic_interactor\": %d"
				", \"source_interactor\": %" PRIu32
				", \"destination_interactor\": %" PRIu32 ", \"entry_bounds\": ",
				reachability->flags, reachability->dynamic_interactor,
				reachability->source_interactor,
				reachability->destination_interactor) ||
			!EvoBot_NavWriteBounds(buffer, &reachability->entry_bounds) ||
			!EvoBot_NavBufferAppend(buffer, ", \"arrival_origin\": ") ||
			!EvoBot_NavWriteVec3(buffer, &reachability->arrival_origin) ||
			!EvoBot_NavBufferAppend(buffer, ", \"arrival_angles\": ") ||
			!EvoBot_NavWriteVec3(buffer, &reachability->arrival_angles) ||
			!EvoBot_NavBufferAppend(buffer, ", \"arrival_velocity\": ") ||
			!EvoBot_NavWriteVec3(buffer, &reachability->arrival_velocity) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"mover_interactor\": %" PRIu32 ", \"board_region\": ",
				reachability->mover_interactor) ||
			!EvoBot_NavWriteBounds(buffer, &reachability->board_region) ||
			!EvoBot_NavBufferAppend(buffer, ", \"standing_region\": ") ||
			!EvoBot_NavWriteBounds(buffer, &reachability->standing_region) ||
			!EvoBot_NavBufferAppend(buffer, ", \"exit_region\": ") ||
			!EvoBot_NavWriteBounds(buffer, &reachability->exit_region) ||
			!EvoBot_NavBufferAppendFormat(buffer,
				", \"expected_wait_time\": %.9g, \"ride_time\": %.9g",
				reachability->expected_wait_time, reachability->ride_time) ||
			!EvoBot_NavBufferAppendFormat(buffer, "}%s\n",
				i + 1 == evobot_nav.reachability_count ? "" : ","))
			return 0;
	}
	return EvoBot_NavBufferAppend(buffer, "  ]\n}\n");
}

static void EvoBot_NavPath(char *path, size_t path_size, const char *map_name,
	const char *suffix)
{
	snprintf(path, path_size, "evobot/nav/%s%s", map_name, suffix);
}

static int EvoBot_NavSafeMapName(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;

	if (!name || !name[0]) return 0;
	while (*cursor)
	{
		if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-') return 0;
		cursor++;
	}
	return 1;
}

static void EvoBot_NavParserWhitespace(evobot_nav_parser_t *parser)
{
	while (parser->cursor < parser->end &&
		isspace((unsigned char)*parser->cursor)) parser->cursor++;
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

static int EvoBot_NavParserString(evobot_nav_parser_t *parser, char *output,
	size_t output_size)
{
	size_t length = 0;

	if (!EvoBot_NavParserConsume(parser, '"')) return 0;
	while (parser->cursor < parser->end)
	{
		char value = *parser->cursor++;

		if (value == '"')
		{
			if (output_size) output[length < output_size ? length : output_size - 1] = '\0';
			return 1;
		}
		if (value == '\\')
		{
			if (parser->cursor >= parser->end) break;
			value = *parser->cursor++;
			switch (value)
			{
			case 'n': value = '\n'; break;
			case 'r': value = '\r'; break;
			case 't': value = '\t'; break;
			case 'b': value = '\b'; break;
			case 'f': value = '\f'; break;
			case 'u':
				if (parser->end - parser->cursor < 4) { parser->failed = 1; return 0; }
				parser->cursor += 4;
				value = '?';
				break;
			default: break;
			}
		}
		if (output_size && length + 1 < output_size) output[length] = value;
		length++;
	}
	parser->failed = 1;
	return 0;
}

static int EvoBot_NavParserNumber(evobot_nav_parser_t *parser, double *number)
{
	char *end;

	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor >= parser->end) return 0;
	*number = strtod(parser->cursor, &end);
	if (end == parser->cursor || end > parser->end)
	{
		parser->failed = 1;
		return 0;
	}
	parser->cursor = end;
	return 1;
}

static int EvoBot_NavParserBool(evobot_nav_parser_t *parser, int *value)
{
	EvoBot_NavParserWhitespace(parser);
	if (parser->end - parser->cursor >= 4 && !memcmp(parser->cursor, "true", 4))
	{
		parser->cursor += 4; *value = 1; return 1;
	}
	if (parser->end - parser->cursor >= 5 && !memcmp(parser->cursor, "false", 5))
	{
		parser->cursor += 5; *value = 0; return 1;
	}
	parser->failed = 1;
	return 0;
}

static int EvoBot_NavParserSkipValue(evobot_nav_parser_t *parser);

static int EvoBot_NavParserSkipObject(evobot_nav_parser_t *parser)
{
	char key[64];

	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return 1; }
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':') ||
			!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return 1; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserSkipArray(evobot_nav_parser_t *parser)
{
	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == ']') { parser->cursor++; return 1; }
	for (;;)
	{
		if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == ']') { parser->cursor++; return 1; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserSkipValue(evobot_nav_parser_t *parser)
{
	char temporary[2];
	double number;
	int boolean;

	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor >= parser->end) return 0;
	if (*parser->cursor == '{') return EvoBot_NavParserSkipObject(parser);
	if (*parser->cursor == '[') return EvoBot_NavParserSkipArray(parser);
	if (*parser->cursor == '"') return EvoBot_NavParserString(parser, temporary, sizeof(temporary));
	if (*parser->cursor == 't' || *parser->cursor == 'f') return EvoBot_NavParserBool(parser, &boolean);
	if (parser->end - parser->cursor >= 4 && !memcmp(parser->cursor, "null", 4))
	{
		parser->cursor += 4; return 1;
	}
	return EvoBot_NavParserNumber(parser, &number);
}

static int EvoBot_NavParserVec3(evobot_nav_parser_t *parser, evobot_vec3_t *vector)
{
	double number;
	int axis;

	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	for (axis = 0; axis < 3; axis++)
	{
		if (!EvoBot_NavParserNumber(parser, &number)) return 0;
		vector->v[axis] = (float)number;
		if (axis < 2 && !EvoBot_NavParserConsume(parser, ',')) return 0;
	}
	return EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParserBounds(evobot_nav_parser_t *parser,
	evobot_bounds_t *bounds)
{
	char key[64];
	int mins = 0;
	int maxs = 0;

	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "mins")) { if (!EvoBot_NavParserVec3(parser, &bounds->mins)) return 0; mins = 1; }
		else if (!strcmp(key, "maxs")) { if (!EvoBot_NavParserVec3(parser, &bounds->maxs)) return 0; maxs = 1; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; break; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
	return mins && maxs;
}

static int EvoBot_NavParserSegment(evobot_nav_parser_t *parser,
	evobot_nav_segment_t *segment)
{
	return EvoBot_NavParserConsume(parser, '[') &&
		EvoBot_NavParserVec3(parser, &segment->start) &&
		EvoBot_NavParserConsume(parser, ',') &&
		EvoBot_NavParserVec3(parser, &segment->end) &&
		EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParserPlane(evobot_nav_parser_t *parser,
	evobot_nav_plane_t *plane)
{
	char key[64];
	int normal = 0;
	int distance = 0;
	double number;

	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "normal")) { if (!EvoBot_NavParserVec3(parser, &plane->normal)) return 0; normal = 1; }
		else if (!strcmp(key, "distance")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; plane->distance = (float)number; distance = 1; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; break; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
	return normal && distance;
}

static int EvoBot_NavParserVertices(evobot_nav_parser_t *parser,
	evobot_vec3_t **vertices, size_t *count)
{
	size_t capacity = 0;

	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	EvoBot_NavParserWhitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == ']') { parser->cursor++; return 1; }
	for (;;)
	{
		if (!EvoBot_NavReserve((void **)vertices, &capacity, *count + 1,
			sizeof(**vertices)) ||
			!EvoBot_NavParserVec3(parser, &(*vertices)[*count])) return 0;
		(*count)++;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == ']') { parser->cursor++; return 1; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserFace(evobot_nav_parser_t *parser,
	evobot_nav_face_t *face)
{
	char key[64];
	char value[32];
	double number;

	memset(face, 0, sizeof(*face));
	face->portal = EVOBOT_NAV_INVALID_ID;
	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "plane")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; face->plane = (int)number; }
		else if (!strcmp(key, "type")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; face->kind = EvoBot_NavFaceFromName(value); }
		else if (!strcmp(key, "portal")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; face->portal = (int)number; }
		else if (!strcmp(key, "vertices")) { if (!EvoBot_NavParserVertices(parser, &face->vertices, &face->vertex_count)) return 0; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return face->vertex_count >= 3; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserArea(evobot_nav_parser_t *parser,
	evobot_nav_area_t *area)
{
	char key[64];
	char value[32];
	double number;
	size_t plane_capacity = 0;
	size_t face_capacity = 0;

	memset(area, 0, sizeof(*area));
	area->dynamic_interactor = EVOBOT_NAV_INVALID_ID;
	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "id")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; area->id = (uint32_t)number; }
		else if (!strcmp(key, "bounds")) { if (!EvoBot_NavParserBounds(parser, &area->bounds)) return 0; }
		else if (!strcmp(key, "contents")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; area->contents = EvoBot_NavContentsFromName(value); }
		else if (!strcmp(key, "supported")) { if (!EvoBot_NavParserBool(parser, &area->supported)) return 0; }
		else if (!strcmp(key, "floor_height")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; area->floor_height = (float)number; }
		else if (!strcmp(key, "support_distance")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; area->support_distance = (float)number; }
		else if (!strcmp(key, "support_normal")) { if (!EvoBot_NavParserVec3(parser, &area->support_normal)) return 0; }
		else if (!strcmp(key, "water_level")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; area->water_level = (int)number; }
		else if (!strcmp(key, "dynamic_interactor")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; area->dynamic_interactor = (int)number; }
		else if (!strcmp(key, "planes"))
		{
			if (!EvoBot_NavParserConsume(parser, '[')) return 0;
			EvoBot_NavParserWhitespace(parser);
			while (parser->cursor < parser->end && *parser->cursor != ']')
			{
				if (!EvoBot_NavReserve((void **)&area->planes, &plane_capacity,
					area->plane_count + 1, sizeof(*area->planes)) ||
					!EvoBot_NavParserPlane(parser, &area->planes[area->plane_count])) return 0;
				area->plane_count++;
				EvoBot_NavParserWhitespace(parser);
				if (*parser->cursor == ',') { parser->cursor++; EvoBot_NavParserWhitespace(parser); }
			}
			if (!EvoBot_NavParserConsume(parser, ']')) return 0;
		}
		else if (!strcmp(key, "faces"))
		{
			if (!EvoBot_NavParserConsume(parser, '[')) return 0;
			EvoBot_NavParserWhitespace(parser);
			while (parser->cursor < parser->end && *parser->cursor != ']')
			{
				if (!EvoBot_NavReserve((void **)&area->faces, &face_capacity,
					area->face_count + 1, sizeof(*area->faces)) ||
					!EvoBot_NavParserFace(parser, &area->faces[area->face_count])) return 0;
				area->face_count++;
				EvoBot_NavParserWhitespace(parser);
				if (*parser->cursor == ',') { parser->cursor++; EvoBot_NavParserWhitespace(parser); }
			}
			if (!EvoBot_NavParserConsume(parser, ']')) return 0;
		}
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return area->plane_count >= 4 && area->face_count >= 4; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserPortal(evobot_nav_parser_t *parser,
	evobot_nav_portal_t *portal)
{
	char key[64];
	char value[32];
	double number;

	memset(portal, 0, sizeof(*portal));
	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "id")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; portal->id = (uint32_t)number; }
		else if (!strcmp(key, "area_a")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; portal->area_a = (uint32_t)number; }
		else if (!strcmp(key, "area_b")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; portal->area_b = (uint32_t)number; }
		else if (!strcmp(key, "type")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; portal->kind = EvoBot_NavFaceFromName(value); }
		else if (!strcmp(key, "plane")) { if (!EvoBot_NavParserPlane(parser, &portal->plane)) return 0; }
		else if (!strcmp(key, "vertices")) { if (!EvoBot_NavParserVertices(parser, &portal->vertices, &portal->vertex_count)) return 0; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return portal->vertex_count >= 3; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserInteractor(evobot_nav_parser_t *parser,
	evobot_nav_interactor_t *interactor)
{
	char key[64];
	char value[EVOBOT_NAV_TARGET_MAX];
	double number;

	memset(interactor, 0, sizeof(*interactor));
	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "id")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->id = (uint32_t)number; }
		else if (!strcmp(key, "type")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; interactor->host.kind = EvoBot_NavInteractorFromName(value); }
		else if (!strcmp(key, "dynamic_brush")) { if (!EvoBot_NavParserBool(parser, &interactor->host.dynamic_brush)) return 0; }
		else if (!strcmp(key, "bounds")) { if (!EvoBot_NavParserBounds(parser, &interactor->host.bounds)) return 0; }
		else if (!strcmp(key, "swept_bounds")) { if (!EvoBot_NavParserBounds(parser, &interactor->host.swept_bounds)) return 0; }
		else if (!strcmp(key, "origin")) { if (!EvoBot_NavParserVec3(parser, &interactor->host.origin)) return 0; }
		else if (!strcmp(key, "angles")) { if (!EvoBot_NavParserVec3(parser, &interactor->host.angles)) return 0; }
		else if (!strcmp(key, "velocity")) { if (!EvoBot_NavParserVec3(parser, &interactor->host.velocity)) return 0; }
		else if (!strcmp(key, "has_angles")) { if (!EvoBot_NavParserBool(parser, &interactor->host.has_angles)) return 0; }
		else if (!strcmp(key, "has_velocity")) { if (!EvoBot_NavParserBool(parser, &interactor->host.has_velocity)) return 0; }
		else if (!strcmp(key, "has_movement")) { if (!EvoBot_NavParserBool(parser, &interactor->host.has_movement)) return 0; }
		else if (!strcmp(key, "endpoint_a")) { if (!EvoBot_NavParserVec3(parser, &interactor->host.endpoint_a)) return 0; }
		else if (!strcmp(key, "endpoint_b")) { if (!EvoBot_NavParserVec3(parser, &interactor->host.endpoint_b)) return 0; }
		else if (!strcmp(key, "endpoint_a_bounds")) { if (!EvoBot_NavParserBounds(parser, &interactor->host.endpoint_a_bounds)) return 0; }
		else if (!strcmp(key, "endpoint_b_bounds")) { if (!EvoBot_NavParserBounds(parser, &interactor->host.endpoint_b_bounds)) return 0; }
		else if (!strcmp(key, "speed")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.speed = (float)number; }
		else if (!strcmp(key, "wait")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.wait = (float)number; }
		else if (!strcmp(key, "travel_time")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.travel_time = (float)number; }
		else if (!strcmp(key, "spawnflags")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.spawnflags = (int)number; }
		else if (!strcmp(key, "health")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.health = (float)number; }
		else if (!strcmp(key, "activation")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.activation = (evobot_interactor_activation_t)(int)number; }
		else if (!strcmp(key, "lifetime")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.lifetime = (evobot_interactor_lifetime_t)(int)number; }
		else if (!strcmp(key, "activation_required_count")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; interactor->host.activation_required_count = (int)number; }
		else if (!strcmp(key, "classname")) { if (!EvoBot_NavParserString(parser, interactor->host.classname, sizeof(interactor->host.classname))) return 0; }
		else if (!strcmp(key, "model")) { if (!EvoBot_NavParserString(parser, interactor->host.model, sizeof(interactor->host.model))) return 0; }
		else if (!strcmp(key, "target")) { if (!EvoBot_NavParserString(parser, interactor->host.target, sizeof(interactor->host.target))) return 0; }
		else if (!strcmp(key, "targetname")) { if (!EvoBot_NavParserString(parser, interactor->host.targetname, sizeof(interactor->host.targetname))) return 0; }
		else if (!strcmp(key, "killtarget")) { if (!EvoBot_NavParserString(parser, interactor->host.killtarget, sizeof(interactor->host.killtarget))) return 0; }
		else if (!strcmp(key, "destination_map")) { if (!EvoBot_NavParserString(parser, interactor->host.destination_map, sizeof(interactor->host.destination_map))) return 0; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return 1; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserReachability(evobot_nav_parser_t *parser,
	evobot_nav_reachability_t *reachability)
{
	char key[64];
	char value[64];
	double number;

	memset(reachability, 0, sizeof(*reachability));
	reachability->dynamic_interactor = EVOBOT_NAV_INVALID_ID;
	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "id")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->id = (uint32_t)number; }
		else if (!strcmp(key, "source_area")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->source_area = (uint32_t)number; }
		else if (!strcmp(key, "destination_area")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->destination_area = (uint32_t)number; }
		else if (!strcmp(key, "travel_type")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; reachability->travel_type = EvoBot_NavTravelFromName(value); }
		else if (!strcmp(key, "portal_id")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->portal_id = (uint32_t)number; }
		else if (!strcmp(key, "source_face")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->source_face = (uint32_t)number; }
		else if (!strcmp(key, "source_edge")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->source_edge = (uint32_t)number; }
		else if (!strcmp(key, "start")) { if (!EvoBot_NavParserSegment(parser, &reachability->start)) return 0; }
		else if (!strcmp(key, "destination")) { if (!EvoBot_NavParserSegment(parser, &reachability->destination)) return 0; }
		else if (!strcmp(key, "height_delta")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->height_delta = (float)number; }
		else if (!strcmp(key, "horizontal_distance")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->horizontal_distance = (float)number; }
		else if (!strcmp(key, "travel_distance")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->travel_distance = (float)number; }
		else if (!strcmp(key, "base_travel_time")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->base_travel_time = (float)number; }
		else if (!strcmp(key, "source_contents")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; reachability->source_contents = EvoBot_NavContentsFromName(value); }
		else if (!strcmp(key, "destination_contents")) { if (!EvoBot_NavParserString(parser, value, sizeof(value))) return 0; reachability->destination_contents = EvoBot_NavContentsFromName(value); }
		else if (!strcmp(key, "flags")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->flags = (uint32_t)number; }
		else if (!strcmp(key, "dynamic_interactor")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->dynamic_interactor = (int)number; }
		else if (!strcmp(key, "source_interactor")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->source_interactor = (uint32_t)number; }
		else if (!strcmp(key, "destination_interactor")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->destination_interactor = (uint32_t)number; }
		else if (!strcmp(key, "entry_bounds")) { if (!EvoBot_NavParserBounds(parser, &reachability->entry_bounds)) return 0; }
		else if (!strcmp(key, "arrival_origin")) { if (!EvoBot_NavParserVec3(parser, &reachability->arrival_origin)) return 0; }
		else if (!strcmp(key, "arrival_angles")) { if (!EvoBot_NavParserVec3(parser, &reachability->arrival_angles)) return 0; }
		else if (!strcmp(key, "arrival_velocity")) { if (!EvoBot_NavParserVec3(parser, &reachability->arrival_velocity)) return 0; }
		else if (!strcmp(key, "mover_interactor")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->mover_interactor = (uint32_t)number; }
		else if (!strcmp(key, "board_region")) { if (!EvoBot_NavParserBounds(parser, &reachability->board_region)) return 0; }
		else if (!strcmp(key, "standing_region")) { if (!EvoBot_NavParserBounds(parser, &reachability->standing_region)) return 0; }
		else if (!strcmp(key, "exit_region")) { if (!EvoBot_NavParserBounds(parser, &reachability->exit_region)) return 0; }
		else if (!strcmp(key, "expected_wait_time")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->expected_wait_time = (float)number; }
		else if (!strcmp(key, "ride_time")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; reachability->ride_time = (float)number; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}')
		{
			parser->cursor++;
			return reachability->id && reachability->source_area &&
				reachability->destination_area &&
				(reachability->portal_id ||
				 reachability->travel_type == EVOBOT_NAV_TRAVEL_TELEPORT ||
				 reachability->travel_type == EVOBOT_NAV_TRAVEL_PLATFORM);
		}
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserMap(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	char key[64];
	double number;

	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':')) return 0;
		if (!strcmp(key, "name")) { if (!EvoBot_NavParserString(parser, dataset->map_name, sizeof(dataset->map_name))) return 0; }
		else if (!strcmp(key, "checksum")) { if (!EvoBot_NavParserNumber(parser, &number)) return 0; dataset->map_checksum = (uint32_t)number; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return 1; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserGeneration(evobot_nav_parser_t *parser,
	evobot_nav_stats_t *stats)
{
	char key[64];
	double number;

	if (!EvoBot_NavParserConsume(parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(parser, ':') ||
			!EvoBot_NavParserNumber(parser, &number)) return 0;
		if (!strcmp(key, "initial_areas")) stats->initial_areas = (uint64_t)number;
		else if (!strcmp(key, "content_splits")) stats->content_splits = (uint64_t)number;
		else if (!strcmp(key, "gravity_splits")) stats->gravity_splits = (uint64_t)number;
		else if (!strcmp(key, "merges")) stats->merges = (uint64_t)number;
		else if (!strcmp(key, "validation_traces")) stats->validation_traces = (uint64_t)number;
		else if (!strcmp(key, "movement_tests")) stats->movement_tests = (uint64_t)number;
		else if (!strcmp(key, "teleporter_triggers")) stats->teleporter_triggers = (uint64_t)number;
		else if (!strcmp(key, "resolved_teleporters")) stats->resolved_teleporters = (uint64_t)number;
		else if (!strcmp(key, "teleport_links")) stats->teleport_links = (uint64_t)number;
		else if (!strcmp(key, "edge_drop_seeds")) stats->edge_drop_seeds = (uint64_t)number;
		else if (!strcmp(key, "edge_drop_links")) stats->edge_drop_links = (uint64_t)number;
		else if (!strcmp(key, "gap_jump_seeds")) stats->gap_jump_seeds = (uint64_t)number;
		else if (!strcmp(key, "gap_jump_links")) stats->gap_jump_links = (uint64_t)number;
		else if (!strcmp(key, "gap_jump_rejected_start")) stats->gap_jump_rejected_start = (uint64_t)number;
		else if (!strcmp(key, "gap_jump_rejected_air")) stats->gap_jump_rejected_air = (uint64_t)number;
		else if (!strcmp(key, "gap_jump_rejected_landing")) stats->gap_jump_rejected_landing = (uint64_t)number;
		else if (!strcmp(key, "gap_jump_rejected_edge")) stats->gap_jump_rejected_edge = (uint64_t)number;
		else if (!strcmp(key, "jump_up_ledge_candidates")) stats->jump_up_ledge_candidates = (uint64_t)number;
		else if (!strcmp(key, "jump_up_ledge_validated")) stats->jump_up_ledge_validated = (uint64_t)number;
		else if (!strcmp(key, "jump_up_ledge_links")) stats->jump_up_ledge_links = (uint64_t)number;
		else if (!strcmp(key, "jump_up_ledge_attempts")) stats->jump_up_ledge_attempts = (uint64_t)number;
		else if (!strcmp(key, "water_jump_candidates")) stats->water_jump_candidates = (uint64_t)number;
		else if (!strcmp(key, "water_jump_validated")) stats->water_jump_validated = (uint64_t)number;
		else if (!strcmp(key, "water_jump_links")) stats->water_jump_links = (uint64_t)number;
		else if (!strcmp(key, "water_jump_attempts")) stats->water_jump_attempts = (uint64_t)number;
		else if (!strcmp(key, "platform_movers")) stats->platform_movers = (uint64_t)number;
		else if (!strcmp(key, "platform_links")) stats->platform_links = (uint64_t)number;
		else if (!strcmp(key, "activation_relationships")) stats->activation_relationships = (uint64_t)number;
		else if (!strcmp(key, "seconds")) stats->generation_time = number;
		else if (!strcmp(key, "reachability_seconds")) stats->reachability_time = number;
		else if (!strcmp(key, "teleport_seconds")) stats->teleport_time = number;
		else if (!strcmp(key, "edge_drop_seconds")) stats->edge_drop_time = number;
		else if (!strcmp(key, "gap_jump_seconds")) stats->gap_jump_time = number;
		else if (!strcmp(key, "jump_up_ledge_seconds")) stats->jump_up_ledge_time = number;
		else if (!strcmp(key, "water_jump_seconds")) stats->water_jump_time = number;
		else if (!strcmp(key, "platform_seconds")) stats->platform_time = number;
		else if (!strcmp(key, "activation_graph_seconds")) stats->activation_graph_time = number;
		else if (!strcmp(key, "dynamic_analysis_seconds")) stats->dynamic_analysis_time = number;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return 1; }
		if (!EvoBot_NavParserConsume(parser, ',')) return 0;
	}
}

static int EvoBot_NavParserAreaArray(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	EvoBot_NavParserWhitespace(parser);
	while (parser->cursor < parser->end && *parser->cursor != ']')
	{
		evobot_nav_area_t area;

		memset(&area, 0, sizeof(area));
		if (!EvoBot_NavParserArea(parser, &area) ||
			!EvoBot_NavAppendArea(dataset, &area))
		{
			EvoBot_NavFreeArea(&area);
			return 0;
		}
		EvoBot_NavParserWhitespace(parser);
		if (*parser->cursor == ',') { parser->cursor++; EvoBot_NavParserWhitespace(parser); }
	}
	return EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParserPortalArray(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	EvoBot_NavParserWhitespace(parser);
	while (parser->cursor < parser->end && *parser->cursor != ']')
	{
		evobot_nav_portal_t portal;

		memset(&portal, 0, sizeof(portal));
		if (!EvoBot_NavParserPortal(parser, &portal) ||
			!EvoBot_NavReserve((void **)&dataset->portals, &dataset->portal_capacity,
				dataset->portal_count + 1, sizeof(*dataset->portals)))
		{
			free(portal.vertices);
			return 0;
		}
		dataset->portals[dataset->portal_count++] = portal;
		EvoBot_NavParserWhitespace(parser);
		if (*parser->cursor == ',') { parser->cursor++; EvoBot_NavParserWhitespace(parser); }
	}
	return EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParserInteractorArray(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	EvoBot_NavParserWhitespace(parser);
	while (parser->cursor < parser->end && *parser->cursor != ']')
	{
		evobot_nav_interactor_t interactor;

		if (!EvoBot_NavParserInteractor(parser, &interactor) ||
			!EvoBot_NavReserve((void **)&dataset->interactors,
				&dataset->interactor_capacity, dataset->interactor_count + 1,
				sizeof(*dataset->interactors))) return 0;
		dataset->interactors[dataset->interactor_count++] = interactor;
		EvoBot_NavParserWhitespace(parser);
		if (*parser->cursor == ',') { parser->cursor++; EvoBot_NavParserWhitespace(parser); }
	}
	return EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParserReachabilityArray(evobot_nav_parser_t *parser,
	evobot_nav_dataset_t *dataset)
{
	if (!EvoBot_NavParserConsume(parser, '[')) return 0;
	EvoBot_NavParserWhitespace(parser);
	while (parser->cursor < parser->end && *parser->cursor != ']')
	{
		evobot_nav_reachability_t reachability;

		if (!EvoBot_NavParserReachability(parser, &reachability) ||
			!EvoBot_NavReserve((void **)&dataset->reachabilities,
				&dataset->reachability_capacity, dataset->reachability_count + 1,
				sizeof(*dataset->reachabilities))) return 0;
		dataset->reachabilities[dataset->reachability_count++] = reachability;
		EvoBot_NavParserWhitespace(parser);
		if (*parser->cursor == ',') { parser->cursor++; EvoBot_NavParserWhitespace(parser); }
	}
	return EvoBot_NavParserConsume(parser, ']');
}

static int EvoBot_NavParseJson(const char *data, size_t size,
	evobot_nav_dataset_t *dataset, int *version)
{
	evobot_nav_parser_t parser;
	char key[64];
	char format[64] = "";
	double number;

	memset(&parser, 0, sizeof(parser));
	parser.cursor = data;
	parser.end = data + size;
	*version = 0;
	if (!EvoBot_NavParserConsume(&parser, '{')) return 0;
	for (;;)
	{
		if (!EvoBot_NavParserString(&parser, key, sizeof(key)) ||
			!EvoBot_NavParserConsume(&parser, ':')) return 0;
		if (!strcmp(key, "format")) { if (!EvoBot_NavParserString(&parser, format, sizeof(format))) return 0; }
		else if (!strcmp(key, "version")) { if (!EvoBot_NavParserNumber(&parser, &number)) return 0; *version = (int)number; }
		else if (!strcmp(key, "map")) { if (!EvoBot_NavParserMap(&parser, dataset)) return 0; }
		else if (!strcmp(key, "world_bounds")) { if (!EvoBot_NavParserBounds(&parser, &dataset->world_bounds)) return 0; }
		else if (!strcmp(key, "player_bounds")) { if (!EvoBot_NavParserBounds(&parser, &dataset->player_bounds)) return 0; }
		else if (!strcmp(key, "generation")) { if (!EvoBot_NavParserGeneration(&parser, &dataset->stats)) return 0; }
		else if (!strcmp(key, "areas")) { if (!EvoBot_NavParserAreaArray(&parser, dataset)) return 0; }
		else if (!strcmp(key, "portals")) { if (!EvoBot_NavParserPortalArray(&parser, dataset)) return 0; }
		else if (!strcmp(key, "interactors")) { if (!EvoBot_NavParserInteractorArray(&parser, dataset)) return 0; }
		else if (!strcmp(key, "reachabilities")) { if (!EvoBot_NavParserReachabilityArray(&parser, dataset)) return 0; }
		else if (!EvoBot_NavParserSkipValue(&parser)) return 0;
		EvoBot_NavParserWhitespace(&parser);
		if (parser.cursor < parser.end && *parser.cursor == '}') { parser.cursor++; break; }
		if (!EvoBot_NavParserConsume(&parser, ',')) return 0;
	}
	EvoBot_NavParserWhitespace(&parser);
	return !parser.failed && parser.cursor == parser.end &&
		!strcmp(format, "evobot-nav") && dataset->map_name[0];
}

void EvoBot_NavConvexSave(void)
{
	evobot_nav_buffer_t buffer;
	char path[EVOBOT_NAV_MAP_MAX + 32];

	if (!EvoBot_NavMapReady()) return;
	if (evobot_nav.state == EVOBOT_NAV_STATE_EMPTY || !evobot_nav.area_count)
	{
		EvoBot_NavPrint("EvoBot navigation: there is no generated data to save\n");
		return;
	}
	memset(&buffer, 0, sizeof(buffer));
	if (!EvoBot_NavSerializeJson(&buffer) || buffer.failed ||
		!evobot_nav_host.write_file)
	{
		free(buffer.data);
		EvoBot_NavPrintf("EvoBot navigation: could not serialize version %d data\n",
			EVOBOT_NAV_FORMAT_VERSION);
		return;
	}
	EvoBot_NavPath(path, sizeof(path), evobot_nav.map_name, ".botnav");
	if (!evobot_nav_host.write_file(path, buffer.data, buffer.length))
	{
		free(buffer.data);
		EvoBot_NavPrintf("EvoBot navigation: could not write %s\n", path);
		return;
	}
	EvoBot_NavPrintf("EvoBot navigation saved: %s (%zu bytes)\n", path,
		buffer.length);
	free(buffer.data);
}

void EvoBot_NavConvexLoad(const char *source_map)
{
	char map_name[EVOBOT_NAV_MAP_MAX];
	char path[EVOBOT_NAV_MAP_MAX + 32];
	char *data;
	size_t size;
	evobot_nav_dataset_t loaded;
	int version;

	if (!EvoBot_NavMapReady()) return;
	snprintf(map_name, sizeof(map_name), "%s",
		source_map && source_map[0] ? source_map : evobot_nav_active_map);
	if (!EvoBot_NavSafeMapName(map_name))
	{
		EvoBot_NavPrint("EvoBot navigation: invalid map name\n");
		return;
	}
	EvoBot_NavPath(path, sizeof(path), map_name, ".botnav");
	if (!evobot_nav_host.file_size || !evobot_nav_host.read_file ||
		!evobot_nav_host.file_size(path, &size))
	{
		EvoBot_NavPrintf("EvoBot navigation: could not find %s\n", path);
		return;
	}
	if (!size || size > EVOBOT_NAV_MAX_FILE_SIZE)
	{
		EvoBot_NavPrintf("EvoBot navigation: invalid file size for %s\n", path);
		return;
	}
	data = malloc(size + 1);
	if (!data || !evobot_nav_host.read_file(path, data, size))
	{
		free(data);
		EvoBot_NavPrintf("EvoBot navigation: could not read %s\n", path);
		return;
	}
	data[size] = '\0';
	memset(&loaded, 0, sizeof(loaded));
	if (!EvoBot_NavParseJson(data, size, &loaded, &version))
	{
		free(data);
		EvoBot_NavFreeDataset(&loaded);
		EvoBot_NavPrintf("EvoBot navigation: invalid navigation data in %s\n", path);
		return;
	}
	free(data);
	if (version != EVOBOT_NAV_FORMAT_VERSION)
	{
		EvoBot_NavPrintf("EvoBot navigation: unsupported navigation version %d (expected %d)\n",
			version, EVOBOT_NAV_FORMAT_VERSION);
		EvoBot_NavFreeDataset(&loaded);
		return;
	}
	if (strcmp(loaded.map_name, evobot_nav_active_map) ||
		loaded.map_checksum != evobot_nav_active_checksum)
	{
		EvoBot_NavPrintf("EvoBot navigation: map mismatch (file %s/%" PRIu32
			", active %s/%" PRIu32 ")\n", loaded.map_name,
			loaded.map_checksum, evobot_nav_active_map,
			evobot_nav_active_checksum);
		EvoBot_NavFreeDataset(&loaded);
		return;
	}
	loaded.state = EVOBOT_NAV_STATE_LOADED;
	EvoBot_NavFreeDataset(&evobot_nav);
	evobot_nav = loaded;
	EvoBot_NavTopologyChanged();
	EvoBot_NavPrintf("EvoBot navigation loaded: %s (%zu areas, %zu portals, "
		"%zu reachabilities)\n", path, evobot_nav.area_count,
		evobot_nav.portal_count, evobot_nav.reachability_count);
}

static evobot_nav_debug_face_kind_t EvoBot_NavDebugFaceKind(
	evobot_nav_face_kind_t kind)
{
	switch (kind)
	{
	case EVOBOT_NAV_FACE_PORTAL:
		return EVOBOT_NAV_DEBUG_FACE_PORTAL;
	case EVOBOT_NAV_FACE_LIQUID:
		return EVOBOT_NAV_DEBUG_FACE_LIQUID;
	case EVOBOT_NAV_FACE_LEDGE:
		return EVOBOT_NAV_DEBUG_FACE_LEDGE;
	case EVOBOT_NAV_FACE_SOLID:
	default:
		return EVOBOT_NAV_DEBUG_FACE_SOLID;
	}
}

uint64_t EvoBot_NavDebugRevision(void)
{
	return evobot_nav_revision;
}

int EvoBot_NavDebugSummary(evobot_nav_debug_summary_t *summary)
{
	if (!summary)
		return 0;
	memset(summary, 0, sizeof(*summary));
	summary->revision = evobot_nav_revision;
	summary->present = evobot_nav.state != EVOBOT_NAV_STATE_EMPTY &&
		evobot_nav.area_count != 0;
	summary->area_count = evobot_nav.area_count;
	summary->portal_count = evobot_nav.portal_count;
	summary->interactor_count = evobot_nav.interactor_count;
	summary->reachability_count = evobot_nav.reachability_count;
	summary->world_bounds = evobot_nav.world_bounds;
	summary->player_bounds = evobot_nav.player_bounds;
	return 1;
}

int EvoBot_NavDebugArea(size_t index, evobot_nav_debug_area_t *result)
{
	const evobot_nav_area_t *area;
	size_t i;

	if (!result || index >= evobot_nav.area_count)
		return 0;
	area = &evobot_nav.areas[index];
	memset(result, 0, sizeof(*result));
	result->id = area->id;
	result->bounds = area->bounds;
	EvoBot_NavAreaCenter(area, &result->center);
	result->contents = area->contents;
	result->supported = area->supported;
	result->floor_height = area->floor_height;
	result->support_distance = area->support_distance;
	result->support_normal = area->support_normal;
	result->water_level = area->water_level;
	result->dynamic_interactor = area->dynamic_interactor;
	result->face_count = area->face_count;
	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		if (evobot_nav.portals[i].area_a == area->id ||
			evobot_nav.portals[i].area_b == area->id)
			result->portal_count++;
	}
	return 1;
}

int EvoBot_NavDebugAreaFace(size_t area_index, size_t face_index,
	evobot_nav_debug_face_t *result)
{
	const evobot_nav_area_t *area;
	const evobot_nav_face_t *face;

	if (!result || area_index >= evobot_nav.area_count)
		return 0;
	area = &evobot_nav.areas[area_index];
	if (face_index >= area->face_count)
		return 0;
	face = &area->faces[face_index];
	memset(result, 0, sizeof(*result));
	result->kind = EvoBot_NavDebugFaceKind(face->kind);
	result->portal_id = face->portal >= 0 ? (uint32_t)face->portal : 0;
	result->normal = area->planes[face->plane].normal;
	result->distance = area->planes[face->plane].distance;
	result->vertex_count = face->vertex_count;
	return 1;
}

int EvoBot_NavDebugAreaFaceVertex(size_t area_index, size_t face_index,
	size_t vertex_index, evobot_vec3_t *vertex)
{
	const evobot_nav_area_t *area;
	const evobot_nav_face_t *face;

	if (!vertex || area_index >= evobot_nav.area_count)
		return 0;
	area = &evobot_nav.areas[area_index];
	if (face_index >= area->face_count)
		return 0;
	face = &area->faces[face_index];
	if (vertex_index >= face->vertex_count)
		return 0;
	*vertex = face->vertices[vertex_index];
	return 1;
}

int EvoBot_NavDebugPortal(size_t index, evobot_nav_debug_portal_t *result)
{
	const evobot_nav_portal_t *portal;

	if (!result || index >= evobot_nav.portal_count)
		return 0;
	portal = &evobot_nav.portals[index];
	memset(result, 0, sizeof(*result));
	result->id = portal->id;
	result->area_a = portal->area_a;
	result->area_b = portal->area_b;
	result->kind = EvoBot_NavDebugFaceKind(portal->kind);
	result->normal = portal->plane.normal;
	result->distance = portal->plane.distance;
	result->vertex_count = portal->vertex_count;
	return 1;
}

int EvoBot_NavDebugPortalVertex(size_t portal_index, size_t vertex_index,
	evobot_vec3_t *vertex)
{
	const evobot_nav_portal_t *portal;

	if (!vertex || portal_index >= evobot_nav.portal_count)
		return 0;
	portal = &evobot_nav.portals[portal_index];
	if (vertex_index >= portal->vertex_count)
		return 0;
	*vertex = portal->vertices[vertex_index];
	return 1;
}

int EvoBot_NavDebugWalkCandidate(uint32_t portal_id, uint32_t source_area,
	uint32_t destination_area, evobot_nav_debug_walk_candidate_t *candidate)
{
	const evobot_nav_portal_t *portal;
	evobot_nav_area_t *source;
	evobot_nav_area_t *destination;
	evobot_player_physics_t physics;

	if (!candidate)
		return 0;
	memset(candidate, 0, sizeof(*candidate));
	if (!portal_id || portal_id > evobot_nav.portal_count)
		return 0;
	portal = &evobot_nav.portals[portal_id - 1];
	if (portal->id != portal_id ||
		!((portal->area_a == source_area && portal->area_b == destination_area) ||
		  (portal->area_b == source_area && portal->area_a == destination_area)))
		return 0;
	source = EvoBot_NavAreaById(source_area);
	destination = EvoBot_NavAreaById(destination_area);
	candidate->portal_id = portal_id;
	candidate->source_area = source_area;
	candidate->destination_area = destination_area;
	if (!source || !destination || !evobot_nav_host.player_physics ||
		!evobot_nav_host.player_physics(&physics))
		return 1;
	return EvoBot_NavEvaluateWalkDirection(portal, source, destination,
		&physics, 0, 0, candidate);
}

const char *EvoBot_NavDebugAirRejectionName(
	evobot_nav_debug_air_rejection_t rejection)
{
	switch (rejection)
	{
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NONE: return "none";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NOT_LEDGE: return "not-ledge";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_UNSUPPORTED_SOURCE: return "unsupported-source";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_DIRECTION: return "no-direction";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_HEIGHT: return "height";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_EDGE: return "no-horizontal-edge";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_START: return "no-supported-start";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_PM_FAILURE: return "pm-failure";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_AIRBORNE: return "no-airborne";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_LANDING: return "no-landing";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_HEADROOM: return "no-headroom";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_WRONG_LANDING: return "wrong-landing";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NOT_WATER_EXIT: return "not-water-exit";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_WATER_JUMP: return "no-water-jump";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_NO_WATER_EXIT: return "no-water-exit";
	case EVOBOT_NAV_DEBUG_AIR_REJECT_WATER_JUMP_OFF_PORTAL:
		return "water-jump-off-portal";
	}
	return "unknown";
}

int EvoBot_NavDebugAirCandidate(uint32_t portal_id, uint32_t source_area,
	uint32_t destination_area, evobot_nav_debug_air_kind_t kind,
	evobot_nav_debug_air_candidate_t *candidate)
{
	const evobot_nav_portal_t *portal;
	evobot_nav_area_t *source;
	evobot_nav_area_t *destination;
	evobot_player_physics_t physics;

	if (!candidate)
		return 0;
	memset(candidate, 0, sizeof(*candidate));
	if (!portal_id || portal_id > evobot_nav.portal_count ||
		(kind != EVOBOT_NAV_DEBUG_AIR_DROP &&
		 kind != EVOBOT_NAV_DEBUG_AIR_JUMP_UP &&
		 kind != EVOBOT_NAV_DEBUG_AIR_WATER_EXIT &&
		 kind != EVOBOT_NAV_DEBUG_AIR_WATER_JUMP))
		return 0;
	portal = &evobot_nav.portals[portal_id - 1];
	if (portal->id != portal_id ||
		!((portal->area_a == source_area && portal->area_b == destination_area) ||
		  (portal->area_b == source_area && portal->area_a == destination_area)))
		return 0;
	source = EvoBot_NavAreaById(source_area);
	destination = EvoBot_NavAreaById(destination_area);
	if (!source || !destination || !evobot_nav_host.player_physics ||
		!evobot_nav_host.player_physics(&physics))
		return 0;
	if (kind == EVOBOT_NAV_DEBUG_AIR_WATER_JUMP)
		return EvoBot_NavEvaluateWaterJumpDirection(portal, source, destination,
			&physics, 0, candidate);
	if (kind == EVOBOT_NAV_DEBUG_AIR_WATER_EXIT)
		return EvoBot_NavEvaluateWaterExitDirection(portal, source, destination,
			&physics, 0, candidate);
	return EvoBot_NavEvaluateAirDirection(portal, source, destination, &physics,
		kind, 0, candidate);
}

int EvoBot_NavDebugInteractor(size_t index,
	evobot_nav_debug_interactor_t *result)
{
	const evobot_nav_interactor_t *interactor;

	if (!result || index >= evobot_nav.interactor_count)
		return 0;
	interactor = &evobot_nav.interactors[index];
	memset(result, 0, sizeof(*result));
	result->id = interactor->id;
	result->kind = interactor->host.kind;
	result->dynamic_brush = interactor->host.dynamic_brush;
	result->bounds = interactor->host.bounds;
	result->swept_bounds = interactor->host.swept_bounds;
	result->origin = interactor->host.origin;
	result->has_movement = interactor->host.has_movement;
	result->endpoint_a = interactor->host.endpoint_a;
	result->endpoint_b = interactor->host.endpoint_b;
	result->endpoint_a_bounds = interactor->host.endpoint_a_bounds;
	result->endpoint_b_bounds = interactor->host.endpoint_b_bounds;
	result->speed = interactor->host.speed;
	result->wait = interactor->host.wait;
	result->travel_time = interactor->host.travel_time;
	result->spawnflags = interactor->host.spawnflags;
	result->health = interactor->host.health;
	result->activation = interactor->host.activation;
	result->lifetime = interactor->host.lifetime;
	result->activation_required_count = interactor->host.activation_required_count;
	result->current_state = EVOBOT_INTERACTOR_STATE_UNKNOWN;
	if (evobot_nav_host.interactor_state)
	{
		evobot_host_interactor_state_t state;

		memset(&state, 0, sizeof(state));
		if (evobot_nav_host.interactor_state(&interactor->host, &state))
			result->current_state = state.state;
	}
	snprintf(result->classname, sizeof(result->classname), "%s",
		interactor->host.classname);
	snprintf(result->model, sizeof(result->model), "%s", interactor->host.model);
	snprintf(result->target, sizeof(result->target), "%s", interactor->host.target);
	snprintf(result->targetname, sizeof(result->targetname), "%s",
		interactor->host.targetname);
	snprintf(result->killtarget, sizeof(result->killtarget), "%s",
		interactor->host.killtarget);
	snprintf(result->destination_map, sizeof(result->destination_map), "%s",
		interactor->host.destination_map);
	return 1;
}

size_t EvoBot_NavDebugActivationCount(void)
{
	size_t count = 0;
	size_t source;

	for (source = 0; source < evobot_nav.interactor_count; source++)
	{
		size_t destination;

		if (!evobot_nav.interactors[source].host.target[0])
			continue;
		for (destination = 0; destination < evobot_nav.interactor_count;
			destination++)
			if (evobot_nav.interactors[destination].host.targetname[0] &&
				!strcmp(evobot_nav.interactors[source].host.target,
					evobot_nav.interactors[destination].host.targetname))
				count++;
	}
	return count;
}

int EvoBot_NavDebugActivation(size_t index,
	evobot_nav_debug_activation_t *result)
{
	size_t current = 0;
	size_t source;

	if (!result)
		return 0;
	for (source = 0; source < evobot_nav.interactor_count; source++)
	{
		size_t destination;

		if (!evobot_nav.interactors[source].host.target[0])
			continue;
		for (destination = 0; destination < evobot_nav.interactor_count;
			destination++)
		{
			if (!evobot_nav.interactors[destination].host.targetname[0] ||
				strcmp(evobot_nav.interactors[source].host.target,
					evobot_nav.interactors[destination].host.targetname))
				continue;
			if (current++ != index)
				continue;
			memset(result, 0, sizeof(*result));
			result->source_interactor = evobot_nav.interactors[source].id;
			result->destination_interactor =
				evobot_nav.interactors[destination].id;
			snprintf(result->target, sizeof(result->target), "%s",
				evobot_nav.interactors[source].host.target);
			return 1;
		}
	}
	return 0;
}

int EvoBot_NavDebugReachability(size_t index,
	evobot_nav_reachability_t *result)
{
	if (!result || index >= evobot_nav.reachability_count)
		return 0;
	*result = evobot_nav.reachabilities[index];
	return 1;
}

int EvoBot_NavDebugFindArea(const evobot_vec3_t *point, uint32_t *area_id)
{
	size_t i;

	if (!point || !area_id)
		return 0;
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		if (EvoBot_NavPointInside(&evobot_nav.areas[i], point,
			EVOBOT_NAV_PLANE_EPSILON))
		{
			*area_id = evobot_nav.areas[i].id;
			return 1;
		}
	}
	return 0;
}

evobot_dynamic_blocker_state_t EvoBot_NavDebugBlockerState(
	int interactor_id, const evobot_bounds_t *crossing_bounds)
{
	const evobot_nav_interactor_t *interactor;

	if (interactor_id <= 0 || (size_t)interactor_id > evobot_nav.interactor_count ||
		!crossing_bounds || !evobot_nav_host.dynamic_blocker_state)
		return EVOBOT_DYNAMIC_BLOCKER_UNKNOWN;
	interactor = &evobot_nav.interactors[interactor_id - 1];
	if (!interactor->host.dynamic_brush)
		return EVOBOT_DYNAMIC_BLOCKER_CLEAR;
	return evobot_nav_host.dynamic_blocker_state(&interactor->host,
		crossing_bounds);
}

static int EvoBot_NavObjPolygon(evobot_nav_buffer_t *buffer,
	const evobot_vec3_t *vertices, size_t vertex_count, size_t *vertex_base,
	int face)
{
	size_t i;

	for (i = 0; i < vertex_count; i++)
	{
		if (!EvoBot_NavBufferAppendFormat(buffer, "v %.9g %.9g %.9g\n",
			vertices[i].v[0], vertices[i].v[1], vertices[i].v[2])) return 0;
	}
	if (!EvoBot_NavBufferAppend(buffer, face ? "f" : "l")) return 0;
	for (i = 0; i < vertex_count; i++)
	{
		if (!EvoBot_NavBufferAppendFormat(buffer, " %zu", *vertex_base + i))
			return 0;
	}
	if (!face && !EvoBot_NavBufferAppendFormat(buffer, " %zu", *vertex_base))
		return 0;
	if (!EvoBot_NavBufferAppend(buffer, "\n")) return 0;
	*vertex_base += vertex_count;
	return 1;
}

static int EvoBot_NavObjAreaGroup(evobot_nav_buffer_t *buffer,
	const char *name, evobot_contents_t contents, int support_filter,
	size_t *vertex_base)
{
	size_t i;

	if (!EvoBot_NavBufferAppendFormat(buffer, "g %s\n", name)) return 0;
	for (i = 0; i < evobot_nav.area_count; i++)
	{
		const evobot_nav_area_t *area = &evobot_nav.areas[i];
		size_t face;

		if (area->contents != contents ||
			(support_filter >= 0 && area->supported != support_filter)) continue;
		for (face = 0; face < area->face_count; face++)
		{
			if (!EvoBot_NavObjPolygon(buffer, area->faces[face].vertices,
				area->faces[face].vertex_count, vertex_base, 1)) return 0;
		}
	}
	return 1;
}

static int EvoBot_NavObjPortalGroup(evobot_nav_buffer_t *buffer,
	const char *name, evobot_nav_face_kind_t kind, size_t *vertex_base)
{
	size_t i;

	if (!EvoBot_NavBufferAppendFormat(buffer, "g %s\n", name)) return 0;
	for (i = 0; i < evobot_nav.portal_count; i++)
	{
		if (evobot_nav.portals[i].kind != kind) continue;
		if (!EvoBot_NavObjPolygon(buffer, evobot_nav.portals[i].vertices,
			evobot_nav.portals[i].vertex_count, vertex_base, 0)) return 0;
	}
	return 1;
}

void EvoBot_NavConvexExportObj(void)
{
	evobot_nav_buffer_t buffer;
	char path[EVOBOT_NAV_MAP_MAX + 48];
	size_t vertex_base = 1;

	if (!EvoBot_NavMapReady()) return;
	if (evobot_nav.state == EVOBOT_NAV_STATE_EMPTY || !evobot_nav.area_count)
	{
		EvoBot_NavPrint("EvoBot navigation: there is no generated data to export\n");
		return;
	}
	memset(&buffer, 0, sizeof(buffer));
	if (!EvoBot_NavBufferAppend(&buffer, "# EvoBot convex navigation format 2\no evobot_navigation\n") ||
		!EvoBot_NavObjAreaGroup(&buffer, "supported_areas", EVOBOT_CONTENTS_AIR,
			1, &vertex_base) ||
		!EvoBot_NavObjAreaGroup(&buffer, "unsupported_open_air", EVOBOT_CONTENTS_AIR,
			0, &vertex_base) ||
		!EvoBot_NavObjAreaGroup(&buffer, "water", EVOBOT_CONTENTS_WATER,
			-1, &vertex_base) ||
		!EvoBot_NavObjAreaGroup(&buffer, "slime", EVOBOT_CONTENTS_SLIME,
			-1, &vertex_base) ||
		!EvoBot_NavObjAreaGroup(&buffer, "lava", EVOBOT_CONTENTS_LAVA,
			-1, &vertex_base) ||
		!EvoBot_NavObjPortalGroup(&buffer, "portals", EVOBOT_NAV_FACE_PORTAL,
			&vertex_base) ||
		!EvoBot_NavObjPortalGroup(&buffer, "liquid_boundaries", EVOBOT_NAV_FACE_LIQUID,
			&vertex_base) ||
		!EvoBot_NavObjPortalGroup(&buffer, "ledge_boundaries", EVOBOT_NAV_FACE_LEDGE,
			&vertex_base) || buffer.failed || !evobot_nav_host.write_file)
	{
		free(buffer.data);
		EvoBot_NavPrint("EvoBot navigation: could not build convex OBJ\n");
		return;
	}
	snprintf(path, sizeof(path), "evobot/nav/debug/%s.obj", evobot_nav.map_name);
	if (!evobot_nav_host.write_file(path, buffer.data, buffer.length))
	{
		free(buffer.data);
		EvoBot_NavPrintf("EvoBot navigation: could not write %s\n", path);
		return;
	}
	EvoBot_NavPrintf("EvoBot navigation OBJ exported: %s (%zu bytes)\n",
		path, buffer.length);
	free(buffer.data);
}
