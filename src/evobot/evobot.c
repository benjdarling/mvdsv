#include "evobot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define EVOBOT_VERSION "0.1.0"

static evobot_host_api_t evobot_host;
static int evobot_initialized;
static int evobot_map_loaded;
static char evobot_map_name[EVOBOT_MAP_NAME_MAX];
static uint32_t evobot_map_checksum;

static void EvoBot_Print(const char *message)
{
	if (evobot_host.print)
		evobot_host.print(message);
}

void EvoBot_Init(const evobot_host_api_t *host)
{
	if (evobot_initialized)
		return;

	memset(&evobot_host, 0, sizeof(evobot_host));
	if (host)
		evobot_host = *host;

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
}

void EvoBot_Frame(double server_time)
{
	(void)server_time;
}

void EvoBot_MapCleared(void)
{
	evobot_map_loaded = 0;
	evobot_map_name[0] = '\0';
	evobot_map_checksum = 0;
}

void EvoBot_Shutdown(void)
{
	if (!evobot_initialized)
		return;

	EvoBot_MapCleared();
	memset(&evobot_host, 0, sizeof(evobot_host));
	evobot_initialized = 0;
}

void EvoBot_PrintStatus(void)
{
	char message[128];

	EvoBot_Print("EvoBot status\n");
	snprintf(message, sizeof(message), "initialized: %s\n", evobot_initialized ? "yes" : "no");
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "map loaded: %s\n", evobot_map_loaded ? "yes" : "no");
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "map: %s\n", evobot_map_loaded ? evobot_map_name : "<none>");
	EvoBot_Print(message);
	snprintf(message, sizeof(message), "map checksum: %" PRIu32 "\n", evobot_map_checksum);
	EvoBot_Print(message);
	EvoBot_Print("bots: 0\n");
}

void EvoBot_PrintVersion(void)
{
	EvoBot_Print("EvoBot " EVOBOT_VERSION "\n");
}
