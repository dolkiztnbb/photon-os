#ifndef BLOCKDEV_H
#define BLOCKDEV_H

/*
 * Единый интерфейс блочных устройств PhotonOS.
 *
 * Все устройства снаружи выглядят одинаково: массив 512-байтных секторов.
 * Бэкенды: drivers/ahci.c (SATA, "sd0"), drivers/ata.c (IDE/PATA диски
 * "hd0".."hd3" и ATAPI CD-ROM "cd0".."cd3"), drivers/ramdisk.c (копия
 * загрузочного образа в ОЗУ, "rd0" — так работает загрузка с USB/CD).
 *
 * Файловая система (kernel/datslaf/fs.c) и системные вызовы работают не с
 * устройством напрямую, а с «загрузочным образом» через blk_read_sector /
 * blk_write_sector: LBA там отсчитывается от начала образа ОС, а смещение
 * образа на носителе (0 на диске, bi_file*4 внутри ISO) добавляется здесь.
 */

#include <libc/stdint>
#include <boot/bootinfo.h>

#define BLK_MAX_DEVICES   8
#define BLK_NAME_LEN      8
#define BLK_SECTOR        512

#define BLK_FLAG_READONLY  0x01   /* запись невозможна (CD-ROM) */
#define BLK_FLAG_REMOVABLE 0x02   /* сменный носитель */
#define BLK_FLAG_VOLATILE  0x04   /* содержимое пропадёт при перезагрузке (ramdisk) */

typedef struct blockdev {
    char        name[BLK_NAME_LEN];
    char        model[41];        /* строка модели / описание */
    uint32_t    sector_size;      /* физический сектор устройства: 512 или 2048 */
    uint32_t    total_sectors;    /* ёмкость в 512-байтных секторах (0 = неизвестно) */
    uint32_t    flags;            /* BLK_FLAG_* */
    /* Чтение/запись ОДНОГО 512-байтного сектора. Возвращают 0 при успехе. */
    int       (*read)(struct blockdev* dev, uint32_t lba, void* buf);
    int       (*write)(struct blockdev* dev, uint32_t lba, const void* buf);
    void*       priv;
} blockdev_t;

/* Регистрация устройства бэкендом (структура копируется). Возвращает индекс или -1. */
int         blockdev_register(const blockdev_t* dev);
int         blockdev_count(void);
blockdev_t* blockdev_get(int index);
blockdev_t* blockdev_find(const char* name);

/* Находит все устройства, выбирает загрузочное. 0 — OK, иначе загрузочного
 * устройства нет (ФС работать не сможет). Вызывается один раз из main(). */
int         blockdev_init_boot(void);
blockdev_t* blockdev_boot(void);
uint32_t    blockdev_boot_image_base(void);   /* смещение образа на устройстве (512-б сектора) */

/* Копия структуры загрузчика (NULL, если ядро запущено без boot/stage2.asm). */
const pht_bootinfo_t* blockdev_bootinfo(void);

/* Доступ к загрузочному образу: LBA от начала образа. 0 = OK, 1 = ошибка.
 * Сигнатура совместима со старыми ahci_read_sector / ahci_write_sector. */
uint8_t     blk_read_sector(uint32_t lba, char* buffer);
uint8_t     blk_write_sector(uint32_t lba, const char* buffer);

/* Команда lsblk */
void        blockdev_print(void);

#endif /* BLOCKDEV_H */
