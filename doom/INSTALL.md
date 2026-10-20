# DOOM Installation Guide for inDevOS

## Quick Start

### 1. Copy DOOM to Disk Image

```bash
cd /path/to/inDevOS
./putfilesinsata.sh doom/doom.exe
```

### 2. Download and Copy WAD File

```bash
# Download shareware DOOM1.WAD (free and legal)
wget https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad

# Copy to disk
./putfilesinsata.sh doom1.wad
```

### 3. Boot inDevOS

Boot your inDevOS and you should see the Zeus shell.

### 4. Run DOOM

```
exec doom.exe
```

## What Should Happen

1. DOOM banner appears
2. Game initializes graphics (320x200 @ 640x400)
3. Loads DOOM1.WAD
4. Shows title screen
5. Ready to play!

## Controls

- **Arrow Keys**: Move forward/backward, turn left/right
- **Ctrl**: Fire weapon
- **Space**: Use/activate (open doors, press switches)
- **Shift**: Run
- **Alt**: Strafe mode
- **Esc**: Menu
- **Tab**: Automap
- **1-7**: Select weapon

## Troubleshooting

### "Please set $HOME to your home directory"
✅ Fixed in latest build - DOOM now uses "/" as home

### "no files found" or WAD errors
- Make sure `doom1.wad` is on the disk image
- DOOM looks in current directory (wherever doom.exe is)
- File name must be lowercase: `doom1.wad` not `DOOM1.WAD`

### No graphics / black screen
- Check framebuffer initialization in serial.log
- Verify VGA/graphics mode is set up

### Keyboard not working
- Test keyboard in Zeus shell first
- Check scancode translation in serial.log

### Game crashes
- Check serial.log for error messages
- Memory allocation issues may appear as page faults

## Files Needed

### Minimum (Shareware)
- `doom.exe` (387KB) - The game binary
- `doom1.wad` (4.2MB) - Shareware game data

### Full Version (if you own it)
- `doom.exe` (387KB) - The game binary  
- `doom.wad` (11MB) - Full game data

## Performance Notes

- Target: 35 FPS (DOOM's original tick rate)
- Resolution: 320x200 scaled to 640x400
- Memory: ~6MB heap allocation
- No sound effects/music yet (PC speaker only)

## Current Limitations

1. ❌ No save games (file I/O partial)
2. ❌ No sound effects/music (PC speaker beep only)
3. ❌ No multiplayer
4. ❌ Fixed resolution (320x200)
5. ✅ Single player works!
6. ✅ All levels accessible
7. ✅ Full keyboard control

## Where to Get WAD Files

### Shareware (Free & Legal)
- https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
- Contains first episode (9 levels)

### Full Game (Purchase Required)
- Steam: https://store.steampowered.com/app/2280/DOOM/
- GOG: https://www.gog.com/game/doom_ii
- Extract `DOOM.WAD` from installation

## Advanced: Debugging

View detailed logs:
```bash
# On host machine
tail -f /path/to/inDevOS/serial.log
```

Look for:
- "sys_print" messages from DOOM
- "DOOM Error:" for crashes
- Page fault messages for memory errors

## Next Steps

Once working:
1. Try different difficulty levels (I'm Too Young to Die through Nightmare)
2. Explore all shareware levels
3. Test keyboard responsiveness
4. Check performance (should be smooth)

Enjoy DOOM on your custom OS! 🎮🔥
