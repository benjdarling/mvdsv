#include "evobot_nav_convex.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVOBOT_NAV_FORMAT_VERSION 2
#define EVOBOT_NAV_MAX_FILE_SIZE (128u * 1024u * 1024u)
#define EVOBOT_NAV_MAX_PLANES 256
#define EVOBOT_NAV_PLANE_EPSILON 0.05f
#define EVOBOT_NAV_POINT_EPSILON 0.1f
#define EVOBOT_NAV_WALKABLE_NORMAL_Z 0.7f
#define EVOBOT_NAV_SUPPORT_PROBE 256.0f
#define EVOBOT_NAV_STEP_SIZE 18.0f
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
	double generation_time;
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
	if (!EvoBot_NavCopyArea(source, front) || !EvoBot_NavAddPlane(front, &reverse) ||
		!EvoBot_NavBuildFaces(front))
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
			return 0;
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
	size_t i;

	EvoBot_NavAreaCenter(area, &center);
	area->supported = 0;
	area->floor_height = 0;
	area->support_distance = 0;
	memset(&area->support_normal, 0, sizeof(area->support_normal));
	if (area->contents == EVOBOT_CONTENTS_WATER ||
		area->contents == EVOBOT_CONTENTS_SLIME ||
		area->contents == EVOBOT_CONTENTS_LAVA)
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
		return;
	}
	area->contents = EVOBOT_CONTENTS_AIR;
	area->water_level = 0;
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
				goto failed;
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
			if (minimum < -EVOBOT_NAV_PLANE_EPSILON &&
				maximum > EVOBOT_NAV_PLANE_EPSILON)
			{
				evobot_nav_area_t back;
				evobot_nav_area_t front;

				memset(&back, 0, sizeof(back));
				memset(&front, 0, sizeof(front));
				if (!EvoBot_NavSplitArea(fragment, &splits[i], &back, &front))
					goto failed;
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
	snprintf(evobot_nav_active_map, sizeof(evobot_nav_active_map), "%s",
		map_name ? map_name : "");
	evobot_nav_active_checksum = map_checksum;
	evobot_nav_map_loaded = map_name && map_name[0];
}

void EvoBot_NavConvexMapCleared(void)
{
	EvoBot_NavFreeDataset(&evobot_nav);
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
		!evobot_nav_host.trace_player_world || !evobot_nav_host.point_contents)
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
		return;
	}
	started = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() : 0;
	EvoBot_NavPrint("EvoBot navigation: building convex player-space areas\n");
	if (!EvoBot_NavCaptureInteractors() ||
		!EvoBot_NavBuildPlayerLeaves(player_tree.root_node, path, 0) ||
		!EvoBot_NavApplyContentsTree() ||
		!EvoBot_NavApplyGravitySubdivision())
		goto failed;
	for (i = 0; i < evobot_nav.area_count; i++)
		evobot_nav.areas[i].dynamic_interactor =
			EvoBot_NavDynamicForArea(&evobot_nav.areas[i]);
	EvoBot_NavMergeAreas();
	if (!EvoBot_NavGeneratePortals())
		goto failed;
	evobot_nav.stats.generation_time = evobot_nav_host.monotonic_time ?
		evobot_nav_host.monotonic_time() - started : 0;
	evobot_nav.state = EVOBOT_NAV_STATE_GENERATED;
	EvoBot_NavPrintf("EvoBot navigation generated\nconvex areas: %zu\nportals: %zu\n"
		"initial collision areas: %" PRIu64 "\ngravity splits: %" PRIu64 "\n"
		"convex merges: %" PRIu64 "\ngeneration time: %.3f seconds\n",
		evobot_nav.area_count, evobot_nav.portal_count,
		evobot_nav.stats.initial_areas, evobot_nav.stats.gravity_splits,
		evobot_nav.stats.merges, evobot_nav.stats.generation_time);
	return;

failed:
	EvoBot_NavPrint("EvoBot navigation: convex generation failed\n");
	EvoBot_NavFreeDataset(&evobot_nav);
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
			", \"seconds\": %.9g},\n  \"areas\": [\n",
			evobot_nav.stats.initial_areas, evobot_nav.stats.content_splits,
			evobot_nav.stats.gravity_splits, evobot_nav.stats.merges,
			evobot_nav.stats.validation_traces, evobot_nav.stats.generation_time))
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
			!EvoBot_NavBufferAppend(buffer, ", \"classname\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->classname) ||
			!EvoBot_NavBufferAppend(buffer, ", \"model\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->model) ||
			!EvoBot_NavBufferAppend(buffer, ", \"target\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->target) ||
			!EvoBot_NavBufferAppend(buffer, ", \"targetname\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->targetname) ||
			!EvoBot_NavBufferAppend(buffer, ", \"destination_map\": ") ||
			!EvoBot_NavBufferJsonString(buffer, host->destination_map) ||
			!EvoBot_NavBufferAppendFormat(buffer, "}%s\n",
				i + 1 == evobot_nav.interactor_count ? "" : ",")) return 0;
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
		else if (!strcmp(key, "classname")) { if (!EvoBot_NavParserString(parser, interactor->host.classname, sizeof(interactor->host.classname))) return 0; }
		else if (!strcmp(key, "model")) { if (!EvoBot_NavParserString(parser, interactor->host.model, sizeof(interactor->host.model))) return 0; }
		else if (!strcmp(key, "target")) { if (!EvoBot_NavParserString(parser, interactor->host.target, sizeof(interactor->host.target))) return 0; }
		else if (!strcmp(key, "targetname")) { if (!EvoBot_NavParserString(parser, interactor->host.targetname, sizeof(interactor->host.targetname))) return 0; }
		else if (!strcmp(key, "destination_map")) { if (!EvoBot_NavParserString(parser, interactor->host.destination_map, sizeof(interactor->host.destination_map))) return 0; }
		else if (!EvoBot_NavParserSkipValue(parser)) return 0;
		EvoBot_NavParserWhitespace(parser);
		if (parser->cursor < parser->end && *parser->cursor == '}') { parser->cursor++; return 1; }
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
		else if (!strcmp(key, "seconds")) stats->generation_time = number;
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
		EvoBot_NavPrint("EvoBot navigation: could not serialize version 2 data\n");
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
	EvoBot_NavPrintf("EvoBot navigation loaded: %s (%zu areas, %zu portals)\n",
		path, evobot_nav.area_count, evobot_nav.portal_count);
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
