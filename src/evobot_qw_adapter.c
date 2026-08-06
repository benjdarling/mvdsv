#include "qwsvdef.h"

#include "evobot_qw_adapter.h"

static evobot_host_api_t evobot_qw_host;
static int evobot_qw_initialized;
static int evobot_qw_map_loaded;

static void EvoBot_QW_Print(const char *message)
{
	Con_Printf("%s", message);
}

static double EvoBot_QW_ServerTime(void)
{
	return sv.time;
}

static void EvoBot_QW_Status_f(void)
{
	EvoBot_PrintStatus();
}

static void EvoBot_QW_Version_f(void)
{
	EvoBot_PrintVersion();
}

void EvoBot_QW_Init(void)
{
	if (evobot_qw_initialized)
		return;

	evobot_qw_host.print = EvoBot_QW_Print;
	evobot_qw_host.server_time = EvoBot_QW_ServerTime;
	EvoBot_Init(&evobot_qw_host);

	Cmd_AddCommand("evobot_status", EvoBot_QW_Status_f);
	Cmd_AddCommand("evobot_version", EvoBot_QW_Version_f);
	evobot_qw_initialized = 1;
}

void EvoBot_QW_MapLoaded(void)
{
	evobot_map_info_t map;

	if (!evobot_qw_initialized || evobot_qw_map_loaded)
		return;

	map.name = sv.mapname;
	map.checksum = sv.map_checksum;
	EvoBot_MapLoaded(&map);
	evobot_qw_map_loaded = 1;
}

void EvoBot_QW_Frame(void)
{
	if (!evobot_qw_initialized)
		return;

	EvoBot_Frame(sv.time);
}

void EvoBot_QW_MapCleared(void)
{
	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return;

	EvoBot_MapCleared();
	evobot_qw_map_loaded = 0;
}

void EvoBot_QW_Shutdown(void)
{
	if (!evobot_qw_initialized)
		return;

	EvoBot_QW_MapCleared();
	EvoBot_Shutdown();
	evobot_qw_initialized = 0;
}
