#include "shm.h"
#include <cpu/vmm.h>
#include <cpu/mt.h>
#include <cpu/malloc/kmalloc.h>
#include <libc/mem.h>

static shm_region_t shm_regions[MAX_SHM_REGIONS];

int32_t sys_shm_create(uint32_t size) {
    if (size == 0) return -1;
    task_t* cur = mt_get_current_task();
    if (!cur) return -1;

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > MAX_SHM_PAGES) return -1;

    // Ищем свободный слот
    int id = -1;
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (!shm_regions[i].active) {
            id = i;
            break;
        }
    }
    if (id == -1) return -1;

    shm_region_t* reg = &shm_regions[id];
    reg->num_pages = pages;
    reg->size = size;

    // Выделяем физические страницы в памяти ядра
    for (uint32_t i = 0; i < pages; i++) {
        void* phys = kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
        if (!phys) {
            for (uint32_t k = 0; k < i; k++) kfree((void*)reg->phys_pages[k]);
            return -1;
        }
        memset(phys, 0, PAGE_SIZE);
        reg->phys_pages[i] = (uintptr_t)phys;
    }

    reg->active = 1;
    reg->ref_count = 0;
    reg->creator_pid = cur->info.pid;
    return id;
}

void* sys_shm_attach(int32_t shmid) {
    if (shmid < 0 || shmid >= MAX_SHM_REGIONS) return NULL;
    shm_region_t* reg = &shm_regions[shmid];
    if (!reg->active) return NULL;

    task_t* cur = mt_get_current_task();
    if (!cur || !cur->proc_space.pd_virt) return NULL;

    uintptr_t virt_base = PEX_SHM_BASE + ((uintptr_t)shmid * (MAX_SHM_PAGES * PAGE_SIZE));

    // Если этот процесс уже подключил данный сегмент — не увеличиваем ref_count повторно
    if (vmm_get_phys_mapping(cur->proc_space.pd_virt, virt_base) == reg->phys_pages[0]) {
        return (void*)virt_base;
    }

    for (uint32_t i = 0; i < reg->num_pages; i++) {
        uintptr_t virt = virt_base + (i * PAGE_SIZE);
        vmm_map_page(cur->proc_space.pd_virt, virt, reg->phys_pages[i], 0x07);
        asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    reg->ref_count++;
    return (void*)virt_base;
}

int32_t sys_shm_detach(int32_t shmid) {
    if (shmid < 0 || shmid >= MAX_SHM_REGIONS) return -1;
    shm_region_t* reg = &shm_regions[shmid];
    if (!reg->active) return -1;

    task_t* cur = mt_get_current_task();
    if (!cur || !cur->proc_space.pd_virt) return -1;

    uintptr_t virt_base = PEX_SHM_BASE + ((uintptr_t)shmid * (MAX_SHM_PAGES * PAGE_SIZE));

    // Проверяем, действительно ли сегмент подключён в адресное пространство этого процесса
    if (vmm_get_phys_mapping(cur->proc_space.pd_virt, virt_base) == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < reg->num_pages; i++) {
        vmm_unmap_page(cur->proc_space.pd_virt, virt_base + (i * PAGE_SIZE));
    }

    reg->ref_count--;
    if (reg->ref_count <= 0) {
        for (uint32_t i = 0; i < reg->num_pages; i++) {
            kfree((void*)reg->phys_pages[i]);
        }
        reg->active = 0;
    }
    return 0;
}

// Автоматически отключает и освобождает все SHM-сегменты процесса при его завершении / kill
void shm_cleanup_process(uint32_t* proc_pd) {
    if (!proc_pd) return;
    for (int shmid = 0; shmid < MAX_SHM_REGIONS; shmid++) {
        shm_region_t* reg = &shm_regions[shmid];
        if (!reg->active || reg->num_pages == 0) continue;

        uintptr_t virt_base = PEX_SHM_BASE + ((uintptr_t)shmid * (MAX_SHM_PAGES * PAGE_SIZE));
        if (vmm_get_phys_mapping(proc_pd, virt_base) == reg->phys_pages[0]) {
            for (uint32_t i = 0; i < reg->num_pages; i++) {
                vmm_unmap_page(proc_pd, virt_base + (i * PAGE_SIZE));
            }
            reg->ref_count--;
            if (reg->ref_count <= 0) {
                for (uint32_t i = 0; i < reg->num_pages; i++) {
                    kfree((void*)reg->phys_pages[i]);
                }
                reg->active = 0;
            }
        }
    }
}
