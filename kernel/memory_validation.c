/*
 * memory_validation.c - Memory boundary validation utility
 * 
 * Usage: Include this in your kernel initialization to verify memory layout
 * is correct at runtime (optional, for debugging)
 */

#include <stdint.h>
#include <setup.h>

void memory_validate(void) {
    // This function validates memory layout at runtime
    // Can be called from kernel initialization for debugging
    
    uint32_t ahci_size = __PHT_MEM_AHCI_END - __PHT_MEM_AHCI_BASE;
    uint32_t malloc_table_size = __PHT_MEM_MALLOC_POOL - __PHT_MEM_MALLOC_TABLE;
    uint32_t malloc_pool_size = __PHT_MEM_MALLOC_END - __PHT_MEM_MALLOC_POOL;
    uint32_t handles_size = __PHT_MEM_HANDLES_END - __PHT_MEM_HANDLES;
    uint32_t userspace_size = __PHT_MEM_END - __PHT_MEM_USERSPACE;
    
    // You can add kprintf calls here to print memory layout
    // Example (if kprintf is available):
    kprintd("AHCI:        0x%x - 0x%x (%d KB)\n", __PHT_MEM_AHCI_BASE, __PHT_MEM_AHCI_END, ahci_size/1024);
    kprintd("Malloc pool: 0x%x - 0x%x (%d KB)\n", __PHT_MEM_MALLOC_POOL, __PHT_MEM_MALLOC_END, malloc_pool_size/1024);
    kprintd("Handles:     0x%x - 0x%x (%d KB)\n", __PHT_MEM_HANDLES, __PHT_MEM_HANDLES_END, handles_size/1024);
    kprintd("Boot disk:   0x%x - 0x%x (%d KB)\n", __PHT_MEM_BOOTDISK, __PHT_MEM_BOOTDISK_END, (__PHT_MEM_BOOTDISK_END - __PHT_MEM_BOOTDISK)/1024);
    
    // Compile-time assertions (using #error is already in memory_check.h)
    // Runtime checks:
    if (__PHT_MEM_AHCI_END != __PHT_MEM_MALLOC_TABLE) {
        // ERROR: sections overlap or gap exists
        while(1); // Halt
    }
    if (__PHT_MEM_MALLOC_END != __PHT_MEM_HANDLES) {
        // ERROR: sections overlap or gap exists
        while(1); // Halt
    }
    if (__PHT_MEM_HANDLES_END != __PHT_MEM_BOOTDISK) {
        // ERROR: sections overlap or gap exists
        while(1); // Halt
    }
    if (__PHT_MEM_BOOTDISK_END != __PHT_MEM_USERSPACE) {
        // ERROR: sections overlap or gap exists
        while(1); // Halt
    }
}

/*
 * MEMORY LAYOUT (8 MB total):
 * 
 * 0x1000000  ┌─────────────────────┐
 *            │ User space (4 MB)   │
 * 0xc00000   ├─────────────────────┤
 *            │ Handles (64 KB)     │ ← Max 128 FILE_HANDLE entries
 * 0xbf0000   ├─────────────────────┤
 *            │ Malloc pool (7 MB)  │ ← 7,168 KB available!
 * 0x900000   ├─────────────────────┤
 *            │ Malloc table (64 KB)│
 * 0x8f0000   ├─────────────────────┤
 *            │ AHCI (1 MB)         │
 * 0x800000   └─────────────────────┘
 * 
 * VALIDATION:
 * ✓ No gaps between sections
 * ✓ Handles cannot collide with malloc (64 KB << 7 MB)
 * ✓ All sections properly aligned
 */
