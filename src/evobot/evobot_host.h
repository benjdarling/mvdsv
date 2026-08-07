#ifndef EVOBOT_HOST_H
#define EVOBOT_HOST_H

#include <stdint.h>

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

typedef struct evobot_host_api_s
{
	evobot_host_print_t print;
	evobot_host_server_time_t server_time;
	evobot_host_create_bot_client_t create_bot_client;
	evobot_host_remove_bot_client_t remove_bot_client;
	evobot_host_is_bot_client_valid_t is_bot_client_valid;
} evobot_host_api_t;

#endif
