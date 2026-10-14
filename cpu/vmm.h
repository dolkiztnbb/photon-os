#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// Карта виртуальной памяти PEX-процессов
#define PEX_VIRT_BASE   0x04000000 // 64 MB — .text / .data / .bss
#define PEX_STACK_BASE  0x08000000 // 128 MB — Виртуальный стек процесса (растёт вниз)
#define PEX_HEAP_BASE   0x0A000000 // 160 MB — Виртуальная куча процесса

typedef struct {
    uint32_t cr3_phys;  // Физический адрес Page Directory (значение для CR3)
    uint32_t* pd_virt;  // Виртуальный указатель на Page Directory (для редактирования ядром)
} process_space_t;

// Создаёт индивидуальную Page Directory для процесса с маппингом ядра
process_space_t vmm_create_process_space(void);

// Отображает физическую страницу на виртуальный адрес в указанной Page Directory
void vmm_map_page(uint32_t* proc_pd, uintptr_t virt_addr, uintptr_t phys_addr, uint32_t flags);

// Освобождает таблицы страниц и физические страницы процесса
void vmm_destroy_process_space(process_space_t* proc, uint32_t text_size, uint32_t stack_size, uint32_t heap_size);

#endif