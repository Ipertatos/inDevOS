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
//      inDevOS sound interface using PC speaker
//      Note: This is a minimal implementation. Full sound would require
//      a proper sound card driver.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdint.h>
#include "doomdef.h"
#include "i_sound.h"
#include "sounds.h"
#include "m_argv.h"
#include "w_wad.h"

// inDevOS PC speaker interface
extern void play_sound(uint32_t freq);
extern void nosound(void);
extern void beep(void);

// Sound disabled by default (PC speaker is very limited)
static int sound_enabled = 0;

//
// I_InitSound
//
void I_InitSound()
{
    // Check if sound is requested
    if (M_CheckParm("-nosound"))
        sound_enabled = 0;
    else
        sound_enabled = 0;  // Disabled by default
    
    if (sound_enabled)
        printf("DOOM: PC Speaker sound initialized\n");
    else
        printf("DOOM: Sound disabled\n");
}

//
// I_ShutdownSound
//
void I_ShutdownSound(void)
{
    if (sound_enabled)
        nosound();
}

//
// I_UpdateSound
//
void I_UpdateSound(void)
{
    // Not needed for PC speaker
}

//
// I_SubmitSound
//
void I_SubmitSound(void)
{
    // Not needed for PC speaker
}

//
// I_SetChannels
//
void I_SetChannels()
{
    // Not needed for PC speaker
}

//
// I_GetSfxLumpNum
//
int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

//
// I_StartSound
// Returns a handle (unused in this implementation)
//
int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    if (!sound_enabled)
        return 0;
    
    // Very basic: just beep for any sound
    // A real implementation would convert the sound data to frequencies
    beep();
    
    return id;
}

//
// I_StopSound
//
void I_StopSound(int handle)
{
    if (!sound_enabled)
        return;
    
    nosound();
}

//
// I_SoundIsPlaying
//
int I_SoundIsPlaying(int handle)
{
    // PC speaker plays sounds instantly and briefly
    return 0;
}

//
// I_UpdateSoundParams
//
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    // Not supported with PC speaker
}

//
// MUSIC FUNCTIONS (stub out - no music support)
//

void I_InitMusic(void)
{
    printf("DOOM: Music not supported\n");
}

void I_ShutdownMusic(void)
{
}

void I_SetMusicVolume(int volume)
{
    volume = 0;  // Unused
}

void I_PauseSong(int handle)
{
    handle = 0;  // Unused
}

void I_ResumeSong(int handle)
{
    handle = 0;  // Unused
}

int I_RegisterSong(void *data)
{
    data = NULL;  // Unused
    return 0;
}

void I_PlaySong(int handle, int looping)
{
    handle = looping = 0;  // Unused
}

void I_StopSong(int handle)
{
    handle = 0;  // Unused
}

void I_UnRegisterSong(int handle)
{
    handle = 0;  // Unused
}
