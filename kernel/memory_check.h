#ifndef __MEMORY_CHECK_H__
#define __MEMORY_CHECK_H__

#include <setup.h>

// ============================================================
// MEMORY LAYOUT VALIDATION
// ============================================================

// AHCI section
#define AHCI_SIZE           (__PHT_MEM_AHCI_END - __PHT_MEM_AHCI_BASE)

// Malloc section
#define MALLOC_TABLE_SIZE   0x10000  // 64KB for table/reserved
#define MALLOC_POOL_SIZE    (__PHT_MEM_MALLOC_END - __PHT_MEM_MALLOC_POOL)  // 7 MB!

// File handles section - MINIMIZED
#define HANDLES_SIZE        (__PHT_MEM_HANDLES_END - __PHT_MEM_HANDLES)  // 64 KB only

// User space section
#define USERSPACE_SIZE      (__PHT_MEM_END - __PHT_MEM_USERSPACE)

// Validation checks
#if (__PHT_MEM_AHCI_END != __PHT_MEM_MALLOC_TABLE)
    #error "MEMORY LAYOUT ERROR: AHCI_END does not match MALLOC_TABLE start!"
#endif

#if (__PHT_MEM_MALLOC_END != __PHT_MEM_HANDLES)
    #error "MEMORY LAYOUT ERROR: MALLOC_END does not match HANDLES start!"
#endif

#if (__PHT_MEM_HANDLES_END != __PHT_MEM_USERSPACE)
    #error "MEMORY LAYOUT ERROR: HANDLES_END does not match USERSPACE start!"
#endif

#if (__PHT_MEM_USERSPACE + USERSPACE_SIZE != __PHT_MEM_END)
    #error "MEMORY LAYOUT ERROR: USERSPACE does not fit in memory!"
#endif

// Print friendly sizes (in bytes)
// Usage in code:
//   kprintf(font, 0x07, 2, "Malloc pool: %d KB\n", MALLOC_POOL_SIZE / 1024);

#endif // __MEMORY_CHECK_H__
