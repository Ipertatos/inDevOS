#include "cmd.h"
#include "utils.h"
#include "mem.h"
#include "string.h" // Added for strlen
#include "usoft/info.h"
#include "hpet.h"
#include "storage.h"
#include "fat32.h"
#include "serial.h"
#include "debug.h"
#include "usoft/matrix.h"
#include "exec.h" // added for executable support

#define CMD_DELIMS " \t\r\n\b\x7f"

void cmd(char* cmd, uint64_t len){
    // NOTE: Removed DEBUG macros to prevent printf() crashes - use serial debug instead
    // DEBUG_ENTER_ARGS("cmd='%.20s', len=%llu", cmd, len);
    DEBUG_SERIAL("Entering cmd()\n");
    serial_printf("[cmd] raw='%s' len=%d\n", cmd, (int)len);
    
    if (cmd == NULL) {
        DEBUG_SERIAL("cmd() called with NULL command\n");
        return;
    }
    if (((uintptr_t)cmd) < 0x1000) {
        printf("cmd(): ERROR: cmd pointer is NULL or invalid (%p)\n", cmd);
        return;
    }
    
    // no strtok reset needed with new implementation
    char* token = strtok(cmd, CMD_DELIMS);     
    
    if (token == NULL) {
        DEBUG_SERIAL("strtok() returned NULL token\n");
        return;
    }
    
    token = remove_nl(token);
    
    if (token == NULL) {
        DEBUG_SERIAL("remove_nl() returned NULL token\n");
        return;
    }
    
    // DEBUG_MSG("Processing command: '%s'", token);
    serial_printf("Processing command: '%s'\n", token);


    //the first token is the command
    if(strEql(token, "clear")){
        clear();
    }
    else if(strEql(token, "echo")){
        char message[512] = {0};
        size_t msg_len = 0;
        char *out_file = NULL;
        bool first_word = true;


        // Collect tokens to build message and detect redirection symbols
        while (token != NULL) {

            if(token[0]=='\0') {
                continue; // Skip empty tokens (e.g., consecutive delimiters)
            }
            // Handle tokens starting with '>' (could be '>', '>>', '>filename', '>>filename')
            if (token[0] == '>') {
                serial_printf("[echo] redirection detected, token='%s'\n", token);
                // Handle any number of leading '>' characters (keyboard may repeat)
                size_t gt_count = 0;
                while (token[gt_count] == '>') gt_count++;
                bool append_mode = (gt_count > 1); // TODO: implement append

                // Skip the '>' chars to find filename (may be adjacent)
                size_t offset = gt_count;
                if (token[offset] != '\0') {
                    out_file = &token[offset];
                } else {
                    // Filename is in next token
                    out_file = strtok(NULL, CMD_DELIMS);
                }

                // Debug
                serial_printf("[echo] redirection detected, append=%d, out_file_token='%s'\n", append_mode?1:0, out_file ? out_file : "(null)");

                break;
            }

            if (!first_word) {
                if (msg_len < sizeof(message) - 1) {
                    message[msg_len++] = ' ';
                }
            }
            size_t tlen = strlen(token);
            if (msg_len + tlen >= sizeof(message) - 1) {
                tlen = sizeof(message) - 1 - msg_len;
            }
            memmove(&message[msg_len], token, tlen);
            msg_len += tlen;
            message[msg_len] = '\0';
            first_word = false;
            serial_printf("[echo] token='%s'\n", token);
            token = strtok(NULL, CMD_DELIMS);
        }

        // Debug output of parsing results
        serial_printf("[echo] message='%s' len=%d out_file=%s\n", message, (int)msg_len, out_file ? out_file : "(null)");

        if (out_file) {
            if (!fat32_is_mounted()) {
                printf("No filesystem mounted. Use 'mount' command first.\n");
            } else {
                if (!fat32_file_exists(out_file)) {
                    fat32_create_file(out_file);
                }
                fat32_file_t *f = fat32_open(out_file, "w");
                if (!f) {
                    printf("Failed to open '%s' for writing\n", out_file);
                } else {
                    fat32_write(f, message, (uint32_t)strlen(message));
                    fat32_close(f);
                    serial_printf("[echo] Wrote %d bytes to '%s'\n", (int)strlen(message), out_file);
                }
            }
        } else {
            printf("%s\n", msg_len==0?"":message);
        }
    }
    else if(strEql(token, "neofetch"))
    {
        print_info();
    }
    else if(strEql(token, "timertest")){
        timer_test();
    }
    else if(strEql(token, "mount")){
        token = strtok(NULL, CMD_DELIMS);
        if (token == NULL) {
            printf("Usage: mount <device_id>\n");
            printf("Example: mount 0 (for RAM disk) or mount 1 (for attached drive)\n");
        } else {
            uint8_t device_id = (uint8_t)atoi(token);
            mount_filesystem_device(device_id);
        }
    }
    else if(strEql(token, "unmount")){
        unmount_filesystem();
    }
    else if(strEql(token, "ls")){
        list_files();
    }
    else if(strEql(token, "fsinfo")){
        filesystem_info();
    }
    else if(strEql(token, "storage")){
        show_storage_devices();
    }
    else if(strEql(token, "cat")){
        token = strtok(NULL, CMD_DELIMS);
        if (token == NULL) {
            printf("Usage: cat <filename>\n");
        } else {
            cat_file(token);
        }
    }
    else if(strEql(token, "touch")){
        token = strtok(NULL, CMD_DELIMS);
        if (token == NULL) {
            printf("Usage: touch <filename>\n");
        } else {
            printf("touch_file arg: '%s'\n", token);
            touch_file(token);
        }
    }
    else if(strEql(token, "mkdir")){
        token = strtok(NULL, CMD_DELIMS);
        if (token == NULL) {
            printf("Usage: mkdir <dirname>\n");
        } else {
            make_directory(token);
        }
    }
    else if(strEql(token, "rm")){
        token = strtok(NULL, CMD_DELIMS);
        if (token == NULL) {
            printf("Usage: rm <filename>\n");
        } else {
            remove_file(token);
        }
    }
    else if(strEql(token, "cp")){
        char *source = strtok(NULL, CMD_DELIMS);
        char *dest = strtok(NULL, CMD_DELIMS);
        if (source == NULL || dest == NULL) {
            printf("Usage: cp <source> <destination>\n");
        } else {
            copy_file(source, dest);
        }
    }
    else if(strEql(token, "mv")){
        char *source = strtok(NULL, CMD_DELIMS);
        char *dest = strtok(NULL, CMD_DELIMS);
        if (source == NULL || dest == NULL) {
            printf("Usage: mv <source> <destination>\n");
        } else {
            move_file(source, dest);
        }
    }
    else if(strEql(token, "dir")){
        list_files(); // dir is an alias for ls
    }
    else if(strEql(token, "help")){
        show_help();
    }
    else if(strEql(token, "font2x")){
        fb_set_font_scale(2);
    }
    else if(strEql(token, "font1x")){
        fb_set_font_scale(1);
    }
    else if(strEql(token, "matrix")){
        matrix_rain();
    }
    else if(strEql(token, "exec")){
        char *prog = strtok(NULL, CMD_DELIMS);
        if(prog == NULL){
            printf("Usage: exec <filename>\n");
        } else {
            exec_file(prog);
        }
    }
    else {
        printf("Unknown command: %s\n", token);
        printf("Type 'help' for available commands\n");
    }
    
}

void clear(){
    clear_screen();
}

void timer_test(){
    uint64_t start = hpet_get_ticks();
    uint64_t t = hpet_get_ticks();
    while(t - start < 1000){
        printf("%llu\n", (unsigned long long)(t - start));
        t = hpet_get_ticks();
    };
    printf("Timer test passed\n");
}

void mount_filesystem(){
    mount_filesystem_device(0);
}

void mount_filesystem_device(uint8_t device_id){
    printf("Mounting FAT32 filesystem from device %d...\n", device_id);
    if (fat32_mount(device_id) == 0) {
        printf("Filesystem mounted successfully!\n");
    } else {
        printf("Failed to mount filesystem from device %d\n", device_id);
    }
}

// Simple string to integer conversion
int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    if (*str == '-') {
        sign = -1;
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

void unmount_filesystem(){
    if (fat32_is_mounted()) {
        fat32_unmount();
    } else {
        printf("No filesystem is currently mounted\n");
    }
}

void list_files(){
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }
    
    printf("Directory listing:\n");
    printf("Type   Size       Name\n");
    printf("-----  ---------- --------\n");
    
    static fat32_dir_list_t entries[32];  // Static to avoid stack overflow (8KB+)
    int count = fat32_list_directory("/", entries, 32);
    
    if (count < 0) {
        printf("Error reading directory\n");
        return;
    }
    
    if (count == 0) {
        printf("(empty directory)\n");
        return;
    }
    
    for (int i = 0; i < count; i++) {
        printf("%s   %10d   %s\n", 
               entries[i].is_directory ? "DIR " : "FILE",
               entries[i].size,
               entries[i].name);
    }
    
    printf("\nTotal: %d items\n", count);
}

void filesystem_info(){
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted\n");
        return;
    }
    
    printf("Filesystem Information:\n");
    printf("Type: FAT32\n");
    
    // Safely print volume label (ensure null termination)
    char vol_label[12];
    memmove(vol_label, fat32_filesystem.boot_sector.volume_label, 11);
    vol_label[11] = '\0';
    printf("Volume Label: '%s'\n", vol_label);
    
    printf("Bytes per Sector: %d\n", fat32_filesystem.boot_sector.bytes_per_sector);
    printf("Sectors per Cluster: %d\n", fat32_filesystem.boot_sector.sectors_per_cluster);
    printf("Bytes per Cluster: %d\n", fat32_filesystem.bytes_per_cluster);
    printf("Total Sectors: %d\n", fat32_filesystem.boot_sector.total_sectors_32);
    printf("FAT Size: %d sectors\n", fat32_filesystem.boot_sector.fat_size_32);
    printf("Root Directory Cluster: %d\n", fat32_filesystem.root_dir_cluster);
    printf("FAT Start Sector: %d\n", fat32_filesystem.fat_start_sector);
    printf("Data Start Sector: %d\n", fat32_filesystem.data_start_sector);
}

void show_storage_devices(){
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    
    DEBUG_SERIAL("show_storage_devices: device_count=%d\n", storage_device_count);
    
    printf("Storage Devices:\n");
    printf("ID  Name                     Sectors      Size\n");
    printf("--  ----------------------   ----------   --------\n");
    
    if (storage_device_count == 0) {
        printf("No storage devices found\n");
        DEBUG_SERIAL("show_storage_devices: No devices found\n");
        return;
    }
    
    for (uint8_t i = 0; i < storage_device_count; i++) {
        storage_device_t *dev = &storage_devices[i];
        DEBUG_SERIAL("show_storage_devices: device[%d]: initialized=%d, name='%s', sectors=%d\n", 
                     i, dev->is_initialized, dev->name, dev->sector_count);
        
        if (dev->is_initialized) {
            uint64_t size_mb = ((uint64_t)dev->sector_count * dev->sector_size) / (1024 * 1024);
            printf("%u  %s   %u   %llu MB\n", 
                   dev->device_id,
                   dev->name,
                   dev->sector_count,
                   (unsigned long long)size_mb);
        }
    }
    printf("\nTotal devices: %d\n", storage_device_count);
}

void show_help(){
    printf("Available commands:\n");
    printf("  help       - Show this help message\n");
    printf("  clear      - Clear the screen\n");
    printf("  echo <msg> - Echo a message (use '>' to redirect to file)\n");
    printf("  neofetch   - Show system information\n");
    printf("  timertest  - Test the timer subsystem\n");
    printf("  storage    - Show storage devices\n");
    printf("  mount <id> - Mount FAT32 filesystem from device ID\n");
    printf("  unmount    - Unmount the filesystem\n");
    printf("  ls / dir   - List files and directories\n");
    printf("  fsinfo     - Show filesystem information\n");
    printf("  cat <file> - Display file contents\n");
    printf("  touch <file> - Create empty file\n");
    printf("  mkdir <dir>  - Create directory\n");
    printf("  rm <file>    - Remove file\n");
    printf("  cp <src> <dst> - Copy file\n");
    printf("  mv <src> <dst> - Move/rename file\n");
    printf("  exec <file>  - Load and execute binary file\n");
}

// File I/O command implementations

void cat_file(const char *filename) {
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }
    
    printf("Reading file: %s\n", filename);
    
    fat32_file_t *file = fat32_open(filename, "r");
    if (!file) {
        printf("Failed to open file '%s'\n", filename);
        return;
    }
    
    // Read and display file contents in chunks
    static char buffer[1024];  // Static to avoid stack overflow
    int bytes_read;
    
    while ((bytes_read = fat32_read(file, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0'; // Null terminate for printf
        printf("%s", buffer);
    }
    printf("\n");
    
    fat32_close(file);
}

void touch_file(const char *filename) {
    printf("touch_file called\n");
    fat32_create_file(filename);
    printf("touch_file done\n");
}

void make_directory(const char *dirname) {
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }
    
    if (fat32_create_directory(dirname) == 0) {
        printf("Created directory '%s'\n", dirname);
    } else {
        printf("Failed to create directory '%s'\n", dirname);
    }
}

void remove_file(const char *filename) {
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }
    
    printf("Attempting to remove file '%s'...\n", filename);
    
    if (fat32_delete_file(filename) == 0) {
        printf("Removed file '%s'\n", filename);
    } else {
        printf("Failed to remove file '%s' (not fully implemented yet)\n", filename);
    }
}

void copy_file(const char *source, const char *dest) {
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }
    
    printf("File copy not fully implemented yet\n");
    printf("Would copy '%s' to '%s'\n", source, dest);
}

void move_file(const char *source, const char *dest) {
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }
    
    printf("File move not fully implemented yet\n");
    printf("Would move '%s' to '%s'\n", source, dest);
}