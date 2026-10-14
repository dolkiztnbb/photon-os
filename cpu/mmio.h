#ifndef MMIO_H
#define MMIO_H

#include <libc/stdint>
#include "isr.h"
#include "mt.h"
#include <cpu/malloc/kmalloc.h>

// Добавить регион для отслеживания
int mmio_add_region(const char* name, uintptr_t phys_addr, uint32_t size);

// Инициализация подсистемы
void mmio_init_subsystem(void);

void mmio_page_fault_handler(const registers_t *r);
void mmio_debug_handler(const registers_t *r);

// Функция обхода таблиц страниц (вы должны ее реализовать под ваше ядро)
uint32_t* get_pte_pointer(uintptr_t virt_addr);

#endif