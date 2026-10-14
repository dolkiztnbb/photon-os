/*
 * drivers/ata.c — IDE/PATA диски и ATAPI CD/DVD-приводы в режиме PIO.
 *
 * Простой и надёжный драйвер без DMA и прерываний: каждый сектор читается
 * командой READ SECTORS (0x20 / 0x24 для LBA48) через порт данных, CD-ROM —
 * командой PACKET + READ(12). Этого достаточно, чтобы грузить ОС с IDE-диска,
 * SATA-диска в режиме «IDE/compatibility» или с IDE-CD-ROM.
 *
 * Все устройства регистрируются в реестре drivers/blockdev.c.
 */
#include <drivers/ata.h>
#include <drivers/blockdev.h>
#include <drivers/pci.h>
#include <cpu/ports.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>

/* регистры (смещения от базы канала) */
#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_DRIVE       6
#define ATA_REG_STATUS      7
#define ATA_REG_COMMAND     7

/* биты регистра состояния */
#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

/* команды */
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_READ_PIO_EXT    0x24
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_WRITE_PIO_EXT   0x34
#define ATA_CMD_FLUSH           0xE7
#define ATA_CMD_PACKET          0xA0
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_IDENTIFY        0xEC

#define ATAPI_READ12            0xA8
#define ATAPI_READ_CAPACITY     0x25
#define ATAPI_BLOCK             2048

/* тайм-ауты в итерациях опроса (одно чтение порта ~1 мкс на реальном железе) */
#define ATA_TIMEOUT_BSY         2000000u    /* ~2 с  */
#define ATA_TIMEOUT_DRQ         2000000u
#define ATAPI_TIMEOUT           15000000u   /* привод может несколько секунд раскручивать диск */

typedef struct {
    uint16_t io;            /* база регистров (0x1F0) */
    uint16_t ctrl;          /* регистр управления / alt status (0x3F6) */
    uint8_t  slave;         /* 0 = master, 1 = slave */
    uint8_t  atapi;         /* 1 = ATAPI (CD/DVD) */
    uint8_t  lba48;
    uint8_t  present;
    uint32_t sectors;       /* ATA: 512-байтных секторов, ATAPI: блоков по 2048 (0 = нет диска) */
    char     model[41];
    char     serial[21];
    uint32_t cache_lba;     /* ATAPI: номер блока в cache, 0xFFFFFFFF — пусто */
    uint8_t  cache[ATAPI_BLOCK];
} ata_dev_t;

static ata_dev_t ata_devs[4];
static int ata_ndevs = 0;
static volatile int ata_busy = 0;   /* один PIO-транзакт за раз; ядро вытесняющее */

/* --- низкоуровневые помощники ---------------------------------------------------- */

static void ata_lock(void) {
    while (__sync_lock_test_and_set(&ata_busy, 1))
        asm volatile("pause");
}

static void ata_unlock(void) {
    __sync_lock_release(&ata_busy);
}

/* ~400 нс: четыре чтения alt status */
static void ata_delay400(uint16_t ctrl) {
    for (int i = 0; i < 4; i++) port_byte_in(ctrl);
}

static int ata_wait_bsy(uint16_t io, uint32_t timeout) {
    while (timeout--) {
        if (!(port_byte_in(io + ATA_REG_STATUS) & ATA_SR_BSY)) return 0;
    }
    return 1;
}

/* ждём DRQ. 0 — данные готовы, 1 — ошибка или тайм-аут */
static int ata_wait_drq(uint16_t io, uint32_t timeout) {
    while (timeout--) {
        uint8_t st = port_byte_in(io + ATA_REG_STATUS);
        if (st & ATA_SR_BSY) continue;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return 1;
        if (st & ATA_SR_DRQ) return 0;
    }
    return 1;
}

static void ata_select(ata_dev_t* d, uint8_t bits) {
    port_byte_out(d->io + ATA_REG_DRIVE, bits | (d->slave << 4));
    ata_delay400(d->ctrl);
}

static void ata_insw(uint16_t port, void* buf, uint32_t words) {
#ifdef ATA_HOST_TEST
    /* tools/blkdev_test: порты эмулируются функциями, rep insw недоступен */
    uint16_t* p = (uint16_t*)buf;
    for (uint32_t i = 0; i < words; i++) p[i] = port_word_in(port);
#else
    asm volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
#endif
}

static void ata_outsw(uint16_t port, const void* buf, uint32_t words) {
    const uint16_t* p = (const uint16_t*)buf;
    for (uint32_t i = 0; i < words; i++) port_word_out(port, p[i]);
}

static void ata_copy_string(char* dst, const uint16_t* id, int first_word, int nwords) {
    int n = nwords * 2;
    for (int i = 0; i < nwords; i++) {
        dst[i * 2]     = (char)(id[first_word + i] >> 8);
        dst[i * 2 + 1] = (char)(id[first_word + i] & 0xFF);
    }
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
    for (int i = 0; i < n; i++)
        if (dst[i] < 0x20 || dst[i] > 0x7E) dst[i] = '?';
}

/* --- IDENTIFY ------------------------------------------------------------------------ */

/* 1 — устройство есть и ответило, id[] заполнен; 0 — нет устройства */
static int ata_identify(ata_dev_t* d, uint16_t* id) {
    uint16_t io = d->io;

    ata_select(d, 0xA0);
    /* «плавающая шина» — на канале никого нет */
    if (port_byte_in(io + ATA_REG_STATUS) == 0xFF) return 0;

    port_byte_out(io + ATA_REG_SECCOUNT, 0);
    port_byte_out(io + ATA_REG_LBA0, 0);
    port_byte_out(io + ATA_REG_LBA1, 0);
    port_byte_out(io + ATA_REG_LBA2, 0);
    port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400(d->ctrl);

    uint8_t st = port_byte_in(io + ATA_REG_STATUS);
    if (st == 0 || st == 0xFF) return 0;
    if (ata_wait_bsy(io, ATA_TIMEOUT_BSY)) return 0;

    /* ATAPI-устройства отвергают IDENTIFY и выставляют сигнатуру 14h/EBh */
    uint8_t m = port_byte_in(io + ATA_REG_LBA1);
    uint8_t h = port_byte_in(io + ATA_REG_LBA2);
    if ((m == 0x14 && h == 0xEB) || (m == 0x69 && h == 0x96)) {
        d->atapi = 1;
        port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        ata_delay400(d->ctrl);
        if (ata_wait_bsy(io, ATA_TIMEOUT_BSY)) return 0;
    } else if ((m != 0 || h != 0) && !(m == 0x3C && h == 0xC3)) {
        return 0;   /* не ATA и не SATA-в-режиме-IDE */
    }

    if (ata_wait_drq(io, ATA_TIMEOUT_DRQ)) return 0;
    ata_insw(io + ATA_REG_DATA, id, 256);
    ata_delay400(d->ctrl);
    return 1;
}

static void ata_parse_identify(ata_dev_t* d, const uint16_t* id) {
    ata_copy_string(d->model, id, 27, 20);
    ata_copy_string(d->serial, id, 10, 10);
    if (d->atapi) {
        d->sectors = 0;      /* узнаем через READ CAPACITY */
        return;
    }
    d->lba48 = (id[83] & (1 << 10)) ? 1 : 0;
    if (d->lba48) {
        uint32_t lo = id[100] | ((uint32_t)id[101] << 16);
        uint32_t hi = id[102] | ((uint32_t)id[103] << 16);
        d->sectors = hi ? 0xFFFFFFFFu : lo;
    } else {
        d->sectors = id[60] | ((uint32_t)id[61] << 16);
    }
}

/* --- ATA: чтение/запись сектора ---------------------------------------------- */

/* Выдаёт команду на один сектор и ждёт DRQ. 0 = можно передавать данные. */
static int ata_pio_setup(ata_dev_t* d, uint32_t lba, int write) {
    uint16_t io = d->io;
    if (ata_wait_bsy(io, ATA_TIMEOUT_BSY)) return 1;

    if (lba >= (1u << 28)) {
        if (!d->lba48) return 1;
        ata_select(d, 0xE0);
        port_byte_out(io + ATA_REG_SECCOUNT, 0);               /* count[15:8]  */
        port_byte_out(io + ATA_REG_LBA0, (lba >> 24) & 0xFF);  /* lba[31:24]   */
        port_byte_out(io + ATA_REG_LBA1, 0);                   /* lba[39:32]   */
        port_byte_out(io + ATA_REG_LBA2, 0);                   /* lba[47:40]   */
        port_byte_out(io + ATA_REG_SECCOUNT, 1);               /* count[7:0]   */
        port_byte_out(io + ATA_REG_LBA0, lba & 0xFF);
        port_byte_out(io + ATA_REG_LBA1, (lba >> 8) & 0xFF);
        port_byte_out(io + ATA_REG_LBA2, (lba >> 16) & 0xFF);
        port_byte_out(io + ATA_REG_COMMAND, write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT);
    } else {
        ata_select(d, 0xE0 | ((lba >> 24) & 0x0F));
        port_byte_out(io + ATA_REG_FEATURES, 0);
        port_byte_out(io + ATA_REG_SECCOUNT, 1);
        port_byte_out(io + ATA_REG_LBA0, lba & 0xFF);
        port_byte_out(io + ATA_REG_LBA1, (lba >> 8) & 0xFF);
        port_byte_out(io + ATA_REG_LBA2, (lba >> 16) & 0xFF);
        port_byte_out(io + ATA_REG_COMMAND, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
    }
    ata_delay400(d->ctrl);
    return ata_wait_drq(io, ATA_TIMEOUT_DRQ);
}

static int ata_blk_read(blockdev_t* dev, uint32_t lba, void* buf) {
    ata_dev_t* d = (ata_dev_t*)dev->priv;
    int rc = 1;
    ata_lock();
    if (ata_pio_setup(d, lba, 0) == 0) {
        ata_insw(d->io + ATA_REG_DATA, buf, 256);
        ata_delay400(d->ctrl);
        rc = 0;
    }
    ata_unlock();
    if (rc) kprintw("ata: %s read error at LBA %d (status %x)\n", dev->name, lba,
                    port_byte_in(d->io + ATA_REG_STATUS));
    return rc;
}

static int ata_blk_write(blockdev_t* dev, uint32_t lba, const void* buf) {
    ata_dev_t* d = (ata_dev_t*)dev->priv;
    int rc = 1;
    ata_lock();
    if (ata_pio_setup(d, lba, 1) == 0) {
        ata_outsw(d->io + ATA_REG_DATA, buf, 256);
        ata_delay400(d->ctrl);
        if (ata_wait_bsy(d->io, ATA_TIMEOUT_BSY) == 0 &&
            !(port_byte_in(d->io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF))) {
            rc = 0;
            /* сброс кэша диска; ошибку (старые диски без FLUSH) не считаем фатальной */
            port_byte_out(d->io + ATA_REG_COMMAND, ATA_CMD_FLUSH);
            ata_delay400(d->ctrl);
            ata_wait_bsy(d->io, ATA_TIMEOUT_BSY);
        }
    }
    ata_unlock();
    if (rc) kprintw("ata: %s write error at LBA %d\n", dev->name, lba);
    return rc;
}

/* --- ATAPI --------------------------------------------------------------------------- */

/* Выполняет 12-байтный пакет, принимает до buflen байт. 0 = OK. */
static int atapi_packet(ata_dev_t* d, const uint8_t* pkt, void* buf, uint32_t buflen, uint32_t timeout) {
    uint16_t io = d->io;
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;

    if (ata_wait_bsy(io, ATA_TIMEOUT_BSY)) return 1;
    ata_select(d, 0xA0);
    port_byte_out(io + ATA_REG_FEATURES, 0);                 /* PIO, без DMA */
    port_byte_out(io + ATA_REG_SECCOUNT, 0);
    port_byte_out(io + ATA_REG_LBA0, 0);
    port_byte_out(io + ATA_REG_LBA1, buflen & 0xFF);         /* byte count limit */
    port_byte_out(io + ATA_REG_LBA2, (buflen >> 8) & 0xFF);
    port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_PACKET);
    ata_delay400(d->ctrl);
    if (ata_wait_drq(io, ATA_TIMEOUT_DRQ)) return 1;         /* готов принять пакет */

    ata_outsw(io + ATA_REG_DATA, pkt, 6);
    ata_delay400(d->ctrl);

    /* фаза данных: устройство может отдавать их несколькими порциями */
    for (;;) {
        if (ata_wait_bsy(io, timeout)) return 1;
        uint8_t st = port_byte_in(io + ATA_REG_STATUS);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return 1;
        if (!(st & ATA_SR_DRQ)) break;                        /* всё передано */
        uint32_t n = port_byte_in(io + ATA_REG_LBA1) | ((uint32_t)port_byte_in(io + ATA_REG_LBA2) << 8);
        if (n == 0) break;
        for (uint32_t i = 0; i < n / 2; i++) {
            uint16_t w = port_word_in(io + ATA_REG_DATA);
            if (got + 1 < buflen) { p[got] = w & 0xFF; p[got + 1] = w >> 8; }
            got += 2;
        }
        ata_delay400(d->ctrl);
    }
    return got >= buflen ? 0 : 1;
}

static int atapi_read_block(ata_dev_t* d, uint32_t block) {
    if (d->cache_lba == block) return 0;
    uint8_t pkt[12];
    memory_set(pkt, 0, sizeof(pkt));
    pkt[0] = ATAPI_READ12;
    pkt[2] = (block >> 24) & 0xFF;
    pkt[3] = (block >> 16) & 0xFF;
    pkt[4] = (block >> 8) & 0xFF;
    pkt[5] = block & 0xFF;
    pkt[9] = 1;                                 /* один блок */
    /* первая команда после смены диска / включения часто отвечает UNIT ATTENTION — повторяем */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (atapi_packet(d, pkt, d->cache, ATAPI_BLOCK, ATAPI_TIMEOUT) == 0) {
            d->cache_lba = block;
            return 0;
        }
    }
    d->cache_lba = 0xFFFFFFFFu;
    return 1;
}

static int atapi_blk_read(blockdev_t* dev, uint32_t lba, void* buf) {
    ata_dev_t* d = (ata_dev_t*)dev->priv;
    int rc;
    ata_lock();
    rc = atapi_read_block(d, lba / (ATAPI_BLOCK / BLK_SECTOR));
    if (rc == 0)
        memory_copy(d->cache + (lba % (ATAPI_BLOCK / BLK_SECTOR)) * BLK_SECTOR, buf, BLK_SECTOR);
    ata_unlock();
    if (rc) kprintw("ata: %s read error at sector %d\n", dev->name, lba);
    return rc;
}

static void atapi_read_capacity(ata_dev_t* d) {
    uint8_t pkt[12];
    uint8_t resp[8];
    memory_set(pkt, 0, sizeof(pkt));
    pkt[0] = ATAPI_READ_CAPACITY;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (atapi_packet(d, pkt, resp, 8, ATAPI_TIMEOUT) == 0) {
            uint32_t last = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                            ((uint32_t)resp[2] << 8) | resp[3];
            uint32_t bsize = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                             ((uint32_t)resp[6] << 8) | resp[7];
            d->sectors = (bsize == ATAPI_BLOCK) ? last + 1 : 0;
            return;
        }
    }
    d->sectors = 0;   /* нет диска в приводе */
}

/* --- обнаружение ------------------------------------------------------------------- */

static int hd_count = 0, cd_count = 0;

static void ata_probe_channel(uint16_t io, uint16_t ctrl) {
    if (!io || port_byte_in(io + ATA_REG_STATUS) == 0xFF) return;   /* канала нет */
    port_byte_out(ctrl, 0x02);                                      /* nIEN: без IRQ */

    for (int slave = 0; slave < 2 && ata_ndevs < 4; slave++) {
        ata_dev_t* d = &ata_devs[ata_ndevs];
        static uint16_t id[256];
        memory_set(d, 0, sizeof(*d));
        d->io = io;
        d->ctrl = ctrl;
        d->slave = slave;
        d->cache_lba = 0xFFFFFFFFu;

        if (!ata_identify(d, id)) continue;
        ata_parse_identify(d, id);

        /* старые контроллеры «отражают» master при опросе отсутствующего slave */
        if (slave == 1 && ata_ndevs > 0) {
            ata_dev_t* m = &ata_devs[ata_ndevs - 1];
            if (m->io == io && m->slave == 0 && strcmp(m->model, d->model) == 0 &&
                strcmp(m->serial, d->serial) == 0)
                continue;
        }
        if (!d->atapi && !(id[49] & 0x200)) {
            kprintw("ata: %s does not support LBA, skipped\n", d->model);
            continue;
        }
        if (d->atapi) atapi_read_capacity(d);
        d->present = 1;

        blockdev_t b;
        memory_set(&b, 0, sizeof(b));
        if (d->atapi) {
            b.name[0] = 'c'; b.name[1] = 'd'; b.name[2] = '0' + cd_count++; b.name[3] = 0;
            b.sector_size = ATAPI_BLOCK;
            b.total_sectors = d->sectors * (ATAPI_BLOCK / BLK_SECTOR);
            b.flags = BLK_FLAG_READONLY | BLK_FLAG_REMOVABLE;
            b.read = atapi_blk_read;
            b.write = 0;
        } else {
            b.name[0] = 'h'; b.name[1] = 'd'; b.name[2] = '0' + hd_count++; b.name[3] = 0;
            b.sector_size = BLK_SECTOR;
            b.total_sectors = d->sectors;
            b.flags = 0;
            b.read = ata_blk_read;
            b.write = ata_blk_write;
        }
        strncpy(b.model, d->model, sizeof(b.model) - 1);
        b.priv = d;
        blockdev_register(&b);
        kprintd("ata: %s = %s %s at %x (%s), %d MB\n", b.name, d->atapi ? "ATAPI" : "ATA",
                d->model, io, slave ? "slave" : "master", b.total_sectors / 2048);
        ata_ndevs++;
    }
}

static int ata_scan_pci(void) {
    int found = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t reg0 = pci_read_config_32(bus, dev, 0, 0x00);
            if ((reg0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_read_config_32(bus, dev, 0, 0x0C);
            int nfunc = (hdr & 0x00800000) ? 8 : 1;       /* multi-function? */
            for (int fn = 0; fn < nfunc; fn++) {
                uint32_t r0 = pci_read_config_32(bus, dev, fn, 0x00);
                if ((r0 & 0xFFFF) == 0xFFFF) continue;
                uint32_t r8 = pci_read_config_32(bus, dev, fn, 0x08);
                uint8_t cls  = (r8 >> 24) & 0xFF;
                uint8_t sub  = (r8 >> 16) & 0xFF;
                uint8_t prog = (r8 >> 8) & 0xFF;
                if (cls != 0x01 || sub != 0x01) continue;     /* IDE controller */

                uint32_t cmd = pci_read_config_32(bus, dev, fn, 0x04);
                if (!(cmd & 1)) pci_write_config_32(bus, dev, fn, 0x04, cmd | 1);   /* I/O space */

                uint16_t io0 = 0x1F0, c0 = 0x3F6, io1 = 0x170, c1 = 0x376;
                if (prog & 0x01) {   /* первичный канал в native-режиме: адреса из BAR0/BAR1 */
                    io0 = pci_read_config_32(bus, dev, fn, 0x10) & 0xFFFC;
                    c0  = (pci_read_config_32(bus, dev, fn, 0x14) & 0xFFFC) + 2;
                }
                if (prog & 0x04) {   /* вторичный канал в native-режиме: BAR2/BAR3 */
                    io1 = pci_read_config_32(bus, dev, fn, 0x18) & 0xFFFC;
                    c1  = (pci_read_config_32(bus, dev, fn, 0x1C) & 0xFFFC) + 2;
                }
                kprintd("ata: PCI IDE controller %x:%x.%x prog-if %x, ports %x/%x\n",
                        bus, dev, fn, prog, io0, io1);
                ata_probe_channel(io0, c0);
                ata_probe_channel(io1, c1);
                found++;
            }
        }
    }
    return found;
}

void ata_init(void) {
    ata_ndevs = 0;
    hd_count = 0;
    cd_count = 0;
    if (!ata_scan_pci()) {
        /* нет PCI IDE-контроллера: пробуем стандартные ISA-порты */
        kprintd("ata: no PCI IDE controller, probing legacy ports\n");
        ata_probe_channel(0x1F0, 0x3F6);
        ata_probe_channel(0x170, 0x376);
    }
    kprintd("ata: %d device(s) found\n", ata_ndevs);
}
