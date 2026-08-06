#ifndef EVOBOT_H
#define EVOBOT_H

#include <stdint.h>

#include "evobot_host.h"

#define EVOBOT_MAP_NAME_MAX 64

typedef struct evobot_map_info_s
{
	const char *name;
	uint32_t checksum;
} evobot_map_info_t;

void EvoBot_Init(const evobot_host_api_t *host);
void EvoBot_MapLoaded(const evobot_map_info_t *map);
void EvoBot_Frame(double server_time);
void EvoBot_MapCleared(void);
void EvoBot_Shutdown(void);

void EvoBot_PrintStatus(void);
void EvoBot_PrintVersion(void);

#endif
