#!/bin/bash
# Helper script to convert debug printf statements to use the debug system
# Usage: ./convert_debug.sh <subsystem> <file>
# Example: ./convert_debug.sh RTL8139 src/rtl8139.c

if [ $# -ne 2 ]; then
    echo "Usage: $0 <SUBSYSTEM> <file>"
    echo "Example: $0 RTL8139 src/rtl8139.c"
    echo ""
    echo "This will convert printf/serial_printf to DEBUG_PRINT/SERIAL_DEBUG_PRINT"
    echo "for the specified subsystem."
    exit 1
fi

SUBSYSTEM=$1
FILE=$2

if [ ! -f "$FILE" ]; then
    echo "Error: File $FILE not found"
    exit 1
fi

echo "Converting $FILE to use DEBUG_$SUBSYSTEM..."

# Check if debug_config.h is already included
if ! grep -q "debug_config.h" "$FILE"; then
    echo "Adding #include \"debug_config.h\" to $FILE"
    # Find the last #include line and add after it
    sed -i '/^#include/a #include "debug_config.h"' "$FILE"
fi

echo "Manual conversion needed:"
echo "1. Replace verbose printf() with DEBUG_PRINT($SUBSYSTEM, ...)"
echo "2. Replace verbose serial_printf() with SERIAL_DEBUG_PRINT($SUBSYSTEM, ...)"
echo "3. Keep error messages as regular printf() (no macro)"
echo ""
echo "Example:"
echo "  OLD: printf(\"$SUBSYSTEM: Debug info\\n\");"
echo "  NEW: DEBUG_PRINT($SUBSYSTEM, \"$SUBSYSTEM: Debug info\\n\");"
echo ""
echo "See DEBUG_SYSTEM_USAGE.md for more examples"
