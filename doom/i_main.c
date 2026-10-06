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
//      Main program for inDevOS DOOM port
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

// Fallback when kernel passes no argv (avoid NULL deref in FindResponseFile etc.)
static char *default_argv[] = { "doom", NULL };

// Simple main that calls DOOM's main function
int main(int argc, char **argv)
{
    if (argv == NULL) {
        argv = default_argv;
        argc = 1;
    }
    myargc = argc;
    myargv = argv;
    
    printf("=== inDevOS DOOM Port ===\n");
    printf("Based on Linux DOOM 1.10\n");
    printf("\n");
    
    // Call DOOM's main function
    D_DoomMain();
    
    return 0;
}
