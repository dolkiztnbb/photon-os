#include "fs.h"
#include <drivers/blockdev.h>
#include <cpu/malloc/kmalloc.h>
#include <kernel/debug.h>

// Смещение файловой системы внутри образа ОС (в 512-байтных секторах).
// Makefile передаёт его через -DFS_START_SECTOR=$(IMG_FS_LBA); то же число
// загрузчик кладёт в bootinfo.fs_lba, и fs_load_from_disk() сверяет их.
// Где физически лежит образ (диск, CD, ramdisk) — решает drivers/blockdev.c.
#ifndef FS_START_SECTOR
#define FS_START_SECTOR      384
#endif
#define MAX_DISK_NODES       100
#define MAX_FILE_SIZE         (1024 * 1024 * 1024)

disk_dnode_t disk_nodes[MAX_DISK_NODES];
uint32_t node_counter = 0;
uint32_t current_data_sector = FS_START_SECTOR + 15;

// =========================================================================
// КРИТИЧЕСКИ ВАЖНО ДЛЯ AHCI DMA:
// Статический буфер в секции данных ядра (0 - 16 MB, где Virtual == Physical).
// Теперь AHCI запишет ровно то, что положил процессор, независимо от процесса!
// =========================================================================
__attribute__((aligned(512))) static char fs_dma_buf[512];

static void update_superblock_count(uint32_t count) {
    if (blk_read_sector(FS_START_SECTOR, fs_dma_buf) == 0) {
        disk_superblock_t *sb = (disk_superblock_t*)fs_dma_buf;
        sb->total_nodes = count;
        blk_write_sector(FS_START_SECTOR, fs_dma_buf);
    }
}

void fs_flush_node(Dnode* node) {
    if (!node) return;
    if (node->read != 0 || node->write != 0) {
        return; 
    }

    // A. Новый узел — выделяем ID
    if (node->id < 0) {
        if (node_counter >= MAX_DISK_NODES) return;
        node->id = node_counter++;
        update_superblock_count(node_counter);
    }

    // B. Запись данных файла
    if (node->type == TYPE_FILE && node->size > 0 && node->data != 0) {
        if (node->data_block == 0) {
            node->data_block = current_data_sector - FS_START_SECTOR;
            uint32_t sectors_needed = (node->size + 511) / 512;
            current_data_sector += sectors_needed;
        }

        uint32_t abs_data_sector = FS_START_SECTOR + node->data_block;
        uint32_t sectors_to_write = (node->size + 511) / 512;

        for (uint32_t i = 0; i < sectors_to_write; i++) {
            uint32_t offset = i * 512;
            uint32_t bytes_to_copy = (node->size - offset > 512) ? 512 : (node->size - offset);

            for (int b = 0; b < 512; b++) fs_dma_buf[b] = 0;
            for (uint32_t b = 0; b < bytes_to_copy; b++) {
                fs_dma_buf[b] = node->data[offset + b];
            }
            blk_write_sector(abs_data_sector + i, fs_dma_buf);
        }
    }

    // C. Запись метаданных (структуры disk_dnode_t)
    disk_dnode_t d_node;
    memset(&d_node, 0, sizeof(disk_dnode_t));
    d_node.type = node->type;
    d_node.size = node->size;
    d_node.change_time = node->change_time;
    d_node.permissions = node->permissions;
    d_node.parent_id = (node->parent && node->parent != node) ? node->parent->id : -1;
    d_node.data_block = node->data_block;
    
    // Копируем имя узла
    for (int k = 0; k < 31 && node->name[k]; k++) {
        d_node.name[k] = node->name[k];
    }

    uint32_t byte_offset = node->id * sizeof(disk_dnode_t);
    uint32_t target_sector = FS_START_SECTOR + 1 + (byte_offset / 512);
    uint32_t offset_in_sector = byte_offset % 512;

    // Читаем сектор, модифицируем нужные 64 байта и пишем обратно через DMA буфер ядра
    blk_read_sector(target_sector, fs_dma_buf);

    char* p = (char*)&d_node;
    for (uint32_t i = 0; i < sizeof(disk_dnode_t); i++) {
        fs_dma_buf[offset_in_sector + i] = p[i];
    }

    blk_write_sector(target_sector, fs_dma_buf);
}

void fs_delete_node(Dnode* node) {
    if (!node || node->id < 0 || node->id >= MAX_DISK_NODES) return;

    uint32_t byte_offset = node->id * sizeof(disk_dnode_t);
    uint32_t target_sector = FS_START_SECTOR + 1 + (byte_offset / 512);
    uint32_t offset_in_sector = byte_offset % 512;

    if (blk_read_sector(target_sector, fs_dma_buf) == 0) {
        for (uint32_t b = 0; b < sizeof(disk_dnode_t); b++) {
            fs_dma_buf[offset_in_sector + b] = 0;
        }
        blk_write_sector(target_sector, fs_dma_buf);
    }
}

void fs_load_from_disk(Dnode** node) {
    if (!node) return;
    *node = 0;

    const pht_bootinfo_t* bi = blockdev_bootinfo();
    if (bi && bi->fs_lba != FS_START_SECTOR) {
        kprintw("fs: image header says FS starts at sector %d, kernel built for %d\n",
                bi->fs_lba, FS_START_SECTOR);
    }

    if (blk_read_sector(FS_START_SECTOR, fs_dma_buf) != 0) {
        kprinte("Superblock read error\n");
        return;
    }

    disk_superblock_t *sb = (disk_superblock_t*)fs_dma_buf;
    if (sb->magic != 0xDA751AF5) {
        kprinte("Superblock wrong magic\n");
        return;
    }

    uint32_t total_nodes = sb->total_nodes;
    if (total_nodes == 0 || total_nodes > MAX_DISK_NODES) {
        kprinte("Invalid node count\n");
        return;
    }

    uint64_t nodes_bytes = (uint64_t)total_nodes * sizeof(disk_dnode_t);
    uint32_t node_sectors = (uint32_t)((nodes_bytes + 511) / 512);

    disk_dnode_t *loaded_disk_nodes = (disk_dnode_t*)kmalloc(node_sectors * 512);
    if (!loaded_disk_nodes) return;

    for (uint32_t i = 0; i < node_sectors; i++) {
        blk_read_sector(FS_START_SECTOR + 1 + i, ((char*)loaded_disk_nodes) + (i * 512));
    }

    Dnode** runtime_nodes = (Dnode**)kmalloc(total_nodes * sizeof(Dnode*));
    if (!runtime_nodes) {
        kfree(loaded_disk_nodes);
        return;
    }
    memset(runtime_nodes, 0, total_nodes * sizeof(Dnode*));

    uint32_t max_data_sec = FS_START_SECTOR + 15;

    for (uint32_t i = 0; i < total_nodes; i++) {
        disk_dnode_t *d = &loaded_disk_nodes[i];

        // Узел удален, если имя пустое (кроме самого корня с id=0)
        if (i != 0 && d->name[0] == '\0') {
            runtime_nodes[i] = NULL;
            continue;
        }

        char safe_name[33];
        for (int k = 0; k < 32; k++) safe_name[k] = d->name[k];
        safe_name[32] = '\0';


        Dnode *r = create_node(safe_name, d->type);
        if (!r) continue;

        r->id = i;
        r->size = d->size;
        r->change_time = d->change_time;
        r->permissions = d->permissions;
        r->data_block = d->data_block;

        // Считываем данные файлов
        if (d->type == TYPE_FILE && d->data_block > 0) {
            uint32_t sectors_to_read = (d->size + 511) / 512;
            uint32_t end_sec = FS_START_SECTOR + d->data_block + sectors_to_read;
            if (end_sec > max_data_sec) max_data_sec = end_sec;

            r->data = (char*)kmalloc(sectors_to_read * 512 + 1);
            if (r->data) {
                for (uint32_t s = 0; s < sectors_to_read; s++) {
                    blk_read_sector(FS_START_SECTOR + d->data_block + s, r->data + (s * 512));
                }
                r->data[d->size] = '\0';
            }
        }

        runtime_nodes[i] = r;
    }

    node_counter = total_nodes;
    current_data_sector = max_data_sec;

    // Восстанавливаем дерево каталогов (с защитой от NULL)
    Dnode* root = NULL;
    for (uint32_t i = 0; i < total_nodes; i++) {
        Dnode *r = runtime_nodes[i];
        if (!r) continue;

        disk_dnode_t *d = &loaded_disk_nodes[i];

        if ((int32_t)d->parent_id == -1 || i == 0) {
            root = r;
            root->parent = root;
        } else if (d->parent_id < (int32_t)total_nodes && d->parent_id >= 0 && d->parent_id != (int32_t)i) {
            Dnode *parent = runtime_nodes[d->parent_id];
            if (parent) {
                dnode_add_child(parent, r);
            }
        }
    }

    *node = root;

    kfree(loaded_disk_nodes);
    kfree(runtime_nodes);
}