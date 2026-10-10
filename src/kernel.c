#include "term.h"
#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "mouse.h"
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
#include "sound.h"
#include "paratrooper.h"
#include "e1000.h"
#include "rtl8139.h"
#include "net_device.h"
#include "net_tcp.h"
#include "net_ip.h"
#include "net_udp.h"
#include "fat32.h"
#include "storage.h"
#include "serial.h"
#include "cmd.h"
#include "debug.h"
#include "zeus.h"
#include "vga.h"
#include "net_thread.h"
#include "usb.h"
#include "driver_manager.h"
#include "driver_adapters.h"
#include "rtc.h"
#include "fpu.h"
#include "syscall.h"
#include "string.h"
#include "ac97.h"
#include "pipe.h"
#include "smp.h"
#include "vconsole.h"
#include "xhci.h"
#include "kmod.h"
#include "virtio_gpu.h"
#include "gpu_hal.h"
#include "drm.h"
#include "telnet_server.h"
#include "vfs.h"
#include "tty.h"
#include "profiler.h"
#include "thread.h"
#include "desktop.h"

// Global flag that enables or disables the serial debug port.  Change to
// 'false' (or make it configurable later through boot parameters) to turn
// all serial output off.  serial.c checks this before touching the UART so
// nothing is wasted waiting on the hardware.
bool serial_enabled = true;

#define DEBUG_ALL_NETWORK 0

static volatile LIMINE_BASE_REVISION(0);

/* TTY line callback: dispatches completed input lines to Zeus.
 * Called from tty_poll() in the main loop, never from ISR context. */
static void tty_zeus_line_handler(int tty_id, const char *line, uint32_t len) {
    (void)tty_id;
    (void)len;
    extern int zeus_process_command(const char *command_line);
    zeus_process_command(line);
}

void kmain(void) {
    // CRITICAL: Clear BSS section first to ensure all static variables are zeroed
    // The linker places BSS after .data in the data segment
    // We need to find where BSS starts within the data segment
    extern char __bss_start[];
    extern char __bss_end[];
    
    // Clear BSS section manually since we don't have C runtime startup
    char *bss_start = (char*)&__bss_start;
    char *bss_end = (char*)&__bss_end;
    
    if (bss_end <= bss_start) {
        serial_init();
        serial_puts("FATAL: linker BSS symbols invalid (__bss_start/__bss_end)\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    memset(bss_start, 0, (size_t)(bss_end - bss_start));
    
    // Initialize serial port first for debugging
    if(serial_enabled) {
        serial_init();
        DEBUG_SERIAL("Serial debug initialized\n");
    }
    DEBUG_SERIAL("Entering kmain()\n");
    
    //printk("Kernel started\n");

    if(LIMINE_BASE_REVISION_SUPPORTED == false) {
        //printk("Limine revision not supported\n");
        DEBUG_SERIAL("Limine revision not supported - exiting\n");
        return;
    }

    /* From here on kmain() must never return: its own canary predates this. */
    stack_guard_init();

    DEBUG_SERIAL("Initializing GDT\n");
    gdt_init();
    DEBUG_SERIAL("Initializing IDT\n");
    initIDT();
    

    //printk("Limine revision supported\n");
    DEBUG_SERIAL("Initializing framebuffer\n");
    // Debug framebuffer request to check availability
    debug_framebuffer_request();
    term_init();
    // Terminal is up: klog may now mirror messages to the console.
    klog_console_ready();

    klog_info("Initializing CPU\n");
    cpu_init();
    cpu_enable_protections();
    klog_info("CPU protections: NX=%s SMEP=%s SMAP=%s\n",
              vmm_nx_bit ? "on" : "off",
              cpu_smep_enabled ? "on" : "off",
              cpu_smap_enabled ? "on" : "off");
    fpu_sse_init();
    klog_info("Initializing kernel memory\n");
    kmem_init();
    /* The framebuffer console came up in term_init() before the PMM existed,
     * so it started out drawing straight into the framebuffer.  Now that
     * kalloc() works, switch it to double buffering. */
    fb_console_alloc_backbuffer();
    scheduler_init();
    klog_info("Initializing virtual memory management\n");
    vmm_init();
    klog_debug("Initializing page reference counts (COW)\n");
    vmm_refcount_init();
    smap_selftest();
    klog_debug("Initializing VGA text mode buffer\n");
    vga_init();  // Initialize VGA buffer with HHDM offset for matrix command
    klog_debug("Initializing memory pools\n");
    // Create memory pools for common allocation sizes
    mempool_create(64, 1024);    // Small allocations (64 bytes x 1024)
    mempool_create(256, 512);    // Medium allocations (256 bytes x 512)
    mempool_create(1024, 256);   // Large allocations (1KB x 256)
    klog_info("Initializing ACPI\n");
    init_acpi();
    acpi_power_init();
    klog_info("Initializing PCI\n");
    pci_init();
    
    // Initialize driver manager and adapters
    klog_info("Initializing driver manager\n");
    driver_manager_init();
    driver_adapters_init();
    driver_adapters_register_all();
    
    // Auto-discover and load drivers
    klog_info("Auto-discovering devices\n");
    driver_auto_discover();
    
    // Try virtio-GPU first (preferred for QEMU). It has no PCI registration
    // in driver_manager yet, so probe it directly; AMD GPU / VMware SVGA /
    // NVMe / IDE / AHCI / NICs are initialized by driver_auto_discover()
    // above via their driver_manager adapters (all inits are idempotent).
    klog_info("Probing virtio-GPU\n");
    if (virtio_gpu_init() < 0) {
        klog_info("Virtio-GPU not available, using driver-manager GPU drivers\n");
    }

    // GPU HAL: unify all display backends behind one interface
    gpu_hal_init();
    drm_init();

    /* Console drew to the Limine FB before virtio-gpu came up; push it once
     * to the virtio scanout so the QEMU window is not blank/stale. */
    if (virtio_gpu_is_available()) {
        fb_mark_dirty_all();
        fb_flush();
        fb_present_scanout();
    }
    
    // Initialize USB subsystem
    klog_info("Initializing USB subsystem\n");
    usb_init();
    
    // xHCI controllers, then enumerate root ports and hubs; binds HID
    // keyboards/mice and mass storage (usbN disks registered here are seen
    // by storage_init's boot-device detection and the installer).
    klog_info("Initializing xHCI USB controllers\n");
    {
        extern void usb_core_init(void);
        usb_core_init();
    }
    
    // Initialize AC97 audio driver
    klog_info("Initializing audio subsystem\n");
    ac97_init();
    
    // Initialize audio mixer
    extern int mixer_init(void);
    mixer_init();
    
    // Initialize clipboard and settings
    extern int clipboard_init(void);
    extern int settings_init(void);
    clipboard_init();
    settings_init();

    // Initialize user accounts and permissions
    extern void users_init(void);
    users_init();

    // Initialize dynamic linker
    extern void dynlink_init(void);
    dynlink_init();

    // Initialize package manager
    extern void pkg_init(void);
    pkg_init();
    
    // Set up the 1ms HPET periodic timer: drives preemptive scheduling,
    // thread sleep wakeups and cursor blink (was done by the old task manager)
    klog_info("Configuring 1ms periodic timer\n");
    hpet_set_periodic(1);

    // Initialize syscall system
    klog_info("Initializing syscall system\n");
    syscall_init();

    profiler_init();
    
    // Initialize network device abstraction layer and PCI ethernet (E1000 > RTL8139 > PCnet)
    klog_info("Initializing network device abstraction layer\n");
    net_device_init();
    if (net_device_pci_bringup() != 0) {
        klog_warn("No PCI ethernet driver initialized\n");
    } else if (net_get_driver_name()) {
        klog_info("Using %s as network driver\n", net_get_driver_name());
    }
    
    // Initialize network protocols
    klog_info("Initializing network protocols\n");
    ip_init();
    udp_init();
    tcp_init();
    
    // Network RX is serviced from the main loop (net_rx_service), gated on
    // the IRQ-set flag from NIC interrupt handlers — no init needed here.

    // Start the internal Telnet server (listens on port 23)
    telnet_server_init();
    
    // Initialize keyboard subsystem
    klog_info("Initializing keyboard\n");
    keyboard_init();
    
    // Initialize mouse driver (memory allocator is now interrupt-safe)
    klog_info("Initializing mouse\n");
    mouse_init();
    
    // Initialize RTC (wall-clock time)
    rtc_init();

    // Initialize storage and filesystem
    /* Coalesce console GPU flushes from here on. The main loop (and the
     * checkpoints below) call fb_flush_pending(); without this, every
     * printf after VFS/kmod was a full virtio present and looked hung. */
    fb_set_defer_flush(true);

    klog_info("Initializing storage subsystem\n");
    storage_init();
    storage_init_ramdisk();
    
    klog_info("Main loop started - ready for input\n");
    klog_info("Keyboard systems: PS/2 (interrupt-driven) + USB (polling) enabled\n");
    //for easy debug 
    mount_filesystem();
    fb_flush_pending();
    // CRITICAL FIX: Enable interrupts AFTER all boot printf calls are complete
    __asm__("sti");
    
    //PlayWAV(44100, fsize, file);
    //rtl8169_init();


    // Initialize kernel module subsystem
    klog_info("Initializing kernel module subsystem\n");
    kmod_init();
    fb_flush_pending();
    
    // Initialize SMP subsystem and bring up secondary CPUs
    klog_info("Initializing SMP subsystem\n");
    smp_init();
    smp_start_aps();
    
    // Create idle threads for all CPUs (enables SMP scheduling)
    smp_create_idle_threads();
    fb_flush_pending();

    // Initialize desktop compositor (window manager, not started yet)
    {
        extern void desktop_init(void);
        desktop_init();
    }

    // Enable preemptive thread scheduler (required for desktop + spawn)
    scheduler_start();

    // Initialize virtual console system (4 switchable consoles via Ctrl+Q)
    vconsole_init();

    // Initialize TTY abstraction layer
    tty_init();
    tty_set_line_callback(tty_zeus_line_handler);
    
    klog_info("System ready.\n");
    kernel_start_zeus();

    printf("Virtual consoles: Ctrl+F1 desktop, Ctrl+F2-F4 ttys, Ctrl+Q cycle\n");
    vconsole_draw_status_bar();
    fb_flush_pending();
    zeus_process_command("netset ip 192.168.56.2 mask 255.255.255.0 gw 192.168.56.1 dns 1.1.1.1");
    // Auto-detect network: try DHCP first, fall back to QEMU user-net defaults
        // DHCP works with both tap (if dnsmasq is running) and QEMU user-mode networking.
        // If DHCP fails (no server), apply QEMU user-net defaults (10.0.2.x) as fallback.
       // printf("Configuring network (trying DHCP)...\n");
       // zeus_process_command("dhcp");
       // // Check if DHCP gave us an IP
       // {
       //     extern void net_get_ip_address(uint8_t *ip);
       //     uint8_t check_ip[4];
       //     net_get_ip_address(check_ip);
       //     if ((check_ip[0] | check_ip[1] | check_ip[2] | check_ip[3]) == 0) {
       //         printf("DHCP failed, applying QEMU user-net defaults...\n");
       //         zeus_process_command("ddhcp");
       //     }
       // }
    // Enable LAPIC timer as a reliable heartbeat (~1ms periodic).
    // This ensures the CPU wakes from HLT even if the HPET periodic timer
    // is not working correctly (common on Proxmox / KVM).
    {
        extern void apic_enable_timer(void);
        apic_enable_timer();
    }

    while(1) {
        static uint64_t loop_count = 0;
        loop_count++;
        
        // Track if we did any work this iteration (for CPU idling)
        bool did_work = false;
        
        uint64_t current_time = hpet_get_ticks();

        /* Paint console output held back by the coalescing window. */
        fb_flush_pending();

        /* Spawned Ring-3 apps present via SYS_DESKTOP_COMPOSITE. While one is
         * READY, yield instead of burning a CRITICAL quantum on the compositor
         * (flamegraph showed ~24% samples in kmain competing with Doom). */
        if (global_scheduler.scheduler_running &&
            scheduler_has_ready_user_threads()) {
            thread_yield();
            continue;
        }

    /* WM vs client fast-present:
     * desktop_wm_needs_composite() is true for multi-window stacking, drag,
     * menus, or dirty chrome. Only skip the full composite when a single
     * focused Ring-3 client can fast-present its own pixels. Always keep WM
     * input (poll_input) so focus/chrome/routing stay live.
     * Text TTYs (tty1-3) own the framebuffer — do not composite over them. */
    bool desktop_may_paint = desktop_is_running() && vconsole_graphics_active();
    bool app_fast_present = desktop_may_paint && !desktop_wm_needs_composite();

        /* USB HID mice update canonical cursor state in mouse.c; run before compositor
         * so desktop reads fresh PS/2+USB position each iteration. */
        {
            extern void usb_mouse_poll_all(void);
            usb_mouse_poll_all();
        }

        /* PS/2 mouse polled fallback. If IRQ12 is being delivered properly the
         * controller's AUX output buffer will already have been drained by the
         * ISR, and this becomes a near-no-op (one inb of port 0x64). If IRQ
         * delivery is broken, this keeps the cursor working. */
        /* PS/2 keyboard polled fallback. IRQ1 is the fast path (QEMU).
         * Hyper-V Gen 1 often leaves the scancode in the i8042 with IRQ1
         * not yet delivered; keyboard_poll drains non-AUX bytes. Must run
         * before mouse_poll so a mouse byte is not required to unblock. */
        keyboard_poll();

        mouse_poll();

        /* Drive the desktop compositor from the kernel loop. Ring-0
         * preemption does not yet switch kernel stacks, so the desktop
         * must not run as a timer-preempted kernel thread. */
        // Drain deferred SDL key events queued by the keyboard ISR
        if (keyboard_has_sdl_pending()) {
            keyboard_process_sdl_events();
            did_work = true;
        }

        if (desktop_may_paint && !app_fast_present) {
            desktop_composite_frame();
        } else if (desktop_may_paint) {
            desktop_poll_input();
        }

        // Virtual console switching (Ctrl+Q) — also polled from the timer
        // ISR so Ctrl+Q works while the shell is blocked in exec_file().
        {
            int prev_vc = vconsole_get_active();
            vconsole_poll_switch();
            if (vconsole_get_active() != prev_vc)
                did_work = true;
        }

        // Process keyboard echo safely from main thread context
        if (keyboard_has_echo_pending()) {
            keyboard_process_echo();
            did_work = true;
        }

        // Tab completion – runs in main thread context (safe for printf)
        if (keyboard_get_tab_requested()) {
            keyboard_clear_tab_requested();
            char *buf = keyboard_get_buffer();
            uint32_t blen = keyboard_get_buffer_length();
            if (blen > 0) {
                char comp[128];
                int rc = zeus_tab_complete(buf, blen, comp, sizeof(comp));
                if (rc > 0) {
                    /* Append completion suffix to keyboard buffer */
                    for (int ci = 0; ci < rc && blen < 254; ci++) {
                        buf[blen] = comp[ci];
                        blen++;
                        printf("%c", comp[ci]);
                    }
                    buf[blen] = '\0';
                    vconsole_get_by_id(vconsole_get_active())->kbd_len = blen;
                } else if (rc < 0) {
                    /* Multiple matches – show them (list owned by zeus.c) */
                    zeus_print_command_matches(buf, blen);
                    printf("%s%s", tty_get_prompt(), buf);
                }
            }
            did_work = true;
        }

        // Command history navigation: Up / Down arrows
        if (keyboard_get_arrow_up()) {
            keyboard_clear_arrow_up();
            const char *hist = zeus_history_prev();
            if (hist) {
                /* Erase current line on screen */
                uint32_t old_len = keyboard_get_buffer_length();
                for (uint32_t bi = 0; bi < old_len; bi++) printf("\b \b");
                /* Replace keyboard buffer and print new content */
                uint32_t hlen = strlen(hist);
                keyboard_set_buffer(hist, hlen);
                printf("%s", hist);
            }
            did_work = true;
        }
        if (keyboard_get_arrow_down()) {
            keyboard_clear_arrow_down();
            const char *hist = zeus_history_next();
            if (hist) {
                uint32_t old_len = keyboard_get_buffer_length();
                for (uint32_t bi = 0; bi < old_len; bi++) printf("\b \b");
                uint32_t hlen = strlen(hist);
                keyboard_set_buffer(hist, hlen);
                printf("%s", hist);
            }
            did_work = true;
        }

        /* Line editing: arrows, Home/End, Ctrl-U/W, mid-line insert reprint. */
        if (keyboard_get_arrow_left()) {
            keyboard_clear_arrow_left();
            uint32_t cur = keyboard_get_cursor();
            if (cur > 0) {
                keyboard_set_cursor(cur - 1);
                printf("\b");
            }
            did_work = true;
        }
        if (keyboard_get_arrow_right()) {
            keyboard_clear_arrow_right();
            char *buf = keyboard_get_buffer();
            uint32_t cur = keyboard_get_cursor();
            uint32_t len = keyboard_get_buffer_length();
            if (cur < len) {
                printf("%c", buf[cur]);
                keyboard_set_cursor(cur + 1);
            }
            did_work = true;
        }
        if (keyboard_get_home()) {
            keyboard_clear_home();
            uint32_t cur = keyboard_get_cursor();
            while (cur > 0) { printf("\b"); cur--; }
            keyboard_set_cursor(0);
            did_work = true;
        }
        if (keyboard_get_end()) {
            keyboard_clear_end();
            char *buf = keyboard_get_buffer();
            uint32_t cur = keyboard_get_cursor();
            uint32_t len = keyboard_get_buffer_length();
            if (cur < len) printf("%s", buf + cur);
            keyboard_set_cursor(len);
            did_work = true;
        }
        if (keyboard_get_kill_line()) {
            keyboard_clear_kill_line();
            uint32_t cur = keyboard_get_cursor();
            if (cur)
                keyboard_delete_range(0, cur);
            did_work = true;
        }
        if (keyboard_get_kill_word()) {
            keyboard_clear_kill_word();
            char *buf = keyboard_get_buffer();
            uint32_t cur = keyboard_get_cursor();
            uint32_t start = cur;
            while (start > 0 && (buf[start - 1] == ' ' || buf[start - 1] == '\t'))
                start--;
            while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t')
                start--;
            if (start < cur)
                keyboard_delete_range(start, cur);
            did_work = true;
        }
        if (keyboard_take_dirty()) {
            const char *prompt = tty_get_prompt();
            char *buf = keyboard_get_buffer();
            uint32_t len = keyboard_get_buffer_length();
            uint32_t cur = keyboard_get_cursor();
            printf("\r%s%s ", prompt, buf);
            for (uint32_t i = 0; i < (len - cur) + 1; i++)
                printf("\b");
            did_work = true;
        }

        // Scrollback: PgUp / PgDn
        if (keyboard_get_pgup_pressed()) {
            keyboard_clear_pgup();
            extern void fb_console_scroll_back(void);
            fb_console_scroll_back();
            did_work = true;
        }
        if (keyboard_get_pgdn_pressed()) {
            keyboard_clear_pgdn();
            extern void fb_console_scroll_forward(void);
            fb_console_scroll_forward();
            did_work = true;
        }
        
        // Re-enable USB keyboard polling
        usb_keyboard_integrate_with_system();
        
        /* USB keyboard: feed characters through the same per-VC input
         * path as PS/2 so they share the buffer, echo, and line editing. */
        while (usb_keyboard_has_input()) {
            char c = usb_keyboard_get_char();
            if (c != 0) {
                keyboard_feed_char(c);
            }
        }
        
        // USB keyboard status removed - no longer needed
        
        // PS/2 keyboard polling removed - using interrupt-driven system

        /*
         * Zeus shell command processing via TTY abstraction.
         * The keyboard ISR fills the VC buffer; when Enter is pressed
         * (keyboard_get_execute), we submit the line through the TTY
         * layer which dispatches to Zeus via the registered callback.
         */
        if (keyboard_get_execute()) {
            char *buffer = keyboard_get_buffer();
            int active_tty = vconsole_get_active();

            tty_feed_line(active_tty, buffer);
            tty_submit_line(active_tty);
            /* The tty has its own copy now. Drop the line from the VC buffer
             * before running it, or a program reading stdin (busybox sh)
             * gets the Zeus command line again as its first input. */
            keyboard_clear_buffer();

            set_terminate_flag(false);
            tty_poll();
            keyboard_clear_buffer();
            set_terminate_flag(false);
            printf("%s", tty_get_prompt());
            did_work = true;
        }
        
        // Check if Ctrl+C was pressed (shows ^C and needs new prompt)
        static bool last_terminate_state = false;
        bool current_terminate = get_terminate_flag();
        if (current_terminate && !last_terminate_state) {
            // Ctrl+C was just pressed - show new prompt
            printf("%s", tty_get_prompt());
            set_terminate_flag(false);
        }
        last_terminate_state = current_terminate;
        
        // Poll AC97 audio driver for buffer management
        if (ac97_is_playing()) {
            ac97_poll();
            did_work = true;
        }
        
        // IRQ-gated network RX: NIC ISRs set a flag (and wake the CPU from
        // HLT); net_rx_service() drains a batch only when work is pending,
        // with a slow fallback poll for missed/misrouted IRQs.
        if (net_rx_service() > 0)
            did_work = true;

        // Service the telnet server (flush output, detect closed sessions)
        telnet_server_poll();

        // TCP retransmission timer: check for timed-out segments (~200ms)
        static uint64_t last_tcp_retransmit = 0;
        if (current_time - last_tcp_retransmit > 200) {
            tcp_retransmit_check();
            last_tcp_retransmit = current_time;
        }

        // Clean up stale TCP connections periodically to prevent memory leaks
        static uint64_t last_tcp_cleanup = 0;
        if (current_time - last_tcp_cleanup > 10000) {  // Every 10 seconds
            tcp_cleanup_stale_connections();
            last_tcp_cleanup = current_time;
        }

        // Network polling is now handled by the aggressive polling loop above
        // No need for periodic testing anymore

        // INTERRUPT-DRIVEN OPTIMIZATION: CPU halts when idle, wakes on network interrupts
        // This gives us BOTH low power consumption AND instant network response
        // Network packets trigger hardware interrupts which wake the CPU immediately
        if (!did_work) {
            __asm__ volatile("sti; hlt");
        }
    }

}

// Zeus Shell kernel integration functions

// Global Zeus shell state
static bool zeus_initialized = false;

// Initialize and start Zeus shell
void kernel_start_zeus(void) {
    if (zeus_initialized) {
        printf("Zeus shell already running\n");
        return;
    }

    printf("Initializing Zeus Shell...\n");

    // Initialize Zeus shell state
    zeus_init_shell();

    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("Mounting filesystem for Zeus...\n");
        mount_filesystem();
    }

    zeus_initialized = true;

    printf("Zeus Shell initialized and ready\n");
    printf("Type 'help' for available commands\n");
}

// Process command through Zeus shell
int zeus_process_command(const char* command_line) {
    if (!zeus_initialized) {
        printf("Zeus shell not initialized\n");
        return -1;
    }

    // Use the Zeus shell service to process the command
    return zeus_process_command_service(command_line);
}

// Initialize Zeus shell state
void zeus_init_shell(void) {
    extern void zeus_init_service(void);
    zeus_init_service();
}

// Clean shutdown of Zeus shell
void zeus_shutdown_shell(void) {
    if (zeus_initialized) {
        printf("Shutting down Zeus Shell...\n");
        zeus_initialized = false;
    }
}