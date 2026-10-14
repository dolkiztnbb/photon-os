#ifndef MCFS_H
#define MCFS_H

#include <libc/string.h>
#include <cpu/ports.h>
#include <drivers/screen.h>

// Константы
#define SECTOR_SIZE 512
#define MAX_NAME_LEN 16
#define FILE_SIGNATURE 0x80
#define PARTITION_SIGNATURE "MCFS"

// Коды ошибок
#define ERR_INVALID_SIZE 1
#define ERR_INVALID_SIGNATURE 2
#define ERR_OVERWRITE 3
#define ERR_DATE_MISMATCH 4

// Структуры (упакованные для точного соответствия бинарному формату)
typedef struct {
    unsigned char signature;          // 0x80 для файлов
    unsigned char attributes;         // Атрибуты файла
    char name[MAX_NAME_LEN];          // Имя файла (0-терминированное)
    unsigned char reserved[16];       // Зарезервировано
    unsigned int length_sectors;      // Размер в секторах (big-endian)
} __attribute__((packed)) FileMeta;

typedef struct {
    char signature[4];                // "MCFS" для разделов
    char name[MAX_NAME_LEN];          // Имя раздела
    unsigned char reserved[8];        // Зарезервировано
    unsigned char size_sectors[8];    // Размер в секторах (big-endian)
} __attribute__((packed)) PartitionMeta;

void create_file_record(const char* name, const unsigned char* data, 
                        size_t data_len, unsigned char* output,
                        unsigned char attributes, 
                        unsigned int creation_date,
                        unsigned int modification_date);

int place_record(unsigned char* dest, const unsigned char* src, 
                        size_t src_len, unsigned int sector_pos);

void create_partition(const char* name, unsigned long long size_sectors,
                        unsigned char* output, unsigned char attributes,
                        unsigned int creation_date);

int read_file_record(const unsigned char* metasector, char* name_out,
                        unsigned char* attributes_out, unsigned int* sector_count_out);

int read_partition_record(const unsigned char* metasector, char* name_out,
                        unsigned long long* sector_count_out);

int validate_file_data(const unsigned char* data, unsigned int expected_sectors);
int check_partition_integrity(const unsigned char* partition, size_t total_size);
int check_file_integrity(const unsigned char* file_data, size_t total_size);

// Вспомогательные функции
unsigned int calculate_sector_count(size_t data_len);
unsigned int from_big_endian4(const unsigned char *bytes);
unsigned long long from_big_endian8(const unsigned char *bytes);

// Функции работы с диском (объявления из disk.c)
extern long long sect_avail;
void f_timeout();
long long get_total_sectors(char drive);
void get_disk_info(char drive, int* cylinders, int* heads, long long* sectors, char* isLBAsupported);
char read_sector(char drive, int lba, unsigned char *buffer);
char write_sector(char drive, int lba, const unsigned char *buffer); // Формат как у read_sector

#endif // MCFS_H
