#pragma once
#include <stdint.h>
#include <setup.h>

#define MAX_ALLOCATIONS 4096
#define MEMORY_POOL_SIZE (__PHT_MEM_MALLOC_END - __PHT_MEM_MALLOC_POOL)
#define NULL ((void*)0)

typedef struct {
    uint32_t addr;    
    uint32_t size;    
    char     used;    
} MallocTable;

uint8_t      haveBeenCalled      = 0;

static MallocTable table_reserved_storage[MAX_ALLOCATIONS];
MallocTable* table               = (MallocTable*) __PHT_MEM_MALLOC_TABLE;
MallocTable* table_reserved      = table_reserved_storage;
uint8_t*     memory_pool         = (uint8_t*)    __PHT_MEM_MALLOC_POOL;

uint32_t     allocated_amount    = 0;

// Верхние индексы активных записей, чтобы не сканировать вхолостую все 4096 слотов
static int   table_high_water          = 0;
static int   table_reserved_high_water = 0;

static inline uint32_t irq_save(void) {
    uint32_t eflags;
    asm volatile("pushfl; popl %0; cli" : "=r"(eflags) : : "memory");
    return eflags;
}

static inline void irq_restore(uint32_t eflags) {
    asm volatile("pushl %0; popfl" : : "r"(eflags) : "memory");
}

void kminit(void) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        table[i].used = 0;
        table[i].addr = 0;
        table[i].size = 0;
        table_reserved[i].used = 0;
        table_reserved[i].addr = 0;
        table_reserved[i].size = 0;
    }
    allocated_amount = 0;
    table_high_water = 0;
    table_reserved_high_water = 0;
}

// Поиск первого подходящего свободного непрерывного диапазона памяти (First-Fit)
static uint32_t find_free_region(uint32_t size, uint32_t alignment) {
    uint32_t pool_start = (uint32_t)memory_pool;
    uint32_t pool_end   = pool_start + MEMORY_POOL_SIZE;

    // Начальный кандидат с учетом выравнивания
    uint32_t cand = (pool_start + (alignment - 1)) & ~(alignment - 1);

    while (cand + size <= pool_end) {
        int conflict = 0;
        uint32_t next_cand = 0;

        // Проверяем коллизии с обычными блоками (только до table_high_water)
        for (int j = 0; j < table_high_water; j++) {
            if (table[j].used) {
                uint32_t b_start = table[j].addr;
                uint32_t b_end   = b_start + table[j].size;
                if (!(cand + size <= b_start || cand >= b_end)) {
                    conflict = 1;
                    if (b_end > next_cand) next_cand = b_end;
                }
            }
        }

        // Проверяем коллизии с зарезервированными блоками (страницы VMM)
        for (int j = 0; j < table_reserved_high_water; j++) {
            if (table_reserved[j].used) {
                uint32_t b_start = table_reserved[j].addr;
                uint32_t b_end   = b_start + table_reserved[j].size;
                if (!(cand + size <= b_start || cand >= b_end)) {
                    conflict = 1;
                    if (b_end > next_cand) next_cand = b_end;
                }
            }
        }

        // Если коллизий нет — адрес найден!
        if (!conflict) {
            return cand;
        }

        // Сдвигаем кандидата за пределы конфликтующего блока и выравниваем
        cand = (next_cand + (alignment - 1)) & ~(alignment - 1);
    }

    return 0; // Память исчерпана
}

void* kmalloc(uint32_t size) {
    if (size == 0) return NULL;

    uint32_t eflags = irq_save();

    if (haveBeenCalled == 0) {
        haveBeenCalled = 1;
        kminit();
    }

    // Выравниваем размер до 4 байт
    size = (size + 3) & ~3;

    // Ищем свободный слот в таблице
    int free_slot = -1;
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (table[i].used == 0) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1) {
        irq_restore(eflags);
        return NULL;
    }

    uint32_t addr = find_free_region(size, 4);
    if (addr == 0) {
        irq_restore(eflags);
        return NULL;
    }

    table[free_slot].used = 1;
    table[free_slot].addr = addr;
    table[free_slot].size = size;
    if (free_slot >= table_high_water) {
        table_high_water = free_slot + 1;
    }
    allocated_amount += size;

    irq_restore(eflags);
    return (void*)addr;
}

void* kmalloc_aligned_reserved(uint32_t size, uint32_t alignment) {
    if (size == 0) return NULL;
    if (alignment < 4) alignment = 4;

    uint32_t eflags = irq_save();

    if (haveBeenCalled == 0) {
        haveBeenCalled = 1;
        kminit();
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (table_reserved[i].used == 0) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1) {
        irq_restore(eflags);
        return NULL;
    }

    uint32_t addr = find_free_region(size, alignment);
    if (addr == 0) {
        irq_restore(eflags);
        return NULL;
    }

    table_reserved[free_slot].used = 1;
    table_reserved[free_slot].addr = addr;
    table_reserved[free_slot].size = size;
    if (free_slot >= table_reserved_high_water) {
        table_reserved_high_water = free_slot + 1;
    }
    allocated_amount += size;

    irq_restore(eflags);
    return (void*)addr;
}

void* kmalloc_aligned(uint32_t size, uint32_t alignment) {
    return kmalloc_aligned_reserved(size, alignment);
}

void kfree(void* ptr) {
    if (ptr == NULL) return;

    uint32_t eflags = irq_save();
    uint32_t addr_to_free = (uint32_t)ptr;

    // Сначала ищем в обычной таблице
    for (int i = 0; i < table_high_water; i++) {
        if (table[i].used && table[i].addr == addr_to_free) {
            allocated_amount -= table[i].size;
            table[i].used = 0;
            table[i].addr = 0;
            table[i].size = 0;
            while (table_high_water > 0 && !table[table_high_water - 1].used) {
                table_high_water--;
            }
            irq_restore(eflags);
            return;
        }
    }

    // Затем ищем в зарезервированной таблице
    for (int i = 0; i < table_reserved_high_water; i++) {
        if (table_reserved[i].used && table_reserved[i].addr == addr_to_free) {
            allocated_amount -= table_reserved[i].size;
            table_reserved[i].used = 0;
            table_reserved[i].addr = 0;
            table_reserved[i].size = 0;
            while (table_reserved_high_water > 0 && !table_reserved[table_reserved_high_water - 1].used) {
                table_reserved_high_water--;
            }
            irq_restore(eflags);
            return;
        }
    }

    irq_restore(eflags);
}

void kfree_reserved(void* ptr) {
    kfree(ptr);
}

void kcm(void) {
    // В First-Fit дефрагментация не требуется
}
