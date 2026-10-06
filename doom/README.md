# DOOM Port for inDevOS

This is a port of id Software's DOOM (version 1.10) to inDevOS, adapted from the original Linux source code.

## Features

- 320x200 graphics with 2x scaling (640x400 display)
- VGA palette-based rendering using inDevOS framebuffer
- Keyboard input support
- Single-player mode only (no network multiplayer)
- PC speaker sound (minimal)

## Building

```bash
cd doom
make clean
make
```

This will produce `doom.exe` - a PIE (Position Independent Executable) binary compatible with inDevOS.

## Installation

1. Build DOOM:
   ```bash
   make
   ```

2. Copy to disk image:
   ```bash
   make install
   ```
   
   Or manually:
   ```bash
   ../putfilesinsata.sh doom.exe
   ```

3. Get a DOOM WAD file:
   - Shareware: DOOM1.WAD (freely available)
   - Registered: DOOM.WAD (requires purchase)
   
   Copy the WAD file to your inDevOS disk image.

## Running

Boot inDevOS and run:

```
exec doom.exe
```

DOOM will look for WAD files in the current directory.

## Controls

- Arrow keys: Move/turn
- Ctrl: Fire
- Space: Use/Open doors
- Shift: Run
- Alt: Strafe
- Esc: Menu
- F1-F12: Various functions

## Technical Details

### Platform-Specific Files

- `i_system.c` - System interface (timing, memory, error handling)
- `i_video.c` - Video interface (framebuffer graphics)
- `i_sound.c` - Sound interface (PC speaker)
- `i_net.c` - Network interface (single-player stub)
- `i_main.c` - Main entry point
- `libc_shim.c` - Minimal C library implementation

### Dependencies

DOOM requires:
- inDevOS framebuffer support
- Keyboard input
- File system access (FAT32)
- Basic timing functions

### Memory Requirements

DOOM allocates approximately 6MB of heap memory for:
- Game data structures
- Texture cache
- Sprite cache
- WAD file data

## Known Limitations

1. **No sound effects** - Only PC speaker beeps (full sound would require sound card driver)
2. **No music** - Music playback not implemented
3. **No multiplayer** - Network code is stubbed out
4. **No save games** - File I/O for save games not fully implemented yet
5. **Fixed resolution** - 320x200 scaled to 640x400

## Troubleshooting

### DOOM doesn't start
- Ensure WAD file is present
- Check that doom.exe is properly loaded
- Verify keyboard input is working

### Graphics glitches
- Check framebuffer initialization
- Verify palette is loaded correctly

### Keyboard not responding
- Test keyboard in inDevOS shell first
- Check scancode translation in i_video.c

## License

DOOM source code is licensed under the GNU General Public License v2.0.
See LICENSE.TXT in the DOOM source directory.

Copyright (C) 1993-1996 id Software, Inc.
inDevOS port (C) 2026

## Credits

- Original DOOM by id Software
- Linux DOOM 1.10 port by id Software and contributors
- inDevOS port by the inDevOS team

## References

- Original DOOM source: https://github.com/id-Software/DOOM
- Shareware WAD: https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
- DOOM Wiki: https://doomwiki.org/
