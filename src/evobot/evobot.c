#include "evobot.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define EVOBOT_VERSION "0.1.0"
#define EVOBOT_MAX_BOTS 64

typedef struct evobot_bot_s
{
	evobot_client_handle_t handle;
	char name[EVOBOT_CLIENT_NAME_MAX];
} evobot_bot_t;

static evobot_host_api_t evobot_host;
static int evobot_initialized;
static int evobot_map_loaded;
static char evobot_map_name[EVOBOT_MAP_NAME_MAX];
static uint32_t evobot_map_checksum;
static evobot_bot_t evobot_bots[EVOBOT_MAX_BOTS];

static void EvoBot_Print(const char *message)
{
	if (evobot_host.print)
		evobot_host.print(message);
}

static int EvoBot_NameEquals(const char *left, const char *right)
{
	while (*left && *right)
	{
		if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
			return 0;
		left++;
		right++;
	}

	return *left == *right;
}

static int EvoBot_FindBot(const char *name)
{
	int i;

	for (i = 0; i < EVOBOT_MAX_BOTS; i++)
	{
		if (evobot_bots[i].handle != EVOBOT_CLIENT_HANDLE_INVALID &&
			EvoBot_NameEquals(evobot_bots[i].name, name))
			return i;
	}

	return -1;
}

static void EvoBot_PruneInvalidBots(void)
{
	int i;

	if (!evobot_host.is_bot_client_valid)
		return;

	for (i = 0; i < EVOBOT_MAX_BOTS; i++)
	{
		if (evobot_bots[i].handle != EVOBOT_CLIENT_HANDLE_INVALID &&
			!evobot_host.is_bot_client_valid(evobot_bots[i].handle))
			memset(&evobot_bots[i], 0, sizeof(evobot_bots[i]));
	}
}

void EvoBot_Init(const evobot_host_api_t *host)
{
	if (evobot_initialized)
		return;

	memset(&evobot_host, 0, sizeof(evobot_host));
	if (host)
		evobot_host = *host;

	EvoBot_NavConvexInit(&evobot_host);
	evobot_initialized = 1;
}

void EvoBot_MapLoaded(const evobot_map_info_t *map)
{
	if (!evobot_initialized || !map)
		return;

	if (map->name)
		snprintf(evobot_map_name, sizeof(evobot_map_name), "%s", map->name);
	else
		evobot_map_name[0] = '\0';

	evobot_map_checksum = map->checksum;
	evobot_map_loaded = 1;
	EvoBot_NavConvexMapLoaded(evobot_map_name, evobot_map_checksum);
}

void EvoBot_Frame(double server_time)
{
	(void)server_time;
	EvoBot_PruneInvalidBots();
}

void EvoBot_MapCleared(void)
{
	int i;

	EvoBot_NavConvexMapCleared();

	for (i = 0; i < EVOBOT_MAX_BOTS; i++)
	{
		if (evobot_bots[i].handle != EVOBOT_CLIENT_HANDLE_INVALID && evobot_host.remove_bot_client)
			evobot_host.remove_bot_client(evobot_bots[i].handle);
		memset(&evobot_bots[i], 0, sizeof(evobot_bots[i]));
	}

	evobot_map_loaded = 0;
	evobot_map_name[0] = '\0';
	evobot_map_checksum = 0;
}

void EvoBot_Shutdown(void)
{
	if (!evobot_initialized)
		return;

	EvoBot_MapCleared();
	EvoBot_NavConvexShutdown();
	memset(&evobot_host, 0, sizeof(evobot_host));
	evobot_initialized = 0;
}

void EvoBot_AddBot(const char *name)
{
	evobot_create_bot_result_t result;
	char message[128];
	size_t length;
	int i;

	if (!evobot_initialized || !evobot_map_loaded)
	{
		EvoBot_Print("EvoBot: no map is loaded\n");
		return;
	}
	if (!name || !name[0])
	{
		EvoBot_Print("EvoBot: bot name is required\n");
		return;
	}

	length = strlen(name);
	if (length >= EVOBOT_CLIENT_NAME_MAX || strchr(name, '\\'))
	{
		EvoBot_Print("EvoBot: invalid bot name\n");
		return;
	}

	EvoBot_PruneInvalidBots();
	if (EvoBot_FindBot(name) >= 0)
	{
		snprintf(message, sizeof(message), "EvoBot: bot '%s' already exists\n", name);
		EvoBot_Print(message);
		return;
	}

	for (i = 0; i < EVOBOT_MAX_BOTS; i++)
	{
		if (evobot_bots[i].handle == EVOBOT_CLIENT_HANDLE_INVALID)
			break;
	}
	if (i == EVOBOT_MAX_BOTS || !evobot_host.create_bot_client)
	{
		EvoBot_Print("EvoBot: bot clients are unavailable\n");
		return;
	}

	result = evobot_host.create_bot_client(name);
	if (result.status != EVOBOT_CREATE_BOT_OK || result.handle == EVOBOT_CLIENT_HANDLE_INVALID)
	{
		switch (result.status)
		{
		case EVOBOT_CREATE_BOT_OK:
		case EVOBOT_CREATE_BOT_UNAVAILABLE:
			EvoBot_Print("EvoBot: bot clients are unavailable\n");
			break;
		case EVOBOT_CREATE_BOT_NO_FREE_SLOT:
			EvoBot_Print("EvoBot: no free client slots\n");
			break;
		case EVOBOT_CREATE_BOT_UNSUPPORTED_GAMECODE:
			EvoBot_Print("EvoBot: fake clients require PR2 game code\n");
			break;
		}
		return;
	}

	evobot_bots[i].handle = result.handle;
	snprintf(evobot_bots[i].name, sizeof(evobot_bots[i].name), "%s", name);
	snprintf(message, sizeof(message), "EvoBot added: %s\n", name);
	EvoBot_Print(message);
}

void EvoBot_RemoveBot(const char *name)
{
	char message[128];
	int index;

	if (!name || !name[0])
	{
		EvoBot_Print("EvoBot: bot name is required\n");
		return;
	}

	EvoBot_PruneInvalidBots();
	index = EvoBot_FindBot(name);
	if (index < 0)
	{
		snprintf(message, sizeof(message), "EvoBot: bot '%s' not found\n", name);
		EvoBot_Print(message);
		return;
	}

	if (!evobot_host.remove_bot_client || !evobot_host.remove_bot_client(evobot_bots[index].handle))
	{
		memset(&evobot_bots[index], 0, sizeof(evobot_bots[index]));
		EvoBot_Print("EvoBot: bot client is no longer available\n");
		return;
	}

	snprintf(message, sizeof(message), "EvoBot removed: %s\n", evobot_bots[index].name);
	memset(&evobot_bots[index], 0, sizeof(evobot_bots[index]));
	EvoBot_Print(message);
}

void EvoBot_PrintStatus(void)
{
	char message[128];
	int bots = 0;
	int i;

	EvoBot_PruneInvalidBots();
	for (i = 0; i < EVOBOT_MAX_BOTS; i++)
	{
		if (evobot_bots[i].handle != EVOBOT_CLIENT_HANDLE_INVALID)
			bots++;
	}

	EvoBot_Print("EvoBot status\n");
	snprintf(message, sizeof(message), "initialized: %s\n", evobot_initialized ? "yes" : "no");
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "map loaded: %s\n", evobot_map_loaded ? "yes" : "no");
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "map: %s\n", evobot_map_loaded ? evobot_map_name : "<none>");
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "map checksum: %" PRIu32 "\n", evobot_map_checksum);
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "bots: %d\n", bots);
	EvoBot_Print(message);
}

void EvoBot_PrintVersion(void)
{
	EvoBot_Print("EvoBot " EVOBOT_VERSION "\n");
}
