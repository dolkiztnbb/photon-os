#ifndef SHM_H
#define SHM_H

#include <stdint.h>
#include <stddef.h>

#define MAX_SHM_REGIONS 32
#define MAX_SHM_PAGES   256       // До 1 МБ на окно (640x480 в 1 байт = 300 КБ)
#define PEX_SHM_BASE    0x20000000 // Базовый виртуальный адрес (512 МБ)

typedef struct {
    uint8_t   active;
    uint32_t  size;
    uint32_t  num_pages;
    uintptr_t phys_pages[MAX_SHM_PAGES];
    int       ref_count;
    int       creator_pid;
} shm_region_t;

int32_t sys_shm_create(uint32_t size);
void*   sys_shm_attach(int32_t shmid);
int32_t sys_shm_detach(int32_t shmid);

#endif