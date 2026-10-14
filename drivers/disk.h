#include <cpu/isr.h>
#include <cpu/ports.h>
#include <libc/string.h>
#include <libc/function.h>
#include <drivers/screen.h>

extern long long sect_avail;
extern int use_lba; // Флаг использования LBA (1 - LBA, 0 - CHS)

void f_timeout();

// Функция перевода LBA в CHS
void lba_to_chs(int lba, int *cylinder, int *head, int *sector, int heads, int sectors_per_track);

long long get_total_sectors(char drive);

void get_disk_info(char drive, int* cylinders, int* heads, long long* sectors, char* isLBASupported);

char read_sector(char drive, int lba, unsigned char *buffer);

char write_sector(char drive, int lba, const unsigned char *buffer);

char write_data(char drive, int lba, const unsigned char *buffer, int sects);
// Функция для установки режима адресации
void set_addressing_mode(int use_lba_mode);
