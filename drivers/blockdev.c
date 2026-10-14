/*
 * drivers/blockdev.c — реестр блочных устройств и выбор загрузочного.
 *
 * Как выбирается устройство, с которого читается файловая система:
 *   1. Если ядро запущено нашим загрузчиком (bootinfo по адресу 0x600):
 *      - загрузка с CD-ROM  -> ramdisk rd0 (копия образа, сделанная stage2),
 *                              если его нет — ATAPI-привод, на котором лежит образ;
 *      - загрузка с HDD/USB -> устройство, у которого сектор image_lba совпадает
 *                              с MBR, который прочитал BIOS (так мы находим «свой»
 *                              диск среди SATA/IDE), иначе — ramdisk rd0 (так
 *                              работает USB-флешка: USB-стека в ядре нет).
 *   2. Без bootinfo (старый загрузчик / отладка) — первый найденный диск,
 *      образ считается лежащим с нулевого сектора.
 */
#include <drivers/blockdev.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <drivers/ramdisk.h>
#include <drivers/pci.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>

static blockdev_t   devices[BLK_MAX_DEVICES];
static int          device_count = 0;
static blockdev_t*  boot_dev = 0;
static uint32_t     image_base = 0;        /* смещение образа на boot_dev, 512-б сектора */
static int          warned_write = 0;

static pht_bootinfo_t bootinfo_copy;
static int            bootinfo_valid = 0;

/* --- реестр ------------------------------------------------------------------ */

int blockdev_register(const blockdev_t* dev) {
    if (!dev || !dev->read || device_count >= BLK_MAX_DEVICES) return -1;
    devices[device_count] = *dev;
    return device_count++;
}

int blockdev_count(void) { return device_count; }

blockdev_t* blockdev_get(int index) {
    if (index < 0 || index >= device_count) return 0;
    return &devices[index];
}

blockdev_t* blockdev_find(const char* name) {
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].name, name) == 0) return &devices[i];
    return 0;
}

blockdev_t* blockdev_boot(void)            { return boot_dev; }
uint32_t    blockdev_boot_image_base(void) { return image_base; }

const pht_bootinfo_t* blockdev_bootinfo(void) {
    return bootinfo_valid ? &bootinfo_copy : 0;
}

/* --- бэкенд AHCI (drivers/ahci.c) ------------------------------------------- */

static int ahci_blk_read(blockdev_t* dev, uint32_t lba, void* buf) {
    (void)dev;
    return ahci_read_sector(lba, (char*)buf) ? 1 : 0;
}

static int ahci_blk_write(blockdev_t* dev, uint32_t lba, const void* buf) {
    (void)dev;
    return ahci_write_sector(lba, (const char*)buf) ? 1 : 0;
}

static void ahci_probe(void) {
    uint32_t abar = pci_scan_bus();
    if (!abar) {
        kprintd("blockdev: no AHCI controller\n");
        return;
    }
    uint8_t r = initialize_ahci_and_read(abar);
    if (r == 2) {
        kprintd("blockdev: AHCI controller without SATA disks\n");
        return;
    }
    if (r != 0) {
        kprintw("blockdev: AHCI disk found but test read failed\n");
        return;
    }
    blockdev_t d;
    memory_set(&d, 0, sizeof(d));
    strncpy(d.name, "sd0", BLK_NAME_LEN - 1);
    strncpy(d.model, "SATA disk (AHCI)", sizeof(d.model) - 1);
    d.sector_size = 512;
    d.total_sectors = 0;
    d.read = ahci_blk_read;
    d.write = ahci_blk_write;
    blockdev_register(&d);
}

/* --- выбор загрузочного устройства ------------------------------------------ */

static const char* media_name(uint8_t media) {
    switch (media) {
        case PHT_MEDIA_FLOPPY: return "floppy/USB-FDD";
        case PHT_MEDIA_HDD:    return "HDD/USB-HDD";
        case PHT_MEDIA_CDROM:  return "CD-ROM";
        default:               return "unknown";
    }
}

/* Лежит ли на устройстве dev наш образ по смещению base? Сравниваем сектор с
 * MBR, который прочитал BIOS (без таблицы разделов: у гибридного ISO на флешке
 * она другая). */
static int device_has_image(blockdev_t* dev, uint32_t base) {
    static uint8_t sector[BLK_SECTOR];
    if (dev->flags & BLK_FLAG_VOLATILE) return 0;
    if (dev->read(dev, base, sector)) return 0;
    const uint8_t* mbr = bootinfo_copy.mbr;
    for (int i = 0; i < 0x1BE; i++)
        if (sector[i] != mbr[i]) return 0;
    return sector[510] == 0x55 && sector[511] == 0xAA;
}

static blockdev_t* find_image_device(uint32_t base, int want_cd) {
    for (int i = 0; i < device_count; i++) {
        int is_cd = devices[i].sector_size == 2048;
        if (is_cd != want_cd) continue;
        if (device_has_image(&devices[i], base)) return &devices[i];
    }
    return 0;
}

int blockdev_init_boot(void) {
    const pht_bootinfo_t* bi = pht_bootinfo_get();
    if (bi) {
        memory_copy(bi, &bootinfo_copy, sizeof(bootinfo_copy));
        bootinfo_valid = 1;
        kprintd("boot: media=%s drive=%x sector=%d image_lba=%d flags=%x ram=%dKB\n",
                media_name(bootinfo_copy.media_type), bootinfo_copy.bios_drive,
                1 << bootinfo_copy.sector_shift, bootinfo_copy.image_lba,
                bootinfo_copy.flags, bootinfo_copy.mem_total_kb);
    } else {
        kprintw("boot: no bootinfo from loader (legacy boot), assuming disk image at LBA 0\n");
    }

    ahci_probe();
    ata_init();
    if (bootinfo_valid) ramdisk_init_from_bootinfo(&bootinfo_copy);

    boot_dev = 0;
    image_base = 0;

    if (bootinfo_valid) {
        /* смещение образа на носителе в 512-байтных секторах */
        image_base = bootinfo_copy.image_lba << (bootinfo_copy.sector_shift - 9);
        blockdev_t* rd = blockdev_find("rd0");

        if (bootinfo_copy.media_type == PHT_MEDIA_CDROM) {
            if (rd) {
                boot_dev = rd;
            } else {
                boot_dev = find_image_device(image_base, 1);
                if (boot_dev)
                    kprintw("boot: running directly from CD-ROM %s (read-only)\n", boot_dev->name);
            }
        } else {
            boot_dev = find_image_device(image_base, 0);
            if (!boot_dev && rd) {
                boot_dev = rd;
                kprintw("boot: boot disk not accessible from kernel (USB?), using RAM copy\n");
            }
        }
        if (boot_dev == rd && rd) {
            image_base = 0;    /* в ramdisk образ лежит с нулевого сектора */
            kprintw("boot: filesystem is in RAM - changes will be lost at reboot\n");
        }
    } else {
        /* старый способ: первый диск, образ с нулевого сектора */
        for (int i = 0; i < device_count && !boot_dev; i++)
            if (devices[i].sector_size == 512 && !(devices[i].flags & BLK_FLAG_VOLATILE))
                boot_dev = &devices[i];
    }

    if (!boot_dev) {
        kprinte("boot: no usable boot device found (%d block devices)\n", device_count);
        return 1;
    }
    kprintd("boot: root device %s (%s), image at sector %d\n",
            boot_dev->name, boot_dev->model, image_base);
    return 0;
}

/* --- доступ к образу ----------------------------------------------------------- */

uint8_t blk_read_sector(uint32_t lba, char* buffer) {
    if (!boot_dev || !buffer) return 1;
    return boot_dev->read(boot_dev, image_base + lba, buffer) ? 1 : 0;
}

uint8_t blk_write_sector(uint32_t lba, const char* buffer) {
    if (!boot_dev || !buffer) return 1;
    if ((boot_dev->flags & BLK_FLAG_READONLY) || !boot_dev->write) {
        if (!warned_write) {
            warned_write = 1;
            kprintw("blk: %s is read-only, write to sector %d ignored\n", boot_dev->name, lba);
        }
        return 1;
    }
    if ((boot_dev->flags & BLK_FLAG_VOLATILE) && !warned_write) {
        warned_write = 1;
        kprintw("blk: writing to RAM copy of the disk, changes will not survive reboot\n");
    }
    return boot_dev->write(boot_dev, image_base + lba, buffer) ? 1 : 0;
}

/* --- lsblk ------------------------------------------------------------------------- */

void blockdev_print(void) {
    const pht_bootinfo_t* bi = blockdev_bootinfo();
    if (bi) {
        kprinti("booted from %s (BIOS drive %x), image at %s sector %d, flags %x\n",
                media_name(bi->media_type), bi->bios_drive,
                bi->sector_shift == 11 ? "2048-byte" : "512-byte",
                bi->image_lba, bi->flags);
    } else {
        kprinti("no bootinfo (legacy loader)\n");
    }
    if (device_count == 0) {
        kprinti("no block devices\n");
        return;
    }
    for (int i = 0; i < device_count; i++) {
        blockdev_t* d = &devices[i];
        uint32_t mb = d->total_sectors / 2048;
        kprinti("%s %s %s  %d-byte sectors  %d MB %s%s%s\n",
                d == boot_dev ? "*" : " ", d->name, d->model, d->sector_size, mb,
                (d->flags & BLK_FLAG_READONLY) ? "[ro]" : "",
                (d->flags & BLK_FLAG_REMOVABLE) ? "[removable]" : "",
                (d->flags & BLK_FLAG_VOLATILE) ? "[ram]" : "");
    }
    if (boot_dev)
        kprinti("root: %s, image base sector %d\n", boot_dev->name, image_base);
}
