#include "vmm.h"
#include <cpu/malloc/kmalloc.h>
#include <libc/mem.h>
#include <kernel/debug.h>
#include <kernel/pex/header.h>
#include <kernel/pex/loader.h>
#include <cpu/mt.h>

extern uint32_t page_directory[1024];
extern task_t* tasks;
extern int current_task_idx;
extern void shm_cleanup_process(uint32_t* proc_pd);

uintptr_t vmm_get_phys_mapping(uint32_t* proc_pd, uintptr_t virt_addr) {
    if (!proc_pd) return 0;

    uint32_t pd_idx = (virt_addr >> 22) & 0x3FF;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;

    if (!(proc_pd[pd_idx] & 1)) return 0;

    uint32_t* pt = (uint32_t*)(proc_pd[pd_idx] & ~0xFFF);
    if (!pt) return 0;

    if (!(pt[pt_idx] & 1)) return 0;

    return pt[pt_idx] & ~0xFFF;
}

void vmm_unmap_page(uint32_t* proc_pd, uintptr_t virt_addr) {
    uint32_t pd_idx = (virt_addr >> 22) & 0x3FF;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;

    if (proc_pd[pd_idx] & 1) {
        uint32_t* pt = (uint32_t*)(proc_pd[pd_idx] & ~0xFFF);
        pt[pt_idx] = 0;
        asm volatile("invlpg (%0)" ::"r" (virt_addr) : "memory");
    }
}

process_space_t vmm_create_process_space(void) {
    process_space_t space;

    space.pd_virt = (uint32_t*)kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
    if (!space.pd_virt) {
        space.cr3_phys = 0;
        return space;
    }

    for (int i = 0; i < 1024; i++) {
        space.pd_virt[i] = 0x00000002;
    }

    for (int i = 0; i < 1024; i++) {
        if (page_directory[i] & 0x01) {
            space.pd_virt[i] = page_directory[i];
        }
    }

    space.cr3_phys = (uint32_t)space.pd_virt;
    return space;
}

void vmm_destroy_process_space(process_space_t* proc, uint32_t text_size, uint32_t stack_size, uint32_t heap_size) {
    if (!proc || !proc->pd_virt) return;

    // 0. Автоматически отключаем забытые SHM-сегменты процесса (чтобы они не утекали при exit/kill)
    shm_cleanup_process(proc->pd_virt);

    // 1. Освобождаем страницы секции кода (.text, .data, .bss)
    uint32_t code_pages = (text_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < code_pages; i++) {
        uintptr_t virt = PEX_VIRT_BASE + (i * PAGE_SIZE);
        uintptr_t phys = vmm_get_phys_mapping(proc->pd_virt, virt);
        if (phys) {
            vmm_unmap_page(proc->pd_virt, virt);
            kfree((void*)phys);
        }
    }

    // 2. Освобождаем страницы виртуального стека
    uint32_t stack_pages = (stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < stack_pages; i++) {
        uintptr_t virt = PEX_STACK_BASE - ((i + 1) * PAGE_SIZE);
        uintptr_t phys = vmm_get_phys_mapping(proc->pd_virt, virt);
        if (phys) {
            vmm_unmap_page(proc->pd_virt, virt);
            kfree((void*)phys);
        }
    }

    // 3. Освобождаем страницы пользовательской кучи
    uint32_t heap_pages = (heap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < heap_pages; i++) {
        uintptr_t virt = PEX_HEAP_BASE + (i * PAGE_SIZE);
        uintptr_t phys = vmm_get_phys_mapping(proc->pd_virt, virt);
        if (phys) {
            vmm_unmap_page(proc->pd_virt, virt);
            kfree((void*)phys);
        }
    }

    // 4. Освобождаем таблицы страниц Пользователя (кроме унаследованных таблиц ядра)
    for (int i = 16; i < 768; i++) {
        if ((proc->pd_virt[i] & 0x01) && proc->pd_virt[i] != page_directory[i]) {
            uintptr_t pt_phys = proc->pd_virt[i] & ~0xFFF;
            kfree((void*)pt_phys);
            proc->pd_virt[i] = 0;
        }
    }

    // 5. Освобождаем сам Page Directory процесса
    kfree(proc->pd_virt);
    proc->pd_virt = NULL;
    proc->cr3_phys = 0;
}

void vmm_map_page(uint32_t* proc_pd, uintptr_t virt_addr, uintptr_t phys_addr, uint32_t flags) {
    uint32_t pd_idx = (virt_addr >> 22) & 0x3FF;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;

    uint32_t* pt;

    if (!(proc_pd[pd_idx] & 1)) {
        pt = (uint32_t*)kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
        if (!pt) return;
        memset(pt, 0, PAGE_SIZE);
        proc_pd[pd_idx] = ((uint32_t)pt & ~0xFFF) | 0x07;
    } else {
        pt = (uint32_t*)(proc_pd[pd_idx] & ~0xFFF);
    }

    pt[pt_idx] = (phys_addr & ~0xFFF) | flags;
}

int vmm_check_user_memory(uint32_t* proc_pd, uintptr_t virt_addr, size_t size, int write_intent) {
    uintptr_t start = virt_addr;
    uintptr_t end = virt_addr + size;

    if (end < start) return 0;

    for (uintptr_t page = start & ~0xFFF; page < end; page += PAGE_SIZE) {
        uint32_t pd_idx = (page >> 22) & 0x3FF;
        uint32_t pt_idx = (page >> 12) & 0x3FF;

        if ((proc_pd[pd_idx] & 0x05) != 0x05) return 0;

        uint32_t* pt = (uint32_t*)(proc_pd[pd_idx] & ~0xFFF);

        if ((pt[pt_idx] & 0x05) != 0x05) return 0;

        if (write_intent && !(pt[pt_idx] & 0x02)) return 0;
    }

    return 1;
}

int copy_from_user(void *kdest, const void *usrc, size_t n) {
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    if (!vmm_check_user_memory((uint32_t*)cr3, (uintptr_t)usrc, n, 0)) {
        return 0;
    }

    memcpy(kdest, usrc, n);
    return 1;
}

int copy_to_user(void *udest, const void *ksrc, size_t n) {
    uint32_t* current_pd = tasks[current_task_idx].proc_space.pd_virt;
    if (!vmm_check_user_memory(current_pd, (uintptr_t)udest, n, 1)) {
        return 0;
    }

    memcpy(udest, ksrc, n);
    return 1;
}

void vmm_identity_map_page(uint32_t* pd, uintptr_t phys_addr, uint32_t flags) {
    uint32_t pd_idx = (phys_addr >> 22) & 0x3FF;
    uint32_t pt_idx = (phys_addr >> 12) & 0x3FF;

    uint32_t* pt;
    if (!(pd[pd_idx] & 1)) {
        pt = (uint32_t*)kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
        if (!pt) return;
        memset(pt, 0, PAGE_SIZE);
        pd[pd_idx] = ((uint32_t)pt & ~0xFFF) | flags;
    } else {
        pt = (uint32_t*)(pd[pd_idx] & ~0xFFF);
    }
    pt[pt_idx] = (phys_addr & ~0xFFF) | flags;
}

int sys_map_hardware(uintptr_t phys_addr, uintptr_t virt_addr, uint32_t size) {
    task_t* cur = mt_get_current_task();

    if (!(cur->flags & (PEX_FLAG_ALLOW_IO | PEX_FLAG_RING0))) {
        return -1;
    }

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        vmm_map_page(cur->proc_space.pd_virt,
                     virt_addr + (i * PAGE_SIZE),
                     phys_addr + (i * PAGE_SIZE),
                     0x07);
    }
    return 0;
}
