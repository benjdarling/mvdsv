#ifndef EVOBOT_HOST_H
#define EVOBOT_HOST_H

#include <stddef.h>
#include <stdint.h>

#define EVOBOT_NAV_CLASSNAME_MAX 64
#define EVOBOT_NAV_MODEL_MAX 64
#define EVOBOT_NAV_TARGET_MAX 64
#define EVOBOT_NAV_MAP_MAX 64

typedef struct evobot_vec3_s
{
	float v[3];
} evobot_vec3_t;

typedef struct evobot_bounds_s
{
	evobot_vec3_t mins;
	evobot_vec3_t maxs;
} evobot_bounds_t;

typedef enum evobot_contents_e
{
	EVOBOT_CONTENTS_SOLID,
	EVOBOT_CONTENTS_AIR,
	EVOBOT_CONTENTS_WATER,
	EVOBOT_CONTENTS_SLIME,
	EVOBOT_CONTENTS_LAVA,
	EVOBOT_CONTENTS_OTHER
} evobot_contents_t;

typedef struct evobot_trace_s
{
	int all_solid;
	int start_solid;
	float fraction;
	evobot_vec3_t end;
	evobot_vec3_t normal;
} evobot_trace_t;

typedef enum evobot_collision_tree_kind_e
{
	EVOBOT_COLLISION_TREE_POINT,
	EVOBOT_COLLISION_TREE_PLAYER
} evobot_collision_tree_kind_t;

typedef enum evobot_collision_leaf_e
{
	EVOBOT_COLLISION_LEAF_SOLID = -1,
	EVOBOT_COLLISION_LEAF_AIR = -2,
	EVOBOT_COLLISION_LEAF_WATER = -3,
	EVOBOT_COLLISION_LEAF_SLIME = -4,
	EVOBOT_COLLISION_LEAF_LAVA = -5,
	EVOBOT_COLLISION_LEAF_OTHER = -6
} evobot_collision_leaf_t;

typedef struct evobot_collision_node_s
{
	evobot_vec3_t normal;
	float distance;
	int children[2];
} evobot_collision_node_t;

typedef struct evobot_collision_tree_s
{
	int root_node;
	int first_node;
	int last_node;
} evobot_collision_tree_t;

typedef enum evobot_interactor_kind_e
{
	EVOBOT_INTERACTOR_DOOR,
	EVOBOT_INTERACTOR_BUTTON,
	EVOBOT_INTERACTOR_PLATFORM,
	EVOBOT_INTERACTOR_TRAIN,
	EVOBOT_INTERACTOR_TELEPORTER,
	EVOBOT_INTERACTOR_TELEPORT_DESTINATION,
	EVOBOT_INTERACTOR_LEVEL_EXIT,
	EVOBOT_INTERACTOR_LOGIC,
	EVOBOT_INTERACTOR_OTHER
} evobot_interactor_kind_t;

typedef struct evobot_host_interactor_s
{
	evobot_interactor_kind_t kind;
	int dynamic_brush;
	evobot_bounds_t bounds;
	evobot_bounds_t swept_bounds;
	evobot_vec3_t origin;
	char classname[EVOBOT_NAV_CLASSNAME_MAX];
	char model[EVOBOT_NAV_MODEL_MAX];
	char target[EVOBOT_NAV_TARGET_MAX];
	char targetname[EVOBOT_NAV_TARGET_MAX];
	char destination_map[EVOBOT_NAV_MAP_MAX];
} evobot_host_interactor_t;

typedef uint32_t evobot_client_handle_t;

#define EVOBOT_CLIENT_HANDLE_INVALID ((evobot_client_handle_t)0)

typedef enum evobot_create_bot_status_e
{
	EVOBOT_CREATE_BOT_OK,
	EVOBOT_CREATE_BOT_UNAVAILABLE,
	EVOBOT_CREATE_BOT_NO_FREE_SLOT,
	EVOBOT_CREATE_BOT_UNSUPPORTED_GAMECODE
} evobot_create_bot_status_t;

typedef struct evobot_create_bot_result_s
{
	evobot_create_bot_status_t status;
	evobot_client_handle_t handle;
} evobot_create_bot_result_t;

typedef void (*evobot_host_print_t)(const char *message);
typedef double (*evobot_host_server_time_t)(void);
typedef evobot_create_bot_result_t (*evobot_host_create_bot_client_t)(const char *name);
typedef int (*evobot_host_remove_bot_client_t)(evobot_client_handle_t handle);
typedef int (*evobot_host_is_bot_client_valid_t)(evobot_client_handle_t handle);
typedef double (*evobot_host_monotonic_time_t)(void);
typedef int (*evobot_host_world_bounds_t)(evobot_bounds_t *bounds);
typedef int (*evobot_host_player_bounds_t)(evobot_bounds_t *bounds);
typedef int (*evobot_host_trace_player_world_t)(const evobot_vec3_t *start,
	const evobot_vec3_t *end, evobot_trace_t *trace);
typedef evobot_contents_t (*evobot_host_point_contents_t)(const evobot_vec3_t *point);
typedef int (*evobot_host_collision_tree_t)(evobot_collision_tree_kind_t kind,
	evobot_collision_tree_t *tree);
typedef int (*evobot_host_collision_node_t)(evobot_collision_tree_kind_t kind,
	int node_index, evobot_collision_node_t *node);
typedef int (*evobot_host_interactor_count_t)(void);
typedef int (*evobot_host_get_interactor_t)(int index, evobot_host_interactor_t *interactor);
typedef int (*evobot_host_file_size_t)(const char *path, size_t *size);
typedef int (*evobot_host_read_file_t)(const char *path, void *data, size_t size);
typedef int (*evobot_host_write_file_t)(const char *path, const void *data, size_t size);

typedef struct evobot_host_api_s
{
	evobot_host_print_t print;
	evobot_host_server_time_t server_time;
	evobot_host_create_bot_client_t create_bot_client;
	evobot_host_remove_bot_client_t remove_bot_client;
	evobot_host_is_bot_client_valid_t is_bot_client_valid;
	evobot_host_monotonic_time_t monotonic_time;
	evobot_host_world_bounds_t world_bounds;
	evobot_host_player_bounds_t player_bounds;
	evobot_host_trace_player_world_t trace_player_world;
	evobot_host_point_contents_t point_contents;
	evobot_host_collision_tree_t collision_tree;
	evobot_host_collision_node_t collision_node;
	evobot_host_interactor_count_t interactor_count;
	evobot_host_get_interactor_t get_interactor;
	evobot_host_file_size_t file_size;
	evobot_host_read_file_t read_file;
	evobot_host_write_file_t write_file;
} evobot_host_api_t;

#endif
