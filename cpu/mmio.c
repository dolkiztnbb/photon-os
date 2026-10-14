#include "mmio.h"
#include <kernel/debug.h>

extern char* crash_text;

#define MAX_MMIO_REGIONS 16
#define PAGE_SIZE 0x1000
#define PAGE_MASK ~(PAGE_SIZE - 1)

typedef struct {
    uintptr_t start_page;
    uintptr_t end_page;
    uint8_t enabled;
    const char* name;
} mmio_region_t;

static mmio_region_t regions[MAX_MMIO_REGIONS];
static int region_count = 0;

static uintptr_t last_fault_page = 0;
static uint8_t in_handler = 0;

#define PAGE_SIZE 4096
#define ENTRIES_PER_PT 1024

// Выравниваем структуры по границе 4 КБ
#define PAGE_SIZE 4096

// Мапируем первые 32 МБ памяти (8 таблиц по 1024 записи = 8192 страниц):
// 0..16 МБ — ядро, AHCI, kmalloc; 16..20 МБ — копия загрузочного образа
// (__PHT_MEM_BOOTDISK, нужна при загрузке с CD/USB), остальное — запас.
#define IDENTITY_MAP_TABLES 8
__attribute__((aligned(PAGE_SIZE))) uint32_t page_directory[1024];
__attribute__((aligned(PAGE_SIZE))) uint32_t page_tables[IDENTITY_MAP_TABLES][1024];

void init_paging(void) {
    // 1. Очищаем Page Directory
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002; // Not Present, Read/Write
    }

    // 2. Заполняем таблицы страниц (Identity Map: 0x00000000 - 0x01FFFFFF)
    for (int t = 0; t < IDENTITY_MAP_TABLES; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t phys_addr = (t * 1024 + i) * PAGE_SIZE;
            page_tables[t][i] = phys_addr | 3; // Present | Writable
        }
        // Подключаем таблицу к директории
        page_directory[t] = ((uint32_t)page_tables[t]) | 3;
    }

    // 3. Загружаем Page Directory в CR3
    asm volatile("mov %0, %%cr3" :: "r"(page_directory));

    // 4. Включаем Paging (PG bit 31) + Включаем Write Protect (WP bit 16)
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000; // Bit 31: PG (Paging), Bit 16: WP (Write Protect for Ring 0)
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}

// Заглушка обхода CR3, если у вас Identity Mapping (виртуальный = физический)
uint32_t* get_pte_pointer(uintptr_t virt_addr) {
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    
    // ВАЖНО: Если CR3 ссылается на физический адрес, убедитесь, 
    // что под VMM считывается корректный виртуальный указатель.
    uint32_t* pd = (uint32_t*)cr3;
    if (!pd) return 0;

    uint32_t pd_index = (virt_addr >> 22) & 0x3FF;
    uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

    uint32_t pde = pd[pd_index];
    if (!(pde & 1)) return 0;

    uint32_t* pt = (uint32_t*)(pde & PAGE_MASK);
    return &pt[pt_index];
}

static inline void invlpg(uintptr_t addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static void set_page_writable(uintptr_t page_addr, uint8_t writable) {
    uint32_t* pte = get_pte_pointer(page_addr);
    if (!pte) {
        kprintd("[MMIO ERROR] PTE not found for addr 0x%X!\n", page_addr);
        return;
    }
    
    if (!(*pte & 1)) {
        kprintd("[MMIO ERROR] Page at 0x%X is NOT PRESENT in PT!\n", page_addr);
        return;
    }

    if (writable) *pte |= (1 << 1);
    else          *pte &= ~(1 << 1);

    invlpg(page_addr);
}

void mmio_page_fault_handler(const registers_t *r) {
    uintptr_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint8_t is_write = (r->err_code & 0x02) != 0;

    if (in_handler) return;

    if (is_write) {
        for (int i = 0; i < region_count; i++) {
            if (regions[i].enabled && 
                fault_addr >= regions[i].start_page && 
                fault_addr < regions[i].end_page) 
            {
                in_handler = 1;

                // 1. Временно разрешаем запись для этой 4K страницы
                last_fault_page = fault_addr & PAGE_MASK;
                set_page_writable(last_fault_page, 1);

                // 2. Печатаем дамп перехвата
                kprinte("[MMIO:%s] TRAP WRITE to 0x%X from EIP=0x%X\n", 
                        regions[i].name, fault_addr, r->eip);

                // 3. Ставим Trap Flag (Single-Step) для #DB
                uint32_t *eflags = (uint32_t*)&r->eflags;
                *eflags |= (1 << 8);

                in_handler = 0;
                return;
            }
        }
    }
    uintptr_t fault_address;
    

    // Считываем адрес из регистра CR2
    asm volatile("mov %%cr2, %0" : "=r"(fault_address));

    kprinte("[#PF] Crash at EIP: 0x%08X trying to access ADDR: 0x%08X (Error Code: 0x%X)\n",
            r->eip, fault_address, r->err_code);
    // snkprintf(crash_text,"1234");
    
    dfh(r);
}

void mmio_debug_handler(const registers_t *r) {
    if (last_fault_page != 0) {
        // Возвращаем защиту Read-Only
        set_page_writable(last_fault_page, 0);
        last_fault_page = 0;

        // Выключаем Single-Step
        uint32_t *eflags = (uint32_t*)&r->eflags;
        *eflags &= ~(1 << 8);
    }
}

int mmio_add_region(const char* name, uintptr_t phys_addr, uint32_t size) {
    if (region_count >= MAX_MMIO_REGIONS) return -1;

    int id = region_count++;
    regions[id].name = name;
    regions[id].start_page = phys_addr & PAGE_MASK;
    regions[id].end_page = (phys_addr + size + PAGE_SIZE - 1) & PAGE_MASK;
    regions[id].enabled = 1;

    // Снимаем права на запись со всех страниц региона
    for (uintptr_t page = regions[id].start_page; page < regions[id].end_page; page += PAGE_SIZE) {
        set_page_writable(page, 0);
    }

    return id;
}