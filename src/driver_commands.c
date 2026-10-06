#include "driver_manager.h"
#include "cmd.h"
#include "string.h"
#include "utils.h"

// Driver manager help command
void cmd_driver_help(void) {
    printf("\nDriver Manager Commands:\n");
    printf("=======================\n");
    printf("driver list          - List all registered drivers\n");
    printf("driver status <id>   - Show status of specific driver\n");
    printf("driver enable <id>   - Enable a driver\n");
    printf("driver disable <id>  - Disable a driver\n");
    printf("driver reset <id>    - Reset a driver\n");
    printf("driver unload <id>   - Unload a driver\n");
    printf("driver probe         - Probe all drivers\n");
    printf("driver discover      - Auto-discover and load drivers\n");
    printf("driver load <file>   - Load kernel module (.ko) via kmod\n");
    printf("driver save <id> <file> - Save driver to disk file\n");
    printf("driver help          - Show this help\n");
    printf("\n");
}

// List all drivers command
void cmd_driver_list(void) {
    driver_list_all();
}

// Load driver command
void cmd_driver_load(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver load <filename>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    printf("Loading driver from file: %s\n", args);
    int result = driver_load_from_disk(args);
    if (result == 0) {
        printf("Driver loaded successfully\n");
    } else {
        printf("Failed to load driver\n");
    }
}

// Unload driver command
void cmd_driver_unload(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver unload <id>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    uint32_t driver_id = kstrtoul(args, NULL, 10);
    if (driver_id == 0) {
        printf("Invalid driver ID\n");
        return;
    }
    
    printf("Unloading driver ID: %d\n", driver_id);
    int result = driver_unload(driver_id);
    if (result == 0) {
        printf("Driver unloaded successfully\n");
    } else {
        printf("Failed to unload driver\n");
    }
}

// Enable driver command
void cmd_driver_enable(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver enable <id>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    uint32_t driver_id = kstrtoul(args, NULL, 10);
    if (driver_id == 0) {
        printf("Invalid driver ID\n");
        return;
    }
    
    printf("Enabling driver ID: %d\n", driver_id);
    int result = driver_enable(driver_id);
    if (result == 0) {
        printf("Driver enabled successfully\n");
    } else {
        printf("Failed to enable driver\n");
    }
}

// Disable driver command
void cmd_driver_disable(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver disable <id>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    uint32_t driver_id = kstrtoul(args, NULL, 10);
    if (driver_id == 0) {
        printf("Invalid driver ID\n");
        return;
    }
    
    printf("Disabling driver ID: %d\n", driver_id);
    int result = driver_disable(driver_id);
    if (result == 0) {
        printf("Driver disabled successfully\n");
    } else {
        printf("Failed to disable driver\n");
    }
}

// Status command
void cmd_driver_status(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver status <id>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    uint32_t driver_id = kstrtoul(args, NULL, 10);
    if (driver_id == 0) {
        printf("Invalid driver ID\n");
        return;
    }
    
    driver_status(driver_id);
}

// Reset driver command
void cmd_driver_reset(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver reset <id>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    uint32_t driver_id = kstrtoul(args, NULL, 10);
    if (driver_id == 0) {
        printf("Invalid driver ID\n");
        return;
    }
    
    printf("Resetting driver ID: %d\n", driver_id);
    int result = driver_reset(driver_id);
    if (result == 0) {
        printf("Driver reset successfully\n");
    } else {
        printf("Failed to reset driver\n");
    }
}

// Probe command
void cmd_driver_probe(const char *args) {
    (void)args; // Unused parameter
    
    printf("Probing all drivers...\n");
    int result = driver_probe_all();
    printf("Probe complete, %d drivers probed\n", result);
}

// Discover command
void cmd_driver_discover(const char *args) {
    (void)args; // Unused parameter
    
    printf("Auto-discovering devices...\n");
    int result = driver_auto_discover();
    printf("Auto-discovery complete, loaded %d drivers\n", result);
}

// Save driver command
void cmd_driver_save(const char *args) {
    if (!args || !*args) {
        printf("Usage: driver save <id> <filename>\n");
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    // Parse driver ID
    uint32_t driver_id = kstrtoul(args, NULL, 10);
    if (driver_id == 0) {
        printf("Invalid driver ID\n");
        return;
    }
    
    // Find space to get filename
    while (*args && *args != ' ') args++;
    if (*args == ' ') args++;
    
    if (!*args) {
        printf("Usage: driver save <id> <filename>\n");
        return;
    }
    
    printf("Saving driver ID %d to file: %s\n", driver_id, args);
    int result = driver_save_to_disk(driver_id, args);
    if (result == 0) {
        printf("Driver saved successfully\n");
    } else {
        printf("Failed to save driver\n");
    }
}

// Main driver command handler
void cmd_driver(const char *args) {
    if (!args || !*args) {
        cmd_driver_help();
        return;
    }
    
    // Skip leading whitespace
    while (*args == ' ') args++;
    
    // Parse subcommand
    if (kstrncmp(args, "list", 4) == 0) {
        cmd_driver_list();
    } else if (kstrncmp(args, "status", 6) == 0) {
        cmd_driver_status(args + 6);
    } else if (kstrncmp(args, "enable", 6) == 0) {
        cmd_driver_enable(args + 6);
    } else if (kstrncmp(args, "disable", 7) == 0) {
        cmd_driver_disable(args + 7);
    } else if (kstrncmp(args, "reset", 5) == 0) {
        cmd_driver_reset(args + 5);
    } else if (kstrncmp(args, "unload", 6) == 0) {
        cmd_driver_unload(args + 6);
    } else if (kstrncmp(args, "load", 4) == 0) {
        cmd_driver_load(args + 4);
    } else if (kstrncmp(args, "save", 4) == 0) {
        cmd_driver_save(args + 4);
    } else if (kstrncmp(args, "probe", 5) == 0) {
        cmd_driver_probe(args + 5);
    } else if (kstrncmp(args, "discover", 8) == 0) {
        cmd_driver_discover(args + 8);
    } else if (kstrncmp(args, "help", 4) == 0) {
        cmd_driver_help();
    } else {
        printf("Unknown driver command. Use 'driver help' for usage.\n");
    }
} 