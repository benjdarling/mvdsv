#ifndef EVOBOT_NAV_H
#define EVOBOT_NAV_H

#include "evobot_host.h"

void EvoBot_NavInit(const evobot_host_api_t *host);
void EvoBot_NavMapLoaded(const char *map_name, uint32_t map_checksum);
void EvoBot_NavMapCleared(void);
void EvoBot_NavShutdown(void);

void EvoBot_NavGenerate(void);
void EvoBot_NavPrintStatus(void);
void EvoBot_NavSave(void);
void EvoBot_NavLoad(const char *source_map);
void EvoBot_NavClear(void);
void EvoBot_NavExportObj(void);

#endif
