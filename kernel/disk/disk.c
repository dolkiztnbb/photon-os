/* -------------------------------------------------------------------------- */
/* 1. Disk I/O Implementation (ATA/IDE)                                       */
/* -------------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>

/* --- Your Existing Port Functions --- */
uint8_t pbi(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void pbo(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

void disk_wait() {
    while (1) {
        uint8_t status = pbi(0x1F7);
        if (!(status & 0x80)) {
            if (status & 0x01) {
                uint8_t error = pbi(0x1F1);
                asm volatile("hlt");
                return;
            }
            return;
        }
    }
}

/* --- New: Disk Read Function (Mirrors your disk_write) --- */
size_t disk_read(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t *buffer, uint16_t sector_size) {
    if (sector_size == 0) {
        return 0x00000001;
    }

    uint8_t lba_low = lba & 0xFF;
    uint8_t lba_mid = (lba >> 8) & 0xFF;
    uint8_t lba_high = (lba >> 16) & 0xFF;
    uint8_t drive_head = ((lba >> 24) & 0xF) | (drive << 4);

    disk_wait();

    /* Send read command parameters */
    pbo(0x1F2, sectors);        // Sector count
    pbo(0x1F3, lba_low);        // LBA Low
    pbo(0x1F4, lba_mid);        // LBA Mid
    pbo(0x1F5, lba_high);       // LBA High
    pbo(0x1F6, drive_head | 0xE0); // Drive & LBA mode
    pbo(0x1F7, 0x20);           // Command: READ SECTOR(S)

    /* Wait for disk to be ready to transfer data */
    disk_wait();

    /* Read data from data port (0x1F0) */
    /* ATA transfers data as 16-bit words */
    for (int i = 0; i < sectors; i++) {
        for (int j = 0; j < sector_size; j += 2) {
            uint16_t word = pbi(0x1F0);          // Low byte
            word |= (pbi(0x1F0) << 8);           // High byte
            
            /* Handle potential unaligned buffer pointer safely */
            buffer[i * sector_size + j] = word & 0xFF;
            buffer[i * sector_size + j + 1] = (word >> 8) & 0xFF;
        }
    }

    disk_wait();

    /* Check status for errors */
    uint8_t status = pbi(0x1F7);
    if (status & 0x01) {
        uint8_t error = pbi(0x1F1);
        return (status << 24 | error << 16) | 0x2;
    } else {
        return 0;
    }
}

/* -------------------------------------------------------------------------- */
/* 2. File System I/O Wrappers                                                */
/* -------------------------------------------------------------------------- */

/* Configuration */
#define MCFS_DRIVE_NUMBER   0       // Master on primary channel
#define MCFS_SECTOR_SIZE    512     // Standard sector size

/* 
 * Wrapper for File System: Read Sector
 * Matches: int read_sector(int s, char* target_data)
 */
int read_sector(int s, char* target_data) {
    /* Call low-level driver */
    size_t result = disk_read(
        MCFS_DRIVE_NUMBER,    // Drive 0 (Master)
        (uint32_t)s,          // LBA = Sector Number
        1,                    // Read 1 sector
        (uint8_t*)target_data,// Buffer
        MCFS_SECTOR_SIZE      // 512 bytes
    );

    /* Return 0 on success, 1 on error (matching FS expectations) */
    return (result == 0) ? 0 : 1;
}

/* 
 * Wrapper for File System: Write Sector
 * Matches: int write_sector(int s, char* data)
 */
int write_sector(int s, char* data) {
    /* Call low-level driver */
    size_t result = disk_write(
        MCFS_DRIVE_NUMBER,    // Drive 0 (Master)
        (uint32_t)s,          // LBA = Sector Number
        1,                    // Write 1 sector
        (uint8_t*)data,       // Buffer
        MCFS_SECTOR_SIZE      // 512 bytes
    );

    /* Return 0 on success, 1 on error */
    return (result == 0) ? 0 : 1;
}
