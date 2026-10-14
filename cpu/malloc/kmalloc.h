#include <macros/macros.h> // This is terrible
#include <stdint.h>
EXTENDED_KERNEL void kminit();
EXTENDED_KERNEL void* kmalloc(uint32_t size);
EXTENDED_KERNEL void kfree(void* ptr);
EXTENDED_KERNEL uint8_t kmstatus(void* ptr);
EXTENDED_KERNEL void kcm();
void* kmalloc_aligned(uint32_t size, uint32_t alignment);
void* kmalloc_aligned_reserved(uint32_t size, uint32_t alignment);
