// Full agent/LLM control of the human player (-aiplayer).  See files/g_agent.c.
#ifndef __G_AGENT__
#define __G_AGENT__

#include "d_ticcmd.h"

extern int agent_active;		// set by G_AgentInit when -aiplayer is given

void G_AgentInit (void);		// parse -aiplayer [port|demo], open the socket
int  G_AgentActive (void);		// 1 -> the agent drives the player's ticcmd
void G_AgentBuildTiccmd (ticcmd_t* cmd);// build the player's ticcmd from the intent
void G_Agent_Archive (void);
void G_Agent_UnArchive (void);

#endif
