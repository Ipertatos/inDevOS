#include "term.h"
#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"
#include "timer.h"
#include "gdt.h"
#include "acpi.h"
#include "hpet.h"
#include "mem.h"
#include "cpu.h"    
#include "pci.h"    
#include "nvme.h"
#include "ide.h"
#include "ahci.h"
#include "sound.h"
#include "paratrooper.h"
#include "rtl8169.h"
#include "storage.h"
#include "serial.h"
#include "cmd.h"
#include "debug.h"

// Global flag that enables or disables the serial debug port.  Change to
// 'false' (or make it configurable later through boot parameters) to turn
// all serial output off.  serial.c checks this before touching the UART so
// nothing is wasted waiting on the hardware.
bool serial_enabled = true;

static volatile LIMINE_BASE_REVISION(0);
void kmain(void) {
    // Initialize serial port first for debugging
    serial_init();
    DEBUG_SERIAL("Serial debug initialized\n");
    DEBUG_SERIAL("Entering kmain()\n");
    
    //printk("Kernel started\n");

    if(LIMINE_BASE_REVISION_SUPPORTED == false) {
        //printk("Limine revision not supported\n");
        DEBUG_SERIAL("Limine revision not supported - exiting\n");
        return;
    }

    DEBUG_SERIAL("Initializing GDT\n");
    gdt_init();
    DEBUG_SERIAL("Initializing IDT\n");
    initIDT();
    

    //printk("Limine revision supported\n");
    DEBUG_SERIAL("Initializing framebuffer\n");
    // Debug framebuffer request to check availability
    debug_framebuffer_request();
    term_init();
    
    // NOTE: Removed DEBUG macros from early initialization as they call printf() 
    // which is unsafe before ft_ctx is properly initialized

    printf("Initializing Interrupts\n");
    

    printf("IDT initialized\n");
    printf("Checking APIC\n");

    // Use serial debug for early initialization instead of printf-based DEBUG macros
    DEBUG_SERIAL("Initializing CPU\n");
    cpu_init();
    DEBUG_SERIAL("Initializing kernel memory\n");
    kmem_init();
    DEBUG_SERIAL("Initializing ACPI\n");
    init_acpi();
    DEBUG_SERIAL("Initializing PCI\n");
    pci_init();

    nvme_init();   
    ide_init();
    ahci_init();
    
    // Initialize keyboard subsystem
    printf("Initializing keyboard\n");
    keyboard_init();
    
    // Initialize storage and filesystem
    printf("Initializing storage subsystem\n");
    debug_memory_check("Before storage_init");
    storage_init();
    debug_memory_check("After storage_init");
    storage_init_ramdisk();
    debug_memory_check("After storage_init_ramdisk");
    
    // Auto-execute mount 0 for debugging
    // Test basic commands without file I/O to avoid page faults
    printf("Basic command test:\n");
    printf("1. Running 'help' command:\n");
    debug_memory_check("Before help command");
    cmd("help", 4);
    debug_memory_check("After help command");
    
    printf("\n2. Running 'storage' command:\n");
    debug_memory_check("Before storage command");
    cmd("storage", 7);
    debug_memory_check("After storage command");
    
    printf("\nFile I/O commands available but need debugging.\n");
    printf("You can manually test: mount, ls, touch, mkdir, cat, rm, cp, mv\n");
    debug_memory_check("Before entering main loop");
    
    //beep();
    DEBUG_SERIAL("Entering main loop - ft_ctx should be initialized\n");
    printf("Main loop started - ready for input\n");
    //for easy debug 
    mount_filesystem_device(0);
    // CRITICAL FIX: Enable interrupts AFTER all boot printf calls are complete
    __asm__("sti");
    //PlayWAV(44100, fsize, file);
    //rtl8169_init();
    
    while(1) {
        // Simple NULL check without accessing private fields

        
        // Process keyboard echo safely from main thread context
        if(keyboard_has_echo_pending()) {
            keyboard_process_echo();
        }
        
        if(keyboard_get_execute()) {
            char* buffer = keyboard_get_buffer();
            uint32_t buff_len = keyboard_get_buffer_length();
            cmd(buffer, buff_len);
            
            // Clear the keyboard buffer properly
            keyboard_clear_buffer();
        }
        __asm__("nop");
    }

}