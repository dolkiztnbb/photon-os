/* 
 * MCFS (Mini Custom File System) Kernel Driver
 * Compatible with the provided Python minfs.py structure.
 * 
 * Constraints: No stdlib, freestanding environment.
 */
#include <stdint.h>
/* -------------------------------------------------------------------------- */
/* 1. Basic Type Definitions (Replacing stdint.h)                             */
/* -------------------------------------------------------------------------- */
#define NULL			    ((void*)0)
// typedef unsigned char       uint8_t;
// typedef signed char         int8_t;
// typedef unsigned short      uint16_t;
// typedef signed short        int16_t;
// typedef unsigned int        uint32_t;
// typedef signed int          int32_t;
// typedef unsigned long long  uint64_t;
// typedef signed long long    int64_t;

/* -------------------------------------------------------------------------- */
/* 2. Memory Utilities (Replacing string.h / stdlib.h)                        */
/* -------------------------------------------------------------------------- */

static void mcfs_memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static void mcfs_memset(void *dest, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    while (n--) {
        *d++ = val;
    }
}

static int mcfs_memcmp(const void *s1, const void *s2, uint32_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

static uint32_t mcfs_strlen(const char *str) {
    const char *s = str;
    while (*s) {
        s++;
    }
    return (uint32_t)(s - str);
}

/* -------------------------------------------------------------------------- */
/* 3. External I/O Interface (Provided by Kernel)                             */
/* -------------------------------------------------------------------------- */
/* 
 * These must be implemented by your kernel. 
 * We declare them as extern to link them later.
 */
// int read_sector(int s, char* target_data) {
//     /* Call low-level driver */
//     uint8_t result = disk_read(
//         0xa0,    // Drive 0 (Master)
//         (uint32_t)s,          // LBA = Sector Number
//         1,                    // Read 1 sector
//         (uint8_t*)target_data,// Buffer
//         512      // 512 bytes
//     );
// 
//     /* Return 0 on success, 1 on error (matching FS expectations) */
//     return (result == 0) ? 0 : 1;
// }
// 
// /* 
//  * Wrapper for File System: Write Sector
//  * Matches: int write_sector(int s, char* data)
//  */
// int write_sector(int s, char* data) {
//     /* Call low-level driver */
//     uint8_t result = disk_write(
//         0xa0,    // Drive 0 (Master)
//         (uint32_t)s,          // LBA = Sector Number
//         1,                    // Write 1 sector
//         (uint8_t*)data,       // Buffer
//         512      // 512 bytes
//     );
// 
//     /* Return 0 on success, 1 on error */
//     return (result == 0) ? 0 : 1;
// }

#define read_sector(s, data) read_sector(0xa0, s, data)
#define write_sector(s, data) write_sector(0xa0, s, data)

/* -------------------------------------------------------------------------- */
/* 4. File System Constants & Structures                                      */
/* -------------------------------------------------------------------------- */

#define SECTOR_SIZE         512
#define MCFS_SIGNATURE      "MCFS"
#define FILE_TYPE_MARKER    0x80
#define EMPTY_SECTOR_MARKER 0x00
#define MAX_NAME_LEN        16
#define PATH_ENTRY_SIZE     2
#define PATH_AREA_OFFSET    22
#define PATH_AREA_SIZE      (SECTOR_SIZE - PATH_AREA_OFFSET)
#define MAX_PATH_ENTRIES    (PATH_AREA_SIZE / PATH_ENTRY_SIZE)

/* Partition Header (Sector 0) */
typedef struct {
    uint8_t  signature[4];      /* 0-3: "MCFS" */
    char     name[16];          /* 4-19: Partition Name */
    uint64_t size_sectors;      /* 20-27: Total size in sectors (little-endian) */
    uint8_t  reserved[440];     /* 28-511: Padding */
} __attribute__((packed)) mcfs_partition_t;

/* File Meta Record (1 Sector) */
typedef struct {
    uint8_t  type;              /* 0: 0x80 for file */
    uint8_t  attr;              /* 1: Attributes */
    char     name[16];          /* 2-17: Filename */
    uint32_t data_sectors;      /* 18-21: Length of data in sectors (little-endian) */
    uint16_t path[MAX_PATH_ENTRIES]; /* 22-511: Path array */
} __attribute__((packed)) mcfs_file_meta_t;

/* Error Codes */
#define MCFS_OK             0
#define MCFS_ERR_IO         1
#define MCFS_ERR_NOT_FOUND  2
#define MCFS_ERR_FULL       3
#define MCFS_ERR_INVALID    4
#define MCFS_ERR_TOO_BIG    5

/* -------------------------------------------------------------------------- */
/* 5. Helper Functions                                                        */
/* -------------------------------------------------------------------------- */

/* Read a 32-bit little-endian integer from buffer */
static uint32_t read_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | 
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* Write a 32-bit little-endian integer to buffer */
static void write_u32_le(uint8_t *buf, uint32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

/* Read a 64-bit little-endian integer from buffer */
static uint64_t read_u64_le(const uint8_t *buf) {
    uint64_t low = read_u32_le(buf);
    uint64_t high = read_u32_le(buf + 4);
    return low | (high << 32);
}

/* Write a 64-bit little-endian integer to buffer */
static void write_u64_le(uint8_t *buf, uint64_t val) {
    write_u32_le(buf, (uint32_t)(val & 0xFFFFFFFF));
    write_u32_le(buf + 4, (uint32_t)((val >> 32) & 0xFFFFFFFF));
}

/* -------------------------------------------------------------------------- */
/* 6. Core File System Logic                                                  */
/* -------------------------------------------------------------------------- */

/* 
 * Initialize and verify the partition. 
 * Should be called once at mount time.
 */
int mcfs_mount(void) {
    char sector_buf[SECTOR_SIZE];
    
    if (read_sector(0, sector_buf) != 0) {
        return MCFS_ERR_IO;
    }

    /* Check Signature "MCFS" */
    if (mcfs_memcmp(sector_buf, MCFS_SIGNATURE, 4) != 0) {
        return MCFS_ERR_INVALID;
    }

    return MCFS_OK;
}

/* 
 * Find a file by name. 
 * Returns the sector index of the file's meta record, or -1 if not found.
 */
static int mcfs_find_file(const char *name, mcfs_file_meta_t *out_meta) {
    char sector_buf[SECTOR_SIZE];
    int sector = 0;
    uint64_t partition_size = 0;

    /* Read partition header to know limits */
    if (read_sector(0, sector_buf) != 0) return -1;
    partition_size = read_u64_le((uint8_t*)sector_buf + 20);
    
    /* 
     * Scan sectors. 
     * Python logic: Start at 0. If 0x00 empty. If 0x80 file. Else skip 1.
     * Sector 0 is Partition Header ('M'), so it will be skipped automatically 
     * by the logic below, effectively starting file search at Sector 1.
     */
    while ((uint64_t)sector < partition_size + 1) {
        if (read_sector(sector, sector_buf) != 0) return -1;

        uint8_t type = (uint8_t)sector_buf[0];

        if (type == EMPTY_SECTOR_MARKER) {
            /* Hole in disk, file not here, continue */
            sector++;
        } else if (type == FILE_TYPE_MARKER) {
            /* Potential file, check name */
            /* Name is at offset 2, length 16 */
            char file_name[MAX_NAME_LEN + 1];
            mcfs_memcpy(file_name, sector_buf + 2, MAX_NAME_LEN);
            file_name[MAX_NAME_LEN] = '\0'; /* Null terminate for comparison */

            if (mcfs_strlen(name) <= MAX_NAME_LEN && mcfs_memcmp(file_name, name, mcfs_strlen(name)) == 0) {
                /* Found it */
                if (out_meta) {
                    mcfs_memcpy(out_meta, sector_buf, SECTOR_SIZE);
                }
                return sector;
            }

            /* Not the right name, skip data sectors */
            uint32_t data_len = read_u32_le((uint8_t*)sector_buf + 18);
            sector += (1 + data_len);
        } else {
            /* Unknown type (e.g., Partition Header 'M'), skip 1 */
            sector++;
        }
    }

    return -1; /* Not found */
}

/* 
 * Find free space for a new file.
 * Returns the starting sector index for the new meta record, or -1 if full.
 */
static int mcfs_find_free_space(uint32_t needed_sectors) {
    char sector_buf[SECTOR_SIZE];
    int sector = 0;
    uint64_t partition_size = 0;

    /* Read partition header */
    if (read_sector(0, sector_buf) != 0) return -1;
    partition_size = read_u64_le((uint8_t*)sector_buf + 20);

    while ((uint64_t)sector < partition_size + 1) {
        if (read_sector(sector, sector_buf) != 0) return -1;

        uint8_t type = (uint8_t)sector_buf[0];

        if (type == EMPTY_SECTOR_MARKER) {
            /* Found a hole. Check if we have enough contiguous space? 
             * The Python script simply takes the first 0x00 it finds.
             * It assumes the user manages fragmentation or the disk is linear.
             * To match Python 'add' logic exactly: take first 0x00.
             * Note: A robust FS would check if (sector + needed) is also free.
             * For this implementation, we match the Python 'add' simplicity:
             * It just overwrites the first 0x00 found.
             */
            return sector;
        } else if (type == FILE_TYPE_MARKER) {
            uint32_t data_len = read_u32_le((uint8_t*)sector_buf + 18);
            sector += (1 + data_len);
        } else {
            sector++;
        }
    }

    return -1; /* Disk Full */
}

/* 
 * Create/Write a file.
 * name: Filename (max 16 chars)
 * data: Pointer to file content
 * size: Size of file content in bytes
 * attr: Attribute byte (default 0xff)
 * path: Array of 16-bit path IDs (can be NULL)
 * path_len: Number of entries in path array
 */
int mcfs_write_file(const char *name, const char *data, uint32_t size, 
                    uint8_t attr, const uint16_t *path, int path_len) {
    
    if (mcfs_strlen(name) > MAX_NAME_LEN) return MCFS_ERR_TOO_BIG;
    
    /* Calculate sectors needed for data */
    uint32_t data_sectors = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t total_sectors_needed = 1 + data_sectors; /* 1 Meta + Data */

    /* Check if file already exists, if so, we could update, 
       but Python 'add' appends. Let's check existence to avoid duplicates 
       or implement overwrite logic. Python 'add' does not check existence, 
       it just finds first hole. We will follow Python 'add' behavior (append). */

    int start_sector = mcfs_find_free_space(total_sectors_needed);
    if (start_sector < 0) return MCFS_ERR_FULL;

    /* Prepare Meta Sector */
    char meta_buf[SECTOR_SIZE];
    mcfs_memset(meta_buf, 0, SECTOR_SIZE);

    meta_buf[0] = FILE_TYPE_MARKER;
    meta_buf[1] = attr;
    
    /* Copy Name (pad with zeros implicitly via memset) */
    mcfs_memcpy(meta_buf + 2, name, mcfs_strlen(name));

    /* Write Data Length (Sectors) */
    write_u32_le((uint8_t*)meta_buf + 18, data_sectors);

    /* Write Path */
    if (path && path_len > 0) {
        if (path_len > MAX_PATH_ENTRIES) path_len = MAX_PATH_ENTRIES;
        /* Path starts at offset 22. Each entry is 2 bytes little-endian */
        for (int i = 0; i < path_len; i++) {
            uint8_t *p = (uint8_t*)meta_buf + PATH_AREA_OFFSET + (i * 2);
            p[0] = path[i] & 0xFF;
            p[1] = (path[i] >> 8) & 0xFF;
        }
    }

    /* Write Meta Sector */
    if (write_sector(start_sector, meta_buf) != 0) return MCFS_ERR_IO;

    /* Write Data Sectors */
    uint32_t bytes_written = 0;
    char data_buf[SECTOR_SIZE];

    for (uint32_t i = 0; i < data_sectors; i++) {
        mcfs_memset(data_buf, 0, SECTOR_SIZE); /* Zero fill remainder */
        
        uint32_t chunk_size = SECTOR_SIZE;
        if (size - bytes_written < SECTOR_SIZE) {
            chunk_size = size - bytes_written;
        }

        mcfs_memcpy(data_buf, data + bytes_written, chunk_size);

        if (write_sector(start_sector + 1 + i, data_buf) != 0) {
            /* Rollback meta sector? In kernel, usually just return error */
            return MCFS_ERR_IO;
        }
        bytes_written += chunk_size;
    }

    return MCFS_OK;
}

/* 
 * Read a file.
 * name: Filename to find
 * buffer: Destination buffer (must be large enough)
 * max_size: Size of destination buffer
 * out_size: Pointer to store actual bytes read
 */
int mcfs_read_file(const char *name, char *buffer, uint32_t max_size, uint32_t *out_size) {
    mcfs_file_meta_t meta;
    int start_sector = mcfs_find_file(name, &meta);

    if (start_sector < 0) return MCFS_ERR_NOT_FOUND;

    uint32_t data_sectors = meta.data_sectors;
    uint32_t total_bytes = data_sectors * SECTOR_SIZE;

    if (total_bytes > max_size) {
        total_bytes = max_size; /* Truncate if buffer too small */
    }

    char sector_buf[SECTOR_SIZE];
    uint32_t bytes_read = 0;

    for (uint32_t i = 0; i < data_sectors; i++) {
        if (read_sector(start_sector + 1 + i, sector_buf) != 0) {
            return MCFS_ERR_IO;
        }

        uint32_t copy_len = SECTOR_SIZE;
        if (bytes_read + copy_len > total_bytes) {
            copy_len = total_bytes - bytes_read;
        }

        mcfs_memcpy(buffer + bytes_read, sector_buf, copy_len);
        bytes_read += copy_len;
        
        if (bytes_read >= total_bytes) break;
    }

    if (out_size) *out_size = bytes_read;
    return MCFS_OK;
}

/* 
 * Delete a file.
 * Marks the meta sector as 0x00. 
 * Note: This does not zero out the data sectors (matches typical simple FS behavior).
 */
int mcfs_delete_file(const char *name) {
    char sector_buf[SECTOR_SIZE];
    int start_sector = mcfs_find_file(name, NULL);

    if (start_sector < 0) return MCFS_ERR_NOT_FOUND;

    /* Read to ensure we don't corrupt adjacent data if IO fails mid-write, 
       but primarily we just need to write 0x00 to first byte */
    if (read_sector(start_sector, sector_buf) != 0) return MCFS_ERR_IO;

    sector_buf[0] = EMPTY_SECTOR_MARKER;

    if (write_sector(start_sector, sector_buf) != 0) return MCFS_ERR_IO;

    return MCFS_OK;
}

/* 
 * List files (Callback based to avoid printf).
 * callback function receives: (name, size_bytes, sector_index)
 */
typedef void (*mcfs_list_callback)(const char *name, uint32_t size, int sector);

int mcfs_list_files(mcfs_list_callback callback) {
    char sector_buf[SECTOR_SIZE];
    int sector = 0;
    uint64_t partition_size = 0;

    if (read_sector(0, sector_buf) != 0) return MCFS_ERR_IO;
    partition_size = read_u64_le((uint8_t*)sector_buf + 20);

    while ((uint64_t)sector < partition_size + 1) {
        if (read_sector(sector, sector_buf) != 0) return MCFS_ERR_IO;

        uint8_t type = (uint8_t)sector_buf[0];

        if (type == FILE_TYPE_MARKER) {
            char name[MAX_NAME_LEN + 1];
            mcfs_memcpy(name, sector_buf + 2, MAX_NAME_LEN);
            name[MAX_NAME_LEN] = '\0';

            uint32_t data_sectors = read_u32_le((uint8_t*)sector_buf + 18);
            
            if (callback) {
                callback(name, data_sectors * SECTOR_SIZE, sector);
            }

            sector += (1 + data_sectors);
        } else if (type == EMPTY_SECTOR_MARKER) {
            sector++;
        } else {
            sector++;
        }
    }
    return MCFS_OK;
}
#undef read_sector
#undef write_sector
