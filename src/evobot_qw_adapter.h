#ifndef EVOBOT_QW_ADAPTER_H
#define EVOBOT_QW_ADAPTER_H

#include <evobot/evobot.h>

struct usercmd_s;

void EvoBot_QW_Init(void);
void EvoBot_QW_MapLoaded(void);
void EvoBot_QW_Frame(void);
void EvoBot_QW_PrepareBotCommands(double frame_time);
void EvoBot_QW_RecordHumanCommand(int client_slot,
	const struct usercmd_s *command);
void EvoBot_QW_MapCleared(void);
void EvoBot_QW_Shutdown(void);

#endif
