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
//      inDevOS system interface (timing, memory, error handling)
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"

// inDevOS kernel functions (declarations only - actual implementations in kernel)
// These will be resolved at load time by inDevOS
extern uint64_t get_ticks(void);      // From timer.h
extern void hpet_sleep_counter(uint64_t ms);  // From hpet.h

int mb_used = 6;  // 6MB heap for DOOM

void I_Tactile(int on, int off, int total)
{
    // UNUSED - force feedback not supported
    on = off = total = 0;
}

ticcmd_t emptycmd;
ticcmd_t* I_BaseTiccmd(void)
{
    return &emptycmd;
}

int I_GetHeapSize(void)
{
    return mb_used * 1024 * 1024;
}

byte* I_ZoneBase(int* size)
{
    *size = mb_used * 1024 * 1024;
    return (byte*)malloc(*size);
}

//
// I_GetTime
// returns time in 1/70th second tics (DOOM's 35Hz tick rate = 2 tics per frame)
//
int I_GetTime(void)
{
    static uint64_t basetime = 0;
    uint64_t ticks = get_ticks();  // Get system ticks (usually in ms)
    
    if (!basetime)
        basetime = ticks;
    
    // Convert to DOOM tics (70 tics per second = ~14.3ms per tic)
    // If get_ticks() returns milliseconds, divide by 14.2857 (~1000/70)
    int newtics = (int)((ticks - basetime) * TICRATE / 1000);
    return newtics;
}

//
// I_Init
//
void I_Init(void)
{
    I_InitSound();
}

//
// I_Quit
//
void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    
    // In a real OS we'd exit, but in inDevOS we might just return to kernel
    printf("DOOM exiting...\n");
    /* Spinning here left the program owning the console/framebuffer with
     * interrupts on; return to the shell like the Ctrl+C path does. */
    exit(0);
}

void I_WaitVBL(int count)
{
    // Wait for vertical blank - approximately 1/70th second per count
    hpet_sleep_counter(count * 14);  // ~14ms per tic
}

void I_BeginRead(void)
{
    // Disk read start - not needed for inDevOS
}

void I_EndRead(void)
{
    // Disk read end - not needed for inDevOS
}

byte* I_AllocLow(int length)
{
    byte* mem;
    
    mem = (byte*)malloc(length);
    if (mem)
        memset(mem, 0, length);
    return mem;
}

//
// I_Error
//
extern boolean demorecording;

void I_Error(char *error, ...)
{
    // Simple implementation - just print the error string
    // (our vsnprintf doesn't support formatting)
    printf("DOOM Error: ");
    printf(error);
    printf("\n");
    
    // Shutdown cleanly if possible
    if (demorecording)
        G_CheckDemoStatus();
    
    D_QuitNetGame();
    I_ShutdownGraphics();
    
    // Halt
    while(1);
}
