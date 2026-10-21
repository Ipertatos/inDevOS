/**
 * @file kernel_config.h
 * @brief Kernel configuration constants and limits
 *
 * This file centralizes magic numbers and configuration constants
 * used throughout the kernel for easier maintenance and documentation.
 */

#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

#include <stdint.h>

/*============================================================================
 * Memory Configuration
 *============================================================================*/

/** Page size in bytes (4KB on x86-64) */
#define PAGE_SIZE                   4096

/** Higher-half virtual memory offset (standard for x86-64 kernels) */
#define HIGHER_HALF_OFFSET          0xFFFF800000000000ULL

/** Alternative kernel mapping offset (used by some bootloaders) */
#define KERNEL_MAP_OFFSET           0xFFFFFFFF80000000ULL

/** DMA low memory threshold - RTL8139 and legacy devices need memory below this */
#define DMA_LOW_MEMORY_THRESHOLD    (16 * 1024 * 1024)  /* 16MB */

/** Minimum address for high memory allocations (used by kalloc_high) */
#define HIGH_MEMORY_START           0x100000  /* 1MB */

/** 32-bit DMA address limit (for devices that can't access above 4GB) */
#define DMA_32BIT_LIMIT             0x100000000ULL  /* 4GB */

/*============================================================================
 * Syscall Configuration
 *============================================================================*/

/** Maximum size for a single read syscall (prevents DoS) */
#define SYSCALL_MAX_READ_SIZE       (1 * 1024 * 1024)   /* 1MB */

/** Maximum size for a single write syscall (prevents DoS) */
#define SYSCALL_MAX_WRITE_SIZE      (1 * 1024 * 1024)   /* 1MB */

/** Maximum open files per process */
#define MAX_OPEN_FILES_PER_PROCESS  64

/*============================================================================
 * Network Configuration
 *============================================================================*/

/** Maximum TCP connections */
#define TCP_MAX_CONNECTIONS         16

/** Default TCP buffer size */
#define TCP_DEFAULT_BUFFER_SIZE     4096

/** TCP TIME_WAIT timeout in milliseconds */
#define TCP_TIME_WAIT_TIMEOUT_MS    60000   /* 60 seconds */

/** TCP stale connection timeout in milliseconds */
#define TCP_STALE_TIMEOUT_MS        120000  /* 2 minutes */

/** Maximum size of a network packet */
#define NET_MAX_PACKET_SIZE         1518    /* Ethernet MTU + headers */

/** RTL8139 receive buffer size */
#define RTL8139_RX_BUFFER_SIZE      (8192 + 16 + 1500)

/*============================================================================
 * Process and Thread Configuration
 *============================================================================*/

/** Maximum number of processes */
#define MAX_PROCESSES               128

/** Maximum number of threads */
#define MAX_THREADS                 256

/** Default thread stack size */
#define DEFAULT_THREAD_STACK_SIZE   (16 * 1024)  /* 16KB */

/** Kernel stack size */
#define KERNEL_STACK_SIZE           (8 * 1024)   /* 8KB */

/*============================================================================
 * Filesystem Configuration
 *============================================================================*/

/** Maximum open files (global) */
#define MAX_OPEN_FILES              64

/** Maximum path length */
#define MAX_PATH_LENGTH             256

/** Maximum filename length */
#define MAX_FILENAME_LENGTH         256

/*============================================================================
 * Timer Configuration
 *============================================================================*/

/** HPET timer frequency target (Hz) */
#define HPET_TARGET_FREQUENCY       1000  /* 1ms resolution */

/** Network task interval in milliseconds */
#define NETWORK_TASK_INTERVAL_MS    5000  /* 5 seconds */

/*============================================================================
 * Debug Configuration
 *============================================================================*/

/** Maximum stack trace depth */
#define MAX_STACK_TRACE_DEPTH       16

/** Debug buffer size */
#define DEBUG_BUFFER_SIZE           256

/*============================================================================
 * USB Configuration
 *============================================================================*/

/** Maximum USB devices */
#define USB_MAX_DEVICES             16

/** Maximum USB endpoints per device */
#define USB_MAX_ENDPOINTS           32

#endif /* KERNEL_CONFIG_H */




