// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
// inDevOS port by inDevOS team
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// DESCRIPTION:
//      Network stub for single-player DOOM
//
//-----------------------------------------------------------------------------

#include <stddef.h>

#include "doomdef.h"
#include "i_system.h"

void *malloc(size_t size);
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "doomstat.h"

// Single player - no network
// Note: These are defined in d_net.c, we just initialize them here
extern doomcom_t *doomcom;
extern doomdata_t *netbuffer;

void I_InitNetwork(void)
{
    // Allocate doomcom structure
    doomcom = (doomcom_t*)malloc(sizeof(doomcom_t));
    if (!doomcom)
	I_Error("I_InitNetwork: malloc(doomcom) failed");
    memset(doomcom, 0, sizeof(*doomcom));

    // Single player setup (must match what D_CheckNetGame expects)
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->deathmatch = 0;
    doomcom->consoleplayer = 0;
    doomcom->ticdup = 1;     /* 0 → div-by-zero in maxsend = BACKUPTICS/(2*ticdup)-1 */
    doomcom->extratics = 1;
    doomcom->savegame = -1;  /* new game (not loading a save) */
    
    printf("DOOM: Network initialized (single-player mode)\n");
}

void I_NetCmd(void)
{
    // No network commands in single player
}
