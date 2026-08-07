#ifndef EVOBOT_NAV_CONVEX_H
#define EVOBOT_NAV_CONVEX_H

#include "evobot_host.h"

void EvoBot_NavConvexInit(const evobot_host_api_t *host);
void EvoBot_NavConvexMapLoaded(const char *map_name, uint32_t map_checksum);
void EvoBot_NavConvexMapCleared(void);
void EvoBot_NavConvexShutdown(void);
void EvoBot_NavConvexGenerate(void);
void EvoBot_NavConvexPrintStatus(void);
void EvoBot_NavConvexSave(void);
void EvoBot_NavConvexLoad(const char *source_map);
void EvoBot_NavConvexClear(void);
void EvoBot_NavConvexExportObj(void);

#endif
