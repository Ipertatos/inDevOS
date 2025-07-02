#include "cmd.h"
#include "utils.h"
#include "mem.h"
#include "usoft/info.h"
#include "hpet.h"
#include "storage.h"
#include "fat32.h"
#include "serial.h"

void cmd(char* cmd, uint64_t len){
    //reset strtok
    char* tmp = strtok(NULL, " ");
    char* token = strtok(cmd, " ");     
    token = remove_nl(token);
    //the first token is the command
    if(strEql(token, "clear")){
        clear();
    }
    else if(strEql(token, "echo")){
        token = strtok(NULL, " ");
        while (!strEql((token), "\0")){
            printf("{s}", token);
            token = strtok(NULL, " ");
            if(!strEql(token, "\0")){
                printf(" ");
            }
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
        token = strtok(NULL, " ");
        if (token == NULL) {
            printf("Usage: mount <device_id>{n}");
            printf("Example: mount 0 (for RAM disk) or mount 1 (for attached drive){n}");
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
    else if(strEql(token, "help")){
        show_help();
    }
    else {
        printf("Unknown command: {s}{n}", token);
        printf("Type 'help' for available commands{n}");
    }
    strtok(NULL, NULL);
}

void clear(){
    clear_screen();
}

void timer_test(){
    uint64_t start = hpet_get_ticks();
    uint64_t t = hpet_get_ticks();
    while(t - start < 1000){
        printf("{dn}",t - start);
        t = hpet_get_ticks();
    };
    printf("Timer test passed\n");
}

void mount_filesystem(){
    mount_filesystem_device(0);
}

void mount_filesystem_device(uint8_t device_id){
    printf("Mounting FAT32 filesystem from device {d}...{n}", device_id);
    if (fat32_mount(device_id) == 0) {
        printf("Filesystem mounted successfully!{n}");
    } else {
        printf("Failed to mount filesystem from device {d}{n}", device_id);
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
        printf("No filesystem is currently mounted{n}");
    }
}

void list_files(){
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.{n}");
        return;
    }
    
    printf("Directory listing:{n}");
    printf("Type   Size       Name{n}");
    printf("-----  ---------- --------{n}");
    
    fat32_dir_list_t entries[32];
    int count = fat32_list_directory("/", entries, 32);
    
    if (count < 0) {
        printf("Error reading directory{n}");
        return;
    }
    
    if (count == 0) {
        printf("(empty directory){n}");
        return;
    }
    
    for (int i = 0; i < count; i++) {
        printf("{s}   {d:10}   {s}{n}", 
               entries[i].is_directory ? "DIR " : "FILE",
               entries[i].size,
               entries[i].name);
    }
    
    printf("{n}Total: {d} items{n}", count);
}

void filesystem_info(){
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted{n}");
        return;
    }
    
    printf("Filesystem Information:{n}");
    printf("Type: FAT32{n}");
    
    // Safely print volume label (ensure null termination)
    char vol_label[12];
    memmove(vol_label, fat32_filesystem.boot_sector.volume_label, 11);
    vol_label[11] = '\0';
    printf("Volume Label: '{s}'{n}", vol_label);
    
    printf("Bytes per Sector: {d}{n}", fat32_filesystem.boot_sector.bytes_per_sector);
    printf("Sectors per Cluster: {d}{n}", fat32_filesystem.boot_sector.sectors_per_cluster);
    printf("Bytes per Cluster: {d}{n}", fat32_filesystem.bytes_per_cluster);
    printf("Total Sectors: {d}{n}", fat32_filesystem.boot_sector.total_sectors_32);
    printf("FAT Size: {d} sectors{n}", fat32_filesystem.boot_sector.fat_size_32);
    printf("Root Directory Cluster: {d}{n}", fat32_filesystem.root_dir_cluster);
    printf("FAT Start Sector: {d}{n}", fat32_filesystem.fat_start_sector);
    printf("Data Start Sector: {d}{n}", fat32_filesystem.data_start_sector);
}

void show_storage_devices(){
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    
    DEBUG_SERIAL("show_storage_devices: device_count=%d\n", storage_device_count);
    
    printf("Storage Devices:{n}");
    printf("ID  Name                     Sectors      Size{n}");
    printf("--  ----------------------   ----------   --------{n}");
    
    if (storage_device_count == 0) {
        printf("No storage devices found{n}");
        DEBUG_SERIAL("show_storage_devices: No devices found\n");
        return;
    }
    
    for (uint8_t i = 0; i < storage_device_count; i++) {
        storage_device_t *dev = &storage_devices[i];
        DEBUG_SERIAL("show_storage_devices: device[%d]: initialized=%d, name='%s', sectors=%d\n", 
                     i, dev->is_initialized, dev->name, dev->sector_count);
        
        if (dev->is_initialized) {
            uint64_t size_mb = (dev->sector_count * dev->sector_size) / (1024 * 1024);
            printf("{d:2}  {s:22}   {d:10}   {d} MB{n}", 
                   dev->device_id, 
                   dev->name,
                   dev->sector_count,
                   size_mb);
        }
    }
    printf("{n}Total devices: {d}{n}", storage_device_count);
}

void show_help(){
    printf("Available commands:{n}");
    printf("  help       - Show this help message{n}");
    printf("  clear      - Clear the screen{n}");
    printf("  echo <msg> - Echo a message{n}");
    printf("  neofetch   - Show system information{n}");
    printf("  timertest  - Test the timer subsystem{n}");
    printf("  storage    - Show storage devices{n}");
    printf("  mount <id> - Mount FAT32 filesystem from device ID{n}");
    printf("  unmount    - Unmount the filesystem{n}");
    printf("  ls         - List files and directories{n}");
    printf("  fsinfo     - Show filesystem information{n}");
}