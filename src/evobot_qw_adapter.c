#include "qwsvdef.h"

#include "evobot_qw_adapter.h"

static evobot_host_api_t evobot_qw_host;
static int evobot_qw_initialized;
static int evobot_qw_map_loaded;
static evobot_client_handle_t evobot_qw_client_handles[MAX_CLIENTS];
static evobot_client_handle_t evobot_qw_next_client_handle = 1;
static double evobot_qw_command_msec_remainder;

static void EvoBot_QW_Print(const char *message)
{
	Con_Printf("%s", message);
}

static double EvoBot_QW_ServerTime(void)
{
	return sv.time;
}

static int EvoBot_QW_FindClientSlot(evobot_client_handle_t handle)
{
	int i;

	if (handle == EVOBOT_CLIENT_HANDLE_INVALID)
		return -1;

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		if (evobot_qw_client_handles[i] == handle)
			return i;
	}

	return -1;
}

static evobot_client_handle_t EvoBot_QW_NewClientHandle(void)
{
	evobot_client_handle_t handle;

	do
	{
		handle = evobot_qw_next_client_handle++;
		if (evobot_qw_next_client_handle == EVOBOT_CLIENT_HANDLE_INVALID)
			evobot_qw_next_client_handle++;
	}
	while (handle == EVOBOT_CLIENT_HANDLE_INVALID || EvoBot_QW_FindClientSlot(handle) >= 0);

	return handle;
}

static int EvoBot_QW_IsBotClientValid(evobot_client_handle_t handle)
{
#ifdef USE_PR2
	client_t *client;
	int slot = EvoBot_QW_FindClientSlot(handle);

	if (slot < 0)
		return 0;

	client = &svs.clients[slot];
	if (client->state == cs_free || !client->isBot || client->edict != EDICT_NUM(slot + 1))
	{
		evobot_qw_client_handles[slot] = EVOBOT_CLIENT_HANDLE_INVALID;
		return 0;
	}

	return 1;
#else
	(void)handle;
	return 0;
#endif
}

static evobot_create_bot_result_t EvoBot_QW_CreateBotClient(const char *name)
{
	evobot_create_bot_result_t result;
	int edictnum;
	int slot;

	result.status = EVOBOT_CREATE_BOT_UNAVAILABLE;
	result.handle = EVOBOT_CLIENT_HANDLE_INVALID;
	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return result;

#ifdef USE_PR2
	if (!sv_vm)
	{
		result.status = EVOBOT_CREATE_BOT_UNSUPPORTED_GAMECODE;
		return result;
	}

	edictnum = SV_AddBotClient(name, 0, 0, "base", false);
	if (!edictnum)
	{
		result.status = EVOBOT_CREATE_BOT_NO_FREE_SLOT;
		return result;
	}

	slot = edictnum - 1;
	if (slot < 0 || slot >= MAX_CLIENTS || evobot_qw_client_handles[slot] != EVOBOT_CLIENT_HANDLE_INVALID)
	{
		if (slot >= 0 && slot < MAX_CLIENTS)
		{
			int old_self = pr_global_struct->self;

			RemoveBot(&svs.clients[slot]);
			pr_global_struct->self = old_self;
		}
		return result;
	}

	result.handle = EvoBot_QW_NewClientHandle();
	evobot_qw_client_handles[slot] = result.handle;
	result.status = EVOBOT_CREATE_BOT_OK;
#else
	(void)name;
	(void)edictnum;
	(void)slot;
	result.status = EVOBOT_CREATE_BOT_UNSUPPORTED_GAMECODE;
#endif

	return result;
}

static int EvoBot_QW_RemoveBotClient(evobot_client_handle_t handle)
{
#ifdef USE_PR2
	int old_self;
	int slot = EvoBot_QW_FindClientSlot(handle);

	if (slot < 0 || !EvoBot_QW_IsBotClientValid(handle))
		return 0;

	old_self = pr_global_struct->self;
	RemoveBot(&svs.clients[slot]);
	pr_global_struct->self = old_self;
	evobot_qw_client_handles[slot] = EVOBOT_CLIENT_HANDLE_INVALID;
	return 1;
#else
	(void)handle;
	return 0;
#endif
}

static void EvoBot_QW_Status_f(void)
{
	EvoBot_PrintStatus();
}

static void EvoBot_QW_Version_f(void)
{
	EvoBot_PrintVersion();
}

static void EvoBot_QW_Add_f(void)
{
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_add <name>\n");
		return;
	}

	EvoBot_AddBot(Cmd_Argv(1));
}

static void EvoBot_QW_Remove_f(void)
{
	if (Cmd_Argc() != 2 || !Cmd_Argv(1)[0])
	{
		Con_Printf("usage: evobot_remove <name>\n");
		return;
	}

	EvoBot_RemoveBot(Cmd_Argv(1));
}

void EvoBot_QW_Init(void)
{
	if (evobot_qw_initialized)
		return;

	evobot_qw_host.print = EvoBot_QW_Print;
	evobot_qw_host.server_time = EvoBot_QW_ServerTime;
	evobot_qw_host.create_bot_client = EvoBot_QW_CreateBotClient;
	evobot_qw_host.remove_bot_client = EvoBot_QW_RemoveBotClient;
	evobot_qw_host.is_bot_client_valid = EvoBot_QW_IsBotClientValid;
	EvoBot_Init(&evobot_qw_host);

	Cmd_AddCommand("evobot_status", EvoBot_QW_Status_f);
	Cmd_AddCommand("evobot_version", EvoBot_QW_Version_f);
	Cmd_AddCommand("evobot_add", EvoBot_QW_Add_f);
	Cmd_AddCommand("evobot_remove", EvoBot_QW_Remove_f);
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

void EvoBot_QW_PrepareBotCommands(double frame_time)
{
#ifdef USE_PR2
	double command_msec;
	int msec;
	int i;

	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return;

	command_msec = frame_time * 1000.0 + evobot_qw_command_msec_remainder;
	msec = (int)command_msec;
	evobot_qw_command_msec_remainder = command_msec - msec;
	msec = bound(1, msec, 255);

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client_t *client;

		if (evobot_qw_client_handles[i] == EVOBOT_CLIENT_HANDLE_INVALID ||
			!EvoBot_QW_IsBotClientValid(evobot_qw_client_handles[i]))
			continue;

		client = &svs.clients[i];
		memset(&client->botcmd, 0, sizeof(client->botcmd));
		client->botcmd.msec = (byte)msec;
		VectorCopy(client->edict->v->v_angle, client->botcmd.angles);

		if (client->edict->v->fixangle)
		{
			VectorCopy(client->edict->v->angles, client->botcmd.angles);
			client->botcmd.angles[PITCH] *= -3;
			client->edict->v->fixangle = 0;
		}
	}
#else
	(void)frame_time;
#endif
}

void EvoBot_QW_MapCleared(void)
{
	if (!evobot_qw_initialized || !evobot_qw_map_loaded)
		return;

	EvoBot_MapCleared();
	memset(evobot_qw_client_handles, 0, sizeof(evobot_qw_client_handles));
	evobot_qw_command_msec_remainder = 0;
	evobot_qw_map_loaded = 0;
}

void EvoBot_QW_Shutdown(void)
{
	if (!evobot_qw_initialized)
		return;

	EvoBot_QW_MapCleared();
	EvoBot_Shutdown();
	memset(&evobot_qw_host, 0, sizeof(evobot_qw_host));
	memset(evobot_qw_client_handles, 0, sizeof(evobot_qw_client_handles));
	evobot_qw_command_msec_remainder = 0;
	evobot_qw_initialized = 0;
}
