#ifndef __PHT_BOOTINFO_H__
#define __PHT_BOOTINFO_H__

/*
 * Интерфейс между загрузчиком (boot/bootsect.asm + boot/stage2.asm) и ядром.
 *
 * ВНИМАНИЕ: все смещения продублированы в boot/bootinfo.inc (для NASM).
 * При изменении структуры правьте оба файла! `make check_bootinfo`
 * (вызывается автоматически при сборке образа) проверяет их согласованность.
 */

#include <stdint.h>

/* Физический адрес структуры pht_bootinfo_t (0x500..0x7BFF — свободная
 * обычная память ниже загрузочного сектора). Также передаётся ядру в EBX. */
#ifndef PHT_BOOTINFO_ADDR            /* переопределяется только в tools/blkdev_test */
#define PHT_BOOTINFO_ADDR        0x00000600
#endif
#define PHT_BOOTINFO_MAGIC       0x544F4850u   /* "PHOT" little-endian */

/* Тип носителя, с которого нас загрузил BIOS */
#define PHT_MEDIA_UNKNOWN        0
#define PHT_MEDIA_FLOPPY         1   /* дискета / USB-FDD: CHS, 512 байт */
#define PHT_MEDIA_HDD            2   /* HDD / USB-HDD / IDE / SATA(legacy): LBA, 512 байт */
#define PHT_MEDIA_CDROM          3   /* El Torito (no emulation): LBA, 2048 байт */

/* Биты поля flags */
#define PHT_BOOTF_EDD            (1 << 0)   /* BIOS поддерживает int 13h расширения (AH=42h) */
#define PHT_BOOTF_RAMDISK        (1 << 1)   /* образ скопирован в память (ramdisk_addr валиден) */
#define PHT_BOOTF_ISO            (1 << 2)   /* MBR пропатчен El Torito boot-info-table (загрузка из ISO) */
#define PHT_BOOTF_A20            (1 << 3)   /* линия A20 успешно включена/проверена */

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x00: PHT_BOOTINFO_MAGIC */
    uint8_t  bios_drive;       /* 0x04: DL, полученный от BIOS */
    uint8_t  media_type;       /* 0x05: PHT_MEDIA_* */
    uint8_t  sector_shift;     /* 0x06: log2 размера сектора BIOS: 9 (512) или 11 (2048) */
    uint8_t  flags;            /* 0x07: PHT_BOOTF_* */
    uint32_t image_lba;        /* 0x08: первый сектор образа на носителе (в секторах BIOS) */
    uint32_t kernel_addr;      /* 0x0C: куда загружен init.bin (0x100000) */
    uint32_t kernel_sectors;   /* 0x10: сколько 512-байтных секторов ядра загружено */
    uint32_t ramdisk_addr;     /* 0x14: физический адрес копии образа (0 — нет) */
    uint32_t ramdisk_sectors;  /* 0x18: размер копии в 512-байтных секторах */
    uint32_t image_sectors;    /* 0x1C: полный размер образа в 512-байтных секторах */
    uint32_t fs_lba;           /* 0x20: смещение ФС внутри образа (512-байтные сектора) */
    uint32_t mem_total_kb;     /* 0x24: объём ОЗУ по int 15h E801 (0 — неизвестно) */
    uint8_t  reserved[24];     /* 0x28 .. 0x3F */
    uint8_t  mbr[512];         /* 0x40: первый сектор ОБРАЗА (сектор image_lba носителя);
                                        по нему ядро узнаёт «свой» диск среди устройств */
} pht_bootinfo_t;              /* итого 0x240 байт: 0x600 .. 0x840 */

/*
 * Состояние процессора при входе в ядро (jmp на kernel_addr, см. stage2 pm_entry):
 *   защищённый режим, страничная адресация выключена, прерывания запрещены,
 *   A20 открыт; CS = 0x08 (код, база 0, лимит 4 ГБ), DS=ES=FS=GS=SS = 0x10;
 *   EAX = PHT_BOOTINFO_MAGIC, EBX = PHT_BOOTINFO_ADDR, ESP = 0x7FFF0
 *   (стек растёт вниз в свободную область 0x10000..0x7FFFF).
 */

/*
 * Заголовок образа, расположенный в MBR по смещению 0x40 (сразу после
 * El Torito boot-info-table, занимающей байты 8..63). Значения подставляет
 * Makefile через -D при ассемблировании boot/bootsect.asm.
 */
#define PHT_IMGHDR_OFFSET        0x40
#define PHT_IMGHDR_MAGIC         0x42544850u   /* "PHTB" */

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* +0x00 "PHTB" */
    uint16_t version;          /* +0x04 = 1 */
    uint16_t stage2_sectors;   /* +0x06 размер stage2 в 512-байтных секторах (лежит сразу за MBR) */
    uint32_t init_lba;         /* +0x08 где в образе лежит init.bin (512-байтные сектора) */
    uint32_t init_sectors;     /* +0x0C сколько секторов init.bin грузить */
    uint32_t fs_lba;           /* +0x10 начало файловой системы в образе */
    uint32_t image_sectors;    /* +0x14 полный размер образа */
    uint32_t kernel_addr;      /* +0x18 адрес загрузки init.bin */
    uint32_t ramdisk_addr;     /* +0x1C адрес, куда stage2 копирует образ целиком */
} pht_imghdr_t;                /* 0x20 байт: 0x40 .. 0x5F */

/* El Torito boot-info-table (заполняется tools/mkiso.py / mkisofs -boot-info-table) */
#define PHT_BIT_OFFSET           8
typedef struct __attribute__((packed)) {
    uint32_t bi_pvd;           /* LBA (2048) Primary Volume Descriptor */
    uint32_t bi_file;          /* LBA (2048) загрузочного файла (нашего образа) внутри ISO */
    uint32_t bi_length;        /* длина загрузочного файла в байтах */
    uint32_t bi_csum;          /* контрольная сумма файла начиная с 64-го байта */
    uint8_t  bi_reserved[40];
} pht_bootinfotable_t;

static inline const pht_bootinfo_t* pht_bootinfo_get(void) {
    const pht_bootinfo_t* bi = (const pht_bootinfo_t*)PHT_BOOTINFO_ADDR;
    return (bi->magic == PHT_BOOTINFO_MAGIC) ? bi : (const pht_bootinfo_t*)0;
}

#endif /* __PHT_BOOTINFO_H__ */
