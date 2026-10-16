/*
 * drivers/ramdisk.c — блочное устройство поверх копии образа в ОЗУ.
 *
 * При загрузке с CD-ROM или USB-флешки ядро часто не может обращаться к
 * носителю само (ATAPI-привод может быть на SATA, а drivers/usb.c умеет только
 * EHCI — флешка на xHCI ему не видна), поэтому
 * stage2 ещё в реальном режиме через BIOS копирует весь образ в память по
 * адресу RAMDISK_ADDR (0x1000000, см. setup.h __PHT_MEM_BOOTDISK), а ядро
 * работает с этой копией. Записи работают, но пропадают после перезагрузки.
 */
#include <drivers/ramdisk.h>
#include <drivers/blockdev.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>
#include <setup.h>

/* столько памяти отображает init_paging() (cpu/mmio.c) */
#define RAMDISK_MAPPED_LIMIT  0x02000000u

static uint8_t* rd_base = 0;
static uint32_t rd_sectors = 0;

static int rd_read(blockdev_t* dev, uint32_t lba, void* buf) {
    (void)dev;
    if (lba >= rd_sectors) return 1;
    memory_copy(rd_base + (lba * BLK_SECTOR), buf, BLK_SECTOR);
    return 0;
}

static int rd_write(blockdev_t* dev, uint32_t lba, const void* buf) {
    (void)dev;
    if (lba >= rd_sectors) return 1;
    memory_copy(buf, rd_base + (lba * BLK_SECTOR), BLK_SECTOR);
    return 0;
}

void ramdisk_init_from_bootinfo(const pht_bootinfo_t* bi) {
    if (!bi || !(bi->flags & PHT_BOOTF_RAMDISK) || !bi->ramdisk_addr || !bi->ramdisk_sectors)
        return;

    uint32_t end = bi->ramdisk_addr + bi->ramdisk_sectors * BLK_SECTOR;
    if (end > RAMDISK_MAPPED_LIMIT || end < bi->ramdisk_addr) {
        kprintw("ramdisk: image at %x..%x is outside mapped memory, ignored\n",
                bi->ramdisk_addr, end);
        return;
    }
    if (bi->ramdisk_addr < __PHT_MEM_BOOTDISK || end > __PHT_MEM_BOOTDISK_END) {
        kprintw("ramdisk: image at %x..%x is outside __PHT_MEM_BOOTDISK, check setup.h\n",
                bi->ramdisk_addr, end);
        /* продолжаем: загрузчик лучше знает, куда он положил образ */
    }

    rd_base = (uint8_t*)bi->ramdisk_addr;
    rd_sectors = bi->ramdisk_sectors;

    blockdev_t d;
    memory_set(&d, 0, sizeof(d));
    strncpy(d.name, "rd0", BLK_NAME_LEN - 1);
    strncpy(d.model, "RAM copy of boot image", sizeof(d.model) - 1);
    d.sector_size = 512;
    d.total_sectors = rd_sectors;
    d.flags = BLK_FLAG_VOLATILE;
    d.read = rd_read;
    d.write = rd_write;
    blockdev_register(&d);

    if (rd_sectors < bi->image_sectors)
        kprintw("ramdisk: only %d of %d sectors were loaded by the bootloader\n",
                rd_sectors, bi->image_sectors);
    kprintd("ramdisk: rd0 at %x, %d sectors (%d KB)\n", rd_base, rd_sectors, rd_sectors / 2);
}
