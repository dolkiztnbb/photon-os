/*
 * tools/blkdev_test/sim.c — модель IDE-контроллера (PIIX-подобного) для
 * хост-тестов drivers/ata.c + drivers/blockdev.c + drivers/ramdisk.c.
 *
 * Эмулирует порты 1F0h/3F6h (первичный канал) и 170h/376h (вторичный):
 *   primary master   — ATA-диск, содержимое = буфер sim_hdd (LBA28/48, PIO);
 *   secondary master — ATAPI CD-ROM, содержимое = буфер sim_cd (READ(12),
 *                      READ CAPACITY, UNIT ATTENTION на первую команду,
 *                      данные отдаются порциями по 1024 байта — проверяем,
 *                      что драйвер правильно обрабатывает несколько DRQ-фаз).
 * Плюс заглушки ядра: kprint*, PCI config space с одним IDE-контроллером
 * 00:01.1 (prog-if 80h = оба канала в legacy-режиме), AHCI и USB отсутствуют.
 *
 * Этот файл не включает заголовки ядра — только хостовую libc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include "sim.h"

/* <string.h> не включаем: ниже определяем strcmp/strncpy с семантикой ядра
 * (libc/string.c), а они конфликтовали бы с хостовыми прототипами. */
void* memcpy(void* d, const void* s, unsigned long n);
void* memset(void* d, int v, unsigned long n);
unsigned long strlen(const char* s);

uint8_t* sim_hdd = 0;  uint32_t sim_hdd_sectors = 0;
uint8_t* sim_cd = 0;   uint32_t sim_cd_blocks = 0;
int sim_hdd_present = 1, sim_cd_present = 1;
int sim_quiet = 0;
unsigned long sim_io_count = 0;

typedef struct {
    uint16_t io, ctrl;
    uint8_t  regs[8];      /* последние записанные значения */
    uint8_t  prev[8];      /* предыдущие (LBA48: «старшие» байты пишутся первыми) */
    uint8_t  status, error, lba1, lba2, seccount;
    uint8_t  drive;        /* выбранное устройство: 0 master / 1 slave */
    /* буфер данных */
    uint8_t  buf[65536 + 2048];
    uint32_t buf_len, buf_pos;     /* всего и позиция */
    uint32_t chunk_end;            /* ATAPI: конец текущей DRQ-порции */
    uint32_t atapi_chunk;          /* размер порции */
    int      phase;                /* 0 idle, 1 data-in, 2 data-out(write), 3 awaiting packet */
    uint32_t write_lba;
    uint8_t  packet[12]; int packet_pos;
    int      unit_attention;
    int      kind[2];              /* KIND_NONE / KIND_ATA / KIND_ATAPI */
} channel_t;

enum { KIND_NONE, KIND_ATA, KIND_ATAPI };
enum { PH_IDLE, PH_DATA_IN, PH_DATA_OUT, PH_PACKET };

#define SR_BSY 0x80
#define SR_DRDY 0x40
#define SR_DF 0x20
#define SR_DRQ 0x08
#define SR_ERR 0x01

static channel_t chan[2];

static void sim_log(const char* fmt, ...) {
    if (sim_quiet) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

void sim_reset(void) {
    memset(chan, 0, sizeof(chan));
    chan[0].io = 0x1F0; chan[0].ctrl = 0x3F6;
    chan[1].io = 0x170; chan[1].ctrl = 0x376;
    chan[0].kind[0] = sim_hdd_present ? KIND_ATA : KIND_NONE;
    chan[1].kind[0] = sim_cd_present ? KIND_ATAPI : KIND_NONE;
    for (int c = 0; c < 2; c++) {
        chan[c].status = SR_DRDY;
        chan[c].unit_attention = 1;
        chan[c].atapi_chunk = 1024;
    }
}

static void put_string(uint16_t* id, int first_word, int nwords, const char* s) {
    int n = nwords * 2;
    char tmp[64];
    memset(tmp, ' ', sizeof(tmp));
    memcpy(tmp, s, strlen(s));
    for (int i = 0; i < nwords; i++)
        id[first_word + i] = (uint16_t)((tmp[i * 2] << 8) | (uint8_t)tmp[i * 2 + 1]);
    (void)n;
}

static void start_data_in(channel_t* c, uint32_t len) {
    c->buf_len = len; c->buf_pos = 0; c->phase = PH_DATA_IN;
    c->chunk_end = len;
    c->status = SR_DRDY | SR_DRQ;
}

static void atapi_next_chunk(channel_t* c) {
    uint32_t remaining = c->buf_len - c->buf_pos;
    uint32_t n = remaining < c->atapi_chunk ? remaining : c->atapi_chunk;
    c->chunk_end = c->buf_pos + n;
    c->lba1 = n & 0xFF; c->lba2 = (n >> 8) & 0xFF;
    c->status = SR_DRDY | SR_DRQ;
}

static void atapi_error(channel_t* c, int sense) {
    c->status = SR_DRDY | SR_ERR;
    c->error = (uint8_t)(sense << 4) | 0x04;    /* sense key | ABRT */
    c->phase = PH_IDLE;
}

static void atapi_execute(channel_t* c) {
    uint8_t* p = c->packet;
    if (c->unit_attention) {
        c->unit_attention = 0;
        sim_log("  [sim] ATAPI: UNIT ATTENTION on first command (op %02X)\n", p[0]);
        atapi_error(c, 6);
        return;
    }
    if (p[0] == 0x25) {                   /* READ CAPACITY */
        uint32_t last = sim_cd_blocks - 1;
        uint8_t r[8] = { last >> 24, last >> 16, last >> 8, last, 0, 0, 0x08, 0x00 };
        memcpy(c->buf, r, 8);
        c->buf_len = 8; c->buf_pos = 0; c->phase = PH_DATA_IN;
        atapi_next_chunk(c);
        return;
    }
    if (p[0] == 0xA8) {                   /* READ(12) */
        uint32_t lba = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
        uint32_t cnt = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 8) | p[9];
        if (cnt == 0 || cnt > 16 || lba + cnt > sim_cd_blocks) {
            sim_log("  [sim] ATAPI READ(12) out of range lba=%u cnt=%u\n", lba, cnt);
            atapi_error(c, 5);
            return;
        }
        memcpy(c->buf, sim_cd + (size_t)lba * 2048, cnt * 2048);
        c->buf_len = cnt * 2048; c->buf_pos = 0; c->phase = PH_DATA_IN;
        atapi_next_chunk(c);
        return;
    }
    sim_log("  [sim] ATAPI: unsupported packet op %02X\n", p[0]);
    atapi_error(c, 5);
}

static void command(channel_t* c, uint8_t cmd) {
    int kind = c->kind[c->drive];
    c->error = 0;
    if (kind == KIND_NONE) { c->status = 0; return; }

    switch (cmd) {
    case 0xEC: {  /* IDENTIFY DEVICE */
        if (kind == KIND_ATAPI) {
            c->lba1 = 0x14; c->lba2 = 0xEB;
            c->status = SR_DRDY | SR_ERR; c->error = 0x04; c->phase = PH_IDLE;
            return;
        }
        uint16_t id[256]; memset(id, 0, sizeof(id));
        id[0] = 0x0040;
        put_string(id, 10, 10, "SIM0001");
        put_string(id, 27, 20, "SIM ATA DISK");
        id[49] = 0x0200;                            /* LBA supported */
        id[60] = sim_hdd_sectors & 0xFFFF; id[61] = sim_hdd_sectors >> 16;
        id[83] = 1 << 10;                           /* LBA48 */
        id[100] = sim_hdd_sectors & 0xFFFF; id[101] = sim_hdd_sectors >> 16;
        memcpy(c->buf, id, 512);
        c->lba1 = 0; c->lba2 = 0;
        start_data_in(c, 512);
        return;
    }
    case 0xA1: {  /* IDENTIFY PACKET DEVICE */
        if (kind != KIND_ATAPI) { c->status = SR_DRDY | SR_ERR; c->error = 0x04; return; }
        uint16_t id[256]; memset(id, 0, sizeof(id));
        id[0] = 0x8580;                             /* ATAPI, CD-ROM, 12-byte packets */
        put_string(id, 10, 10, "SIMCD01");
        put_string(id, 27, 20, "SIM ATAPI CD-ROM");
        id[49] = 0x0200;
        memcpy(c->buf, id, 512);
        start_data_in(c, 512);
        return;
    }
    case 0x20: case 0x24: {  /* READ SECTORS (EXT) */
        if (kind != KIND_ATA) { c->status = SR_DRDY | SR_ERR; c->error = 0x04; return; }
        uint32_t lba, cnt;
        if (cmd == 0x24) {
            if (c->prev[4] || c->prev[5]) { sim_log("  [sim] LBA48 > 32 bit\n"); c->status = SR_DRDY | SR_ERR; c->error = 0x10; return; }
            lba = c->regs[3] | (c->regs[4] << 8) | (c->regs[5] << 16) | ((uint32_t)c->prev[3] << 24);
            cnt = c->regs[2] | (c->prev[2] << 8);
            if (cnt == 0) cnt = 65536;
        } else {
            lba = c->regs[3] | (c->regs[4] << 8) | (c->regs[5] << 16) | ((c->regs[6] & 0x0F) << 24);
            cnt = c->seccount ? c->seccount : 256;
            if (!(c->regs[6] & 0x40)) { sim_log("  [sim] CHS read not supported\n"); c->status = SR_DRDY | SR_ERR; c->error = 0x04; return; }
        }
        if (lba + cnt > sim_hdd_sectors) {
            sim_log("  [sim] ATA read out of range lba=%u\n", lba);
            c->status = SR_DRDY | SR_ERR; c->error = 0x10; return;
        }
        memcpy(c->buf, sim_hdd + (size_t)lba * 512, cnt * 512);
        start_data_in(c, cnt * 512);
        return;
    }
    case 0x30: case 0x34: {  /* WRITE SECTORS (EXT) */
        if (kind != KIND_ATA) { c->status = SR_DRDY | SR_ERR; c->error = 0x04; return; }
        uint32_t lba = c->regs[3] | (c->regs[4] << 8) | (c->regs[5] << 16) | ((c->regs[6] & 0x0F) << 24);
        uint32_t cnt = c->seccount ? c->seccount : 256;
        if (lba + cnt > sim_hdd_sectors) { c->status = SR_DRDY | SR_ERR; c->error = 0x10; return; }
        c->write_lba = lba; c->buf_len = cnt * 512; c->buf_pos = 0; c->phase = PH_DATA_OUT;
        c->status = SR_DRDY | SR_DRQ;
        return;
    }
    case 0xE7:    /* FLUSH CACHE */
        c->status = SR_DRDY; c->phase = PH_IDLE; return;
    case 0xA0: {  /* PACKET */
        if (kind != KIND_ATAPI) { c->status = SR_DRDY | SR_ERR; c->error = 0x04; return; }
        c->packet_pos = 0; c->phase = PH_PACKET;
        c->status = SR_DRDY | SR_DRQ;
        return;
    }
    default:
        sim_log("  [sim] unsupported ATA command %02X\n", cmd);
        c->status = SR_DRDY | SR_ERR; c->error = 0x04; c->phase = PH_IDLE;
        return;
    }
}

static channel_t* find_channel(uint16_t port, int* reg, int* is_ctrl) {
    for (int i = 0; i < 2; i++) {
        if (port >= chan[i].io && port < chan[i].io + 8) { *reg = port - chan[i].io; *is_ctrl = 0; return &chan[i]; }
        if (port == chan[i].ctrl) { *reg = 0; *is_ctrl = 1; return &chan[i]; }
    }
    return 0;
}

static int channel_empty(channel_t* c) { return c->kind[0] == KIND_NONE && c->kind[1] == KIND_NONE; }

static void write_reg(channel_t* c, int reg, uint8_t v) {
    c->prev[reg] = c->regs[reg];
    c->regs[reg] = v;
    switch (reg) {
    case 2: c->seccount = v; break;
    case 4: c->lba1 = v; break;
    case 5: c->lba2 = v; break;
    case 6: c->drive = (v >> 4) & 1; break;
    case 7: command(c, v); break;
    }
}

static uint8_t read_status(channel_t* c) {
    if (channel_empty(c)) return 0xFF;
    if (c->kind[c->drive] == KIND_NONE) return 0x00;
    return c->status;
}

/* --- cpu/ports.h -------------------------------------------------------------------- */

unsigned char port_byte_in(const uint16_t port) {
    int reg, is_ctrl;
    sim_io_count++;
    channel_t* c = find_channel(port, &reg, &is_ctrl);
    if (!c) return 0xFF;
    if (is_ctrl || reg == 7) return read_status(c);
    if (channel_empty(c)) return 0xFF;
    switch (reg) {
    case 1: return c->error;
    case 2: return c->seccount;
    case 3: return c->regs[3];
    case 4: return c->lba1;
    case 5: return c->lba2;
    case 6: return c->regs[6];
    case 0: { uint16_t w; w = 0; /* байтовое чтение данных не используется */ return (uint8_t)w; }
    }
    return 0;
}

void port_byte_out(const uint16_t port, const uint8_t data) {
    int reg, is_ctrl;
    sim_io_count++;
    channel_t* c = find_channel(port, &reg, &is_ctrl);
    if (!c) return;
    if (is_ctrl) {
        if (data & 0x04) {   /* SRST */
            c->phase = PH_IDLE; c->status = SR_DRDY;
            if (c->kind[c->drive] == KIND_ATAPI) { c->lba1 = 0x14; c->lba2 = 0xEB; }
        }
        return;
    }
    write_reg(c, reg, data);
}

unsigned short port_word_in(const uint16_t port) {
    int reg, is_ctrl;
    sim_io_count++;
    channel_t* c = find_channel(port, &reg, &is_ctrl);
    if (!c || is_ctrl || reg != 0) return 0xFFFF;
    if (c->phase != PH_DATA_IN || c->buf_pos >= c->chunk_end) {
        sim_log("  [sim] data read with no data pending (phase %d)\n", c->phase);
        return 0xFFFF;
    }
    uint16_t w = c->buf[c->buf_pos] | (c->buf[c->buf_pos + 1] << 8);
    c->buf_pos += 2;
    if (c->buf_pos >= c->chunk_end) {
        if (c->buf_pos >= c->buf_len) { c->phase = PH_IDLE; c->status = SR_DRDY; }
        else atapi_next_chunk(c);
    }
    return w;
}

void port_word_out(const uint16_t port, const uint16_t data) {
    int reg, is_ctrl;
    sim_io_count++;
    channel_t* c = find_channel(port, &reg, &is_ctrl);
    if (!c || is_ctrl || reg != 0) return;
    if (c->phase == PH_PACKET) {
        c->packet[c->packet_pos++] = data & 0xFF;
        c->packet[c->packet_pos++] = data >> 8;
        if (c->packet_pos >= 12) atapi_execute(c);
        return;
    }
    if (c->phase == PH_DATA_OUT) {
        c->buf[c->buf_pos++] = data & 0xFF;
        c->buf[c->buf_pos++] = data >> 8;
        if (c->buf_pos >= c->buf_len) {
            memcpy(sim_hdd + (size_t)c->write_lba * 512, c->buf, c->buf_len);
            c->phase = PH_IDLE; c->status = SR_DRDY;
        }
        return;
    }
    sim_log("  [sim] data write in wrong phase %d\n", c->phase);
}

void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
uint32_t inl(uint16_t port) { (void)port; return 0xFFFFFFFF; }

/* --- drivers/pci.h: один IDE-контроллер 00:01.1 (как PIIX3 в QEMU) ---------------- */

uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    if (bus != 0) return 0xFFFFFFFF;
    if (slot == 0 && func == 0) {            /* host bridge */
        if (offset == 0x00) return 0x12378086;
        if (offset == 0x08) return 0x06000000;
        if (offset == 0x0C) return 0x00000000;
        return 0;
    }
    if (slot == 1) {
        if (func == 0) {                     /* ISA bridge, multi-function */
            if (offset == 0x00) return 0x70008086;
            if (offset == 0x08) return 0x06010000;
            if (offset == 0x0C) return 0x00800000;
            return 0;
        }
        if (func == 1) {                     /* IDE, prog-if 0x80 (legacy ports, bus master) */
            if (offset == 0x00) return 0x70108086;
            if (offset == 0x04) return 0x00000001;
            if (offset == 0x08) return 0x01018000;
            if (offset == 0x0C) return 0x00000000;
            return 0;
        }
    }
    return 0xFFFFFFFF;
}

void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    (void)bus; (void)slot; (void)func; (void)offset; (void)value;
}

uint32_t pci_scan_bus(void) { return 0; }          /* AHCI нет */

/* --- drivers/ahci.h ------------------------------------------------------------------- */

uint8_t initialize_ahci_and_read(uint32_t abar) { (void)abar; return 2; }
uint8_t ahci_read_sector(uint32_t lba, char* buffer) { (void)lba; (void)buffer; return 1; }
uint8_t ahci_write_sector(uint32_t lba, const char* buffer) { (void)lba; (void)buffer; return 1; }

/* --- drivers/usb.h -------------------------------------------------------------------- */
/* USB-контроллеров в модели нет: usb_init() ничего не находит, как на машине без EHCI
 * (или с флешкой на xHCI) — blockdev.c должен выбрать rd0. */
int usb_init(int (*accept)(void* dev)) { (void)accept; return 0; }

/* --- kernel/debug.h ------------------------------------------------------------------- */

static void kvprint(const char* tag, const char* fmt, va_list ap) {
    if (sim_quiet) return;
    fprintf(stderr, "  [%s] ", tag);
    /* формат ядра: %x печатает hex без 0x — для теста обычного vfprintf достаточно */
    vfprintf(stderr, fmt, ap);
}
void kprintd(const char* fmt, ...) { va_list ap; va_start(ap, fmt); kvprint("D", fmt, ap); va_end(ap); }
void kprintw(const char* fmt, ...) { va_list ap; va_start(ap, fmt); kvprint("W", fmt, ap); va_end(ap); }
void kprinti(const char* fmt, ...) { va_list ap; va_start(ap, fmt); kvprint("I", fmt, ap); va_end(ap); }
void kprinte(const char* fmt, ...) { va_list ap; va_start(ap, fmt); kvprint("E", fmt, ap); va_end(ap); }

/* --- libc/mem.h, libc/string.h (семантика как в ядре) ----------------------------- */

void memory_copy(const void* source, void* dest, const int nbytes) { memcpy(dest, source, (unsigned long)nbytes); }
void memory_set(void* dest, const uint8_t val, const uint32_t len) { memset(dest, val, len); }
int strcmp(const char s1[], const char s2[]) {
    int i = 0;
    while (s1[i] && s1[i] == s2[i]) i++;
    return (unsigned char)s1[i] - (unsigned char)s2[i];
}
char* strncpy(char* d, const char* s, const uint32_t n) {
    uint32_t i = 0;
    while (s[i] != '\0' && i < n) { d[i] = s[i]; i++; }
    d[i] = '\0';
    return d;
}
