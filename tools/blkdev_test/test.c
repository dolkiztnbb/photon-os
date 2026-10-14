/*
 * tools/blkdev_test/test.c — хост-тест слоя блочных устройств ядра.
 *
 * Собирает drivers/ata.c, drivers/blockdev.c, drivers/ramdisk.c как обычную
 * 64-битную программу (порты и PCI эмулирует sim.c) и проверяет сценарии
 * выбора загрузочного устройства, чтение/запись через ATA PIO, чтение с
 * ATAPI (порциями, с UNIT ATTENTION) и работу ramdisk.
 *
 * Запуск: make test_blkdev   (или см. команду в Makefile)
 *   test <сценарий> <os-image.bin> <photon.iso>
 * Каждый сценарий — отдельный процесс, т.к. реестр устройств статический.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <drivers/blockdev.h>
#include <boot/bootinfo.h>
#include "sim.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

#define RAMDISK_ADDR 0x1000000u
#define ISO_IMAGE_BLOCK 23

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t* image; static long image_len;
static uint8_t* iso;   static long iso_len;

static uint8_t* read_file(const char* path, long* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(*len);
    if (fread(buf, 1, *len, f) != (size_t)*len) { perror("fread"); exit(2); }
    fclose(f);
    return buf;
}

static void* map_fixed(unsigned long addr, unsigned long size) {
    void* p = mmap((void*)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void*)addr) { fprintf(stderr, "mmap(%lx) failed\n", addr); exit(2); }
    return p;
}

static pht_bootinfo_t* bi;

static void setup_bootinfo(uint8_t media, uint8_t shift, uint32_t image_lba, uint8_t flags,
                           const uint8_t* mbr, int with_ramdisk) {
    memset(bi, 0, sizeof(*bi));
    bi->magic = PHT_BOOTINFO_MAGIC;
    bi->bios_drive = media == PHT_MEDIA_CDROM ? 0xE0 : 0x80;
    bi->media_type = media;
    bi->sector_shift = shift;
    bi->flags = flags;
    bi->image_lba = image_lba;
    bi->kernel_addr = 0x100000;
    bi->kernel_sectors = 324;
    bi->image_sectors = image_len / 512;
    bi->fs_lba = 384;
    bi->mem_total_kb = 65536;
    memcpy(bi->mbr, mbr, 512);
    if (with_ramdisk) {
        bi->flags |= PHT_BOOTF_RAMDISK;
        bi->ramdisk_addr = RAMDISK_ADDR;
        bi->ramdisk_sectors = image_len / 512;
        memcpy((void*)(unsigned long)RAMDISK_ADDR, image, image_len);
    }
}

static int sector_equals_image(uint32_t lba, const uint8_t* got) {
    return memcmp(got, image + (size_t)lba * 512, 512) == 0;
}

static void expect_boot(const char* name, uint32_t base) {
    blockdev_t* b = blockdev_boot();
    CHECK(b != 0, "загрузочное устройство не выбрано");
    if (!b) return;
    CHECK(strcmp(b->name, name) == 0, "загрузочное устройство %s, ожидалось %s", b->name, name);
    CHECK(blockdev_boot_image_base() == base, "image base %u, ожидалось %u", blockdev_boot_image_base(), base);
}

static void check_fs_readable(void) {
    static char buf[512];
    CHECK(blk_read_sector(384, buf) == 0, "blk_read_sector(384) вернул ошибку");
    CHECK(sector_equals_image(384, (uint8_t*)buf), "сектор 384 (суперблок ФС) не совпадает с образом");
    CHECK(blk_read_sector(0, buf) == 0, "blk_read_sector(0) вернул ошибку");
    CHECK(sector_equals_image(0, (uint8_t*)buf), "сектор 0 не совпадает с образом");
    CHECK(blk_read_sector(4095, buf) == 0, "blk_read_sector(4095) вернул ошибку");
    CHECK(sector_equals_image(4095, (uint8_t*)buf), "последний сектор не совпадает с образом");
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <scenario> <os-image.bin> <photon.iso>\n", argv[0]); return 2; }
    const char* sc = argv[1];
    image = read_file(argv[2], &image_len);
    iso = read_file(argv[3], &iso_len);
    if (getenv("SIM_QUIET")) sim_quiet = 1;

    bi = map_fixed(PHT_BOOTINFO_ADDR & ~0xFFFul, 0x1000);
    bi = (pht_bootinfo_t*)(unsigned long)PHT_BOOTINFO_ADDR;
    map_fixed(RAMDISK_ADDR, 4 * 1024 * 1024);

    /* по умолчанию: primary master = наш образ, secondary master = наш ISO */
    sim_hdd = malloc(image_len); memcpy(sim_hdd, image, image_len); sim_hdd_sectors = image_len / 512;
    sim_cd = iso; sim_cd_blocks = iso_len / 2048;
    const uint8_t* image_in_iso = iso + ISO_IMAGE_BLOCK * 2048;

    printf("== %s ==\n", sc);

    if (strcmp(sc, "hdd") == 0) {
        /* загрузка с IDE-диска: ядро должно найти образ на hd0 по совпадению MBR */
        setup_bootinfo(PHT_MEDIA_HDD, 9, 0, PHT_BOOTF_EDD | PHT_BOOTF_A20, image, 1);
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        CHECK(blockdev_count() == 3, "устройств %d, ожидалось 3 (hd0 cd0 rd0)", blockdev_count());
        expect_boot("hd0", 0);
        check_fs_readable();

        /* запись на hd0 реально попадает «на диск» */
        static char pat[512], back[512];
        for (int i = 0; i < 512; i++) pat[i] = (char)(i * 7 + 3);
        CHECK(blk_write_sector(4000, pat) == 0, "blk_write_sector(4000) вернул ошибку");
        CHECK(memcmp(sim_hdd + 4000 * 512, pat, 512) == 0, "данные записи не дошли до диска");
        CHECK(blk_read_sector(4000, back) == 0 && memcmp(back, pat, 512) == 0, "перечитанный сектор отличается");
        CHECK(blk_write_sector(99999999, pat) == 1, "запись за пределы диска должна вернуть ошибку");

        /* cd0: ATAPI, чтение по 512 через кэш 2048, UNIT ATTENTION на первой команде */
        blockdev_t* cd = blockdev_find("cd0");
        CHECK(cd != 0, "cd0 не найден");
        if (cd) {
            CHECK(cd->sector_size == 2048, "cd0 sector_size = %u", cd->sector_size);
            CHECK(cd->flags & BLK_FLAG_READONLY, "cd0 должен быть read-only");
            CHECK(cd->total_sectors == (uint32_t)(iso_len / 512), "cd0 total_sectors = %u", cd->total_sectors);
            CHECK(strcmp(cd->model, "SIM ATAPI CD-ROM") == 0, "cd0 model = '%s'", cd->model);
            static char cbuf[512];
            for (uint32_t s = 0; s < 8; s++) {
                CHECK(cd->read(cd, ISO_IMAGE_BLOCK * 4 + 384 + s, cbuf) == 0, "cd0 read %u failed", s);
                CHECK(memcmp(cbuf, image_in_iso + (384 + s) * 512, 512) == 0, "cd0 sector %u differs", s);
            }
            CHECK(cd->read(cd, 0xFFFFFFF0u, cbuf) == 1, "cd0: чтение за пределами диска должно вернуть ошибку");
        }
        blockdev_t* hd = blockdev_find("hd0");
        CHECK(hd && strcmp(hd->model, "SIM ATA DISK") == 0, "hd0 model");
        CHECK(hd && hd->total_sectors == (uint32_t)(image_len / 512), "hd0 total_sectors");
    }
    else if (strcmp(sc, "cd") == 0) {
        /* загрузка с CD: предпочитаем ramdisk (на CD писать нельзя) */
        memset(sim_hdd, 0, image_len);   /* на внутреннем диске чужие данные */
        setup_bootinfo(PHT_MEDIA_CDROM, 11, ISO_IMAGE_BLOCK, PHT_BOOTF_EDD | PHT_BOOTF_ISO | PHT_BOOTF_A20, image_in_iso, 1);
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        expect_boot("rd0", 0);
        check_fs_readable();
        static char pat[512];
        memset(pat, 0x42, 512);
        CHECK(blk_write_sector(4000, pat) == 0, "запись в ramdisk должна работать");
        CHECK(memcmp((void*)(unsigned long)(RAMDISK_ADDR + 4000 * 512), pat, 512) == 0, "запись не попала в ramdisk");
    }
    else if (strcmp(sc, "cd-nord") == 0) {
        /* загрузка с CD без ramdisk (мало памяти): работаем прямо с ATAPI-привода */
        memset(sim_hdd, 0, image_len);
        setup_bootinfo(PHT_MEDIA_CDROM, 11, ISO_IMAGE_BLOCK, PHT_BOOTF_EDD | PHT_BOOTF_ISO | PHT_BOOTF_A20, image_in_iso, 0);
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        expect_boot("cd0", ISO_IMAGE_BLOCK * 4);
        /* в ISO образ пропатчен boot-info-table (байты 8..63), сравниваем с копией из ISO */
        static char buf[512];
        CHECK(blk_read_sector(0, buf) == 0 && memcmp(buf, image_in_iso, 512) == 0, "сектор 0 образа с CD");
        CHECK(blk_read_sector(384, buf) == 0 && memcmp(buf, image_in_iso + 384 * 512, 512) == 0, "сектор 384 образа с CD");
        CHECK(blk_write_sector(384, buf) == 1, "запись на CD должна вернуть ошибку");
    }
    else if (strcmp(sc, "usb") == 0) {
        /* загрузка с флешки: BIOS видел HDD, но ядро не находит образ ни на одном диске -> rd0 */
        memset(sim_hdd, 0, image_len);
        setup_bootinfo(PHT_MEDIA_HDD, 9, 0, PHT_BOOTF_EDD | PHT_BOOTF_A20, image, 1);
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        expect_boot("rd0", 0);
        check_fs_readable();
    }
    else if (strcmp(sc, "usb-iso") == 0) {
        /* гибридный ISO лежит на IDE-диске: образ по смещению 92 секторов */
        free(sim_hdd);
        sim_hdd = malloc(iso_len); memcpy(sim_hdd, iso, iso_len); sim_hdd_sectors = iso_len / 512;
        setup_bootinfo(PHT_MEDIA_HDD, 9, ISO_IMAGE_BLOCK * 4, PHT_BOOTF_EDD | PHT_BOOTF_ISO | PHT_BOOTF_A20, image_in_iso, 1);
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        expect_boot("hd0", ISO_IMAGE_BLOCK * 4);
        static char buf[512];
        CHECK(blk_read_sector(384, buf) == 0 && memcmp(buf, image_in_iso + 384 * 512, 512) == 0, "сектор 384 через смещение образа");
    }
    else if (strcmp(sc, "legacy") == 0) {
        /* ядро запущено без нашего загрузчика: bootinfo нет, берём первый диск */
        memset(bi, 0, sizeof(*bi));
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        CHECK(blockdev_bootinfo() == 0, "bootinfo должен отсутствовать");
        expect_boot("hd0", 0);
        check_fs_readable();
    }
    else if (strcmp(sc, "nodisk") == 0) {
        /* ни дисков, ни ramdisk: инициализация должна вернуть ошибку, а не упасть */
        sim_hdd_present = 0; sim_cd_present = 0;
        setup_bootinfo(PHT_MEDIA_HDD, 9, 0, PHT_BOOTF_EDD | PHT_BOOTF_A20, image, 0);
        sim_reset();
        CHECK(blockdev_init_boot() == 1, "blockdev_init_boot должен вернуть 1");
        CHECK(blockdev_count() == 0, "устройств быть не должно");
        static char buf[512];
        CHECK(blk_read_sector(0, buf) == 1, "чтение без устройства должно вернуть 1");
        CHECK(blk_write_sector(0, buf) == 1, "запись без устройства должна вернуть 1");
    }
    else if (strcmp(sc, "cdonly") == 0) {
        /* машина только с CD-приводом, загрузка с него без ramdisk */
        sim_hdd_present = 0;
        setup_bootinfo(PHT_MEDIA_CDROM, 11, ISO_IMAGE_BLOCK, PHT_BOOTF_EDD | PHT_BOOTF_ISO | PHT_BOOTF_A20, image_in_iso, 0);
        sim_reset();
        CHECK(blockdev_init_boot() == 0, "blockdev_init_boot вернул ошибку");
        CHECK(blockdev_count() == 1, "устройств %d, ожидалось 1", blockdev_count());
        expect_boot("cd0", ISO_IMAGE_BLOCK * 4);
    }
    else {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }

    blockdev_print();
    printf("  %s (%lu port I/O)\n", failures ? "FAILED" : "OK", sim_io_count);
    return failures ? 1 : 0;
}
