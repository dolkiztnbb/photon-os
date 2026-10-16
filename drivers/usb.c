/*
 * drivers/usb.c — ядро USB-стека: поиск контроллеров, опознание устройств,
 * USB-хабы и USB Mass Storage (Bulk-Only Transport / SCSI).
 *
 * Контроллеры обслуживают drivers/xhci.c (USB 3.x) и drivers/ehci.c
 * (USB 2.0) через операции из drivers/usb_hc.h. Всё работает в защищённом
 * режиме через DMA и MMIO, без прерываний (опрос).
 *
 * Что умеет:
 *   - перебирает ВСЕ контроллеры xHCI и EHCI на PCI. Сначала xHCI: на новых
 *     платах других нет, а на старых с отдельным чипом USB 3.0 (например,
 *     Renesas/NEC в ноутбуках 2011-2012 годов) флешка в синем порту видна
 *     только ему, и тогда EHCI-контроллеры (а с ними и USB-клавиатуры на них)
 *     ядро вообще не трогает;
 *   - USB-хабы: USB 2.0 (High-Speed) и USB 1.1 (Full-Speed), цепочки до 5
 *     хабов, на любом контроллере. В том числе встроенный «rate matching hub»
 *     (RMH) чипсетов Intel 5-9 серий: там к корневому порту EHCI подключён
 *     только он, а все разъёмы платы — к нему;
 *   - флешки USB 3.x (SuperSpeed, в корневом порту xHCI), USB 2.0 и USB 1.1 —
 *     на корневом порту или за хабом; Full-Speed за High-Speed хабом — через
 *     его transaction translator;
 *   - Low-Speed устройства (клавиатуры, мыши) драйвер не трогает вовсе.
 *
 * Чего (пока) нет: SuperSpeed-хабов (USB 3.0 флешка за USB 3.0 хабом не
 * видна; USB 2.0 флешка за таким хабом видна), UHCI/OHCI (флешки USB 1.1 в
 * корневом порту старых плат). Тогда ядро работает с копией образа в ОЗУ
 * (rd0), которую сделал загрузчик. Драйвера HID тоже нет: USB-клавиатура на
 * контроллере, который ядро забрало, перестаёт работать.
 *
 * ВСЕ ожидания ограничены по времени: если контроллер или устройство ведут
 * себя не так, как ожидается, драйвер сдаётся, и загрузка продолжается.
 */

#include <drivers/usb.h>
#include <drivers/usb_hc.h>
#include <drivers/blockdev.h>
#include <drivers/pci.h>
#include <cpu/ports.h>
#include <cpu/vmm.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>

extern uint32_t page_directory[1024];
extern void vmm_identity_map_page(uint32_t* pd, uintptr_t phys_addr, uint32_t flags);

/* ---- запросы классов, дескрипторы ------------------------------------------ */
#define USB_REQ_CLEAR_TT_BUFFER 0x08   /* класс hub (USB 2.0, 11.24.2.3) */
#define USB_REQ_BOT_RESET       0xFF   /* Bulk-Only Mass Storage Reset (class) */

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HUB            0x29
#define USB_DESC_SS_EP_COMP     0x30   /* SuperSpeed Endpoint Companion (USB 3.2, 9.6.7) */

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09
#define USB_PROTO_BOT           0x50

/* ---- хабы (USB 2.0, глава 11.24) ------------------------------------------ */
#define HUB_FEAT_PORT_ENABLE    1
#define HUB_FEAT_PORT_RESET     4
#define HUB_FEAT_PORT_POWER     8
#define HUB_FEAT_C_CONNECTION   16     /* C_PORT_*: 16 + номер бита в wPortChange */

/* wPortStatus / wPortChange */
#define HPS_CONNECTION          (1u << 0)
#define HPS_ENABLE              (1u << 1)
#define HPS_OVERCURRENT         (1u << 3)
#define HPS_RESET               (1u << 4)
#define HPS_LOW_SPEED           (1u << 9)
#define HPS_HIGH_SPEED          (1u << 10)

#define HPC_ALL                 0x1Fu

#define HUB_MAX_PORTS           15
#define HUB_CONNECT_WINDOW_MS   1000   /* сколько ждать подключения устройств к пустым портам хаба */
#define HUB_POLL_MS             50
#define HUB_RESET_TIMEOUT_MS    500

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY      0x25
#define SCSI_READ10             0x28
#define SCSI_WRITE10            0x2A

#define CBW_SIGNATURE           0x43425355 // "USBC"
#define CSW_SIGNATURE           0x53425355 // "USBS"

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_length;
    uint8_t  cb[16];
} usb_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;
} usb_csw_t;

/* Кэш упреждающего чтения: ядро читает ФС по одному сектору, а одна
 * SCSI-команда по USB стоит 3 передачи. Читаем сразу 8 секторов (4 КБ). */
#define USB_CACHE_SECTORS       8
static uint8_t    usb_cache[USB_CACHE_SECTORS * 512];
static uint32_t   usb_cache_lba = 0;
static uint32_t   usb_cache_count = 0;  /* 0 = кэш пуст */

/* ---- состояние ---------------------------------------------------------------- */
static const usb_hc_t* hc = 0;  /* контроллер, с которым сейчас работаем */
int usb_hc_dead = 0;

static usb_dev_t usb_devs[USB_MAX_DEVICES];
static int       usb_ndevs = 0;
static int       usb_identified = 0;    /* опознанных устройств на контроллере (для сообщений) */

/* дескрипторы последнего опрошенного устройства */
static uint8_t   dev_desc[18];
static uint8_t   cfg_desc[512];
static uint16_t  cfg_len = 0;

static usb_dev_t* msc_dev = 0;   /* флешка, с которой работает usb0 */
static uint8_t  usb_iface = 0;
static uint8_t  ep_in = 0, ep_out = 0;
static uint32_t usb_total_sectors = 0;
static char     usb_model[41] = "USB Flash Drive";
static uint32_t bot_tag_counter = 0x1000;
static int      usb_ready = 0;   /* usb0 зарегистрирован и работает */
static int      usb_failures = 0;/* подряд неудачных обращений к usb0 */

/* ---- общие помощники для драйверов контроллеров -------------------------------- */
/* ~1 мкс: чтение порта 0x80 идёт через медленную шину LPC/ISA. Не зависит от
 * прерываний и таймера, поэтому работает и внутри системного вызова. */
void usb_udelay(uint32_t us) {
    while (us--) port_byte_in(0x80);
}

void usb_mdelay(uint32_t ms) {
    while (ms--) usb_udelay(1000);
}

/* Байтовая запись в конфигурационное пространство PCI (в drivers/pci.c есть
 * только 32-битная, а семафоры USBLEGSUP принято трогать по байту). */
void usb_pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    port_byte_out(PCI_CONFIG_DATA + (off & 3), val);
}

/* Регистры контроллера: тождественное отображение без кэширования (PCD|PWT). */
void usb_map_mmio(uint32_t base, uint32_t size) {
    uint32_t first = base & ~0xFFFu;
    uint32_t pages = ((base & 0xFFFu) + size + 0xFFFu) >> 12;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t page = first + (i << 12);
        if (page < first) break;                 /* перешли через 4 ГБ */
        vmm_identity_map_page(page_directory, page, 0x1B);
        asm volatile("invlpg (%0)" :: "r"(page) : "memory");
    }
}

/* ---- передачи ----------------------------------------------------------------- */
static void usb_clear_tt_buffer(usb_dev_t* d, uint8_t ep, int bulk, int is_in);

static int usb_control_xfer(usb_dev_t* d, uint8_t req_type, uint8_t req, uint16_t val,
                            uint16_t idx, uint16_t len, void* data, uint16_t* actual) {
    if (actual) *actual = 0;
    if (usb_hc_dead) return XFER_DEAD;
    int res = hc->control(d, req_type, req, val, idx, len, data, actual);
    if (res == XFER_TIMEOUT || res == XFER_ERROR)
        usb_clear_tt_buffer(d, 0, 0, (req_type & 0x80) != 0);
    return res;
}

static int usb_control_msg(usb_dev_t* d, uint8_t req_type, uint8_t req, uint16_t val,
                           uint16_t idx, uint16_t len, void* data) {
    return usb_control_xfer(d, req_type, req, val, idx, len, data, 0);
}

/* Оборванная по таймауту или сбойная split-транзакция может оставить буфер
 * transaction translator'а в хабе занятым (USB 2.0, 11.17.5), и тогда все
 * следующие Full/Low-Speed передачи через этот TT будут стоять. Как и Linux,
 * после таких ошибок (но не после STALL) посылаем хабу Clear_TT_Buffer. */
static void usb_clear_tt_buffer(usb_dev_t* d, uint8_t ep, int bulk, int is_in) {
    if (!d || !d->tt || d->speed == USB_SPEED_HIGH || d->speed == USB_SPEED_SUPER || usb_hc_dead)
        return;
    uint16_t info = (uint16_t)((ep & 0x0F) | ((d->addr & 0x7F) << 4) | ((bulk ? 2 : 0) << 11));
    /* хабы у нас всегда в режиме одного TT (alt setting 0), поэтому wIndex = 1 */
    if (bulk) {
        usb_control_msg(d->tt, RT_PORT_OUT, USB_REQ_CLEAR_TT_BUFFER, info | (is_in ? 0x8000 : 0), 1, 0, 0);
    } else {                                     /* control: оба направления */
        usb_control_msg(d->tt, RT_PORT_OUT, USB_REQ_CLEAR_TT_BUFFER, info | 0x8000, 1, 0, 0);
        usb_control_msg(d->tt, RT_PORT_OUT, USB_REQ_CLEAR_TT_BUFFER, info, 1, 0, 0);
    }
}

static int usb_bulk_transfer(int is_in, void* data, uint32_t len, uint32_t* actual) {
    if (actual) *actual = 0;
    if (usb_hc_dead) return XFER_DEAD;
    int res = hc->bulk(msc_dev, is_in, data, len, actual);
    if (res == XFER_TIMEOUT || res == XFER_ERROR)
        usb_clear_tt_buffer(msc_dev, is_in ? ep_in : ep_out, 1, is_in);
    return res;
}

/* CLEAR_FEATURE(ENDPOINT_HALT): снимает STALL, toggle на обеих сторонах -> DATA0 */
static void usb_clear_halt(int is_in) {
    uint8_t ep = is_in ? (uint8_t)(ep_in | 0x80) : ep_out;
    usb_control_msg(msc_dev, RT_STD_EP_OUT, USB_REQ_CLEAR_FEATURE, 0, ep, 0, 0);
    if (!usb_hc_dead && hc->reset_bulk) hc->reset_bulk(msc_dev, is_in);
}

/* Reset Recovery из спецификации BOT 5.3.4 */
static void bot_reset_recovery(void) {
    usb_control_msg(msc_dev, RT_CLASS_IFACE_OUT, USB_REQ_BOT_RESET, 0, usb_iface, 0, 0);
    usb_mdelay(10);
    usb_clear_halt(1);
    usb_clear_halt(0);
}

/* -------------------------------------------------------------------------- */
/* SCSI-команда по Bulk-Only Transport                                         */
/* 0 — успех, 1 — команда не выполнена (CSW status 1), < 0 — ошибка транспорта */
/* -------------------------------------------------------------------------- */
static int usb_bot_scsi(const void* cmd, uint8_t cmd_len, void* data, uint32_t data_len, int is_in) {
    usb_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++bot_tag_counter;
    cbw.data_length = data_len;
    cbw.flags = is_in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cb_length = cmd_len;
    memcpy(cbw.cb, cmd, cmd_len);

    int r = usb_bulk_transfer(0, &cbw, 31, 0);
    if (r != XFER_OK) {
        if (r != XFER_DEAD) bot_reset_recovery();
        return -1;
    }

    if (data_len > 0) {
        r = usb_bulk_transfer(is_in, data, data_len, 0);
        if (r == XFER_STALL) {
            usb_clear_halt(is_in);          /* по BOT дальше всё равно читаем CSW */
        } else if (r != XFER_OK) {
            if (r != XFER_DEAD) bot_reset_recovery();
            return -2;
        }
    }

    usb_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    r = usb_bulk_transfer(1, &csw, 13, 0);
    if (r == XFER_STALL) {                  /* BOT 6.7.2: снять STALL и прочитать CSW ещё раз */
        usb_clear_halt(1);
        r = usb_bulk_transfer(1, &csw, 13, 0);
    }
    if (r != XFER_OK) {
        if (r != XFER_DEAD) bot_reset_recovery();
        return -3;
    }
    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag || csw.status > 1) {
        bot_reset_recovery();               /* phase error / мусор вместо CSW */
        return -4;
    }
    return csw.status;                      /* 0 или 1 */
}

/* REQUEST SENSE: сбрасывает sense-данные (иначе следующая команда тоже упадёт).
 * Возвращает (sense key << 8) | ASC или -1. */
#define SENSE_NO_MEDIUM         0x23A  /* NOT READY / MEDIUM NOT PRESENT */
static int usb_request_sense(void) {
    uint8_t cmd[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));
    if (usb_bot_scsi(cmd, 6, sense, 18, 1) != 0) return -1;
    return ((sense[2] & 0x0F) << 8) | sense[12];
}

/* READ(10)/WRITE(10) на count секторов с одним повтором. 0 — успех. */
static int usb_scsi_rw(uint32_t lba, uint32_t count, void* buf, int write) {
    uint8_t cmd[10];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    cmd[2] = (lba >> 24) & 0xFF;
    cmd[3] = (lba >> 16) & 0xFF;
    cmd[4] = (lba >> 8) & 0xFF;
    cmd[5] = lba & 0xFF;
    cmd[7] = (count >> 8) & 0xFF;
    cmd[8] = count & 0xFF;

    for (int attempt = 0; attempt < 2 && !usb_hc_dead; attempt++) {
        int r = usb_bot_scsi(cmd, 10, buf, count * 512, !write);
        if (r == 0) return 0;
        if (r == 1) usb_request_sense();    /* сбросить sense-данные, иначе следующая команда тоже упадёт */
    }
    return 1;
}

/* Чтение/запись одного 512-байтного сектора. После нескольких неудач подряд
 * устройство считается потерянным, чтобы ядро не ждало таймауты на каждом
 * секторе. */
static int usb_rw_sector(uint32_t lba, void* buf, int write) {
    if (!usb_ready || usb_hc_dead) return 1;

    int failed;
    if (!write && usb_cache_count && lba >= usb_cache_lba && lba - usb_cache_lba < usb_cache_count) {
        memcpy(buf, usb_cache + (lba - usb_cache_lba) * 512, 512);
        return 0;
    }

    if (write) {
        failed = usb_scsi_rw(lba, 1, buf, 1);
        if (usb_cache_count && lba >= usb_cache_lba && lba - usb_cache_lba < usb_cache_count) {
            if (failed) usb_cache_count = 0;
            else        memcpy(usb_cache + (lba - usb_cache_lba) * 512, buf, 512);
        }
    } else {
        uint32_t count = USB_CACHE_SECTORS;
        if (usb_total_sectors && lba < usb_total_sectors && usb_total_sectors - lba < count)
            count = usb_total_sectors - lba;
        usb_cache_count = 0;
        failed = usb_scsi_rw(lba, count, usb_cache, 0);
        if (failed && count > 1)              /* вдруг флешка не любит длинные чтения */
            failed = usb_scsi_rw(lba, count = 1, usb_cache, 0);
        if (!failed) {
            usb_cache_lba = lba;
            usb_cache_count = count;
            memcpy(buf, usb_cache, 512);
        }
    }

    if (!failed) {
        usb_failures = 0;
        return 0;
    }
    if (++usb_failures >= 3 || usb_hc_dead) {
        kprinte("usb0: device does not respond, disabled\n");
        usb_ready = 0;
    }
    return 1;
}

static int usb_blk_read(blockdev_t* dev, uint32_t lba, void* buf) {
    (void)dev;
    return usb_rw_sector(lba, buf, 0);
}

static int usb_blk_write(blockdev_t* dev, uint32_t lba, const void* buf) {
    (void)dev;
    return usb_rw_sector(lba, (void*)buf, 1);
}

/* -------------------------------------------------------------------------- */
/* Устройства: адресация и дескрипторы                                         */
/* -------------------------------------------------------------------------- */
static void copy_trimmed(char* dst, const uint8_t* src, int n) {
    int len = n;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
    for (int i = 0; i < len; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : '?';
    dst[len] = 0;
}

const char* usb_speed_name(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_HIGH:  return "high";
        case USB_SPEED_LOW:   return "low";
        case USB_SPEED_SUPER: return "super";
        default:              return "full";
    }
}

/* «Адрес» устройства для сообщений: корневой порт, затем порты хабов — 0.3.2 */
const char* usb_path(const usb_dev_t* d) {
    static char buf[32];
    uint8_t ports[USB_MAX_HUB_DEPTH + 2];
    int n = 0;
    for (; d && n < (int)sizeof(ports); d = d->parent) ports[n++] = d->port;
    char* p = buf;
    while (n--) {
        uint8_t v = ports[n];
        if (v >= 100) *p++ = '0' + v / 100;
        if (v >= 10)  *p++ = '0' + (v / 10) % 10;
        *p++ = '0' + v % 10;
        if (n) *p++ = '.';
    }
    *p = 0;
    return buf;
}

void usb_bus_begin(const usb_hc_t* ops) {
    hc = ops;
    usb_hc_dead = 0;
    usb_ndevs = 0;
    usb_identified = 0;
}

int usb_bus_identified(void) {
    return usb_identified;
}

usb_dev_t* usb_alloc_dev(usb_dev_t* parent, uint8_t port, uint8_t speed) {
    if (usb_ndevs >= USB_MAX_DEVICES) {
        kprintw("usb: too many devices on one controller, the rest are ignored\n");
        return 0;
    }
    usb_dev_t* d = &usb_devs[usb_ndevs++];
    memset(d, 0, sizeof(*d));
    d->parent = parent;
    d->port = port;
    d->speed = speed;
    d->depth = parent ? parent->depth + 1 : 0;
    d->root_port = parent ? parent->root_port : port;
    if (parent)                                  /* xHCI: порт хаба на ярусе depth */
        d->route = parent->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * parent->depth));
    if ((speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) && parent) {
        if (parent->speed == USB_SPEED_HIGH) {   /* TT — в этом High-Speed хабе */
            d->tt = parent;
            d->tt_hub = parent->addr;
            d->tt_port = port;
        } else {                                 /* Full-Speed хаб сам за TT */
            d->tt = parent->tt;
            d->tt_hub = parent->tt_hub;
            d->tt_port = parent->tt_port;
        }
    }
    return d;
}

/* Устройство больше не нужно (не ответило или не наше): освободить его
 * ресурсы в контроллере (у xHCI — слот). */
static void usb_release(usb_dev_t* d) {
    if (hc->dev_close && !usb_hc_dead) hc->dev_close(d);
}

/* Назначает адрес только что сброшенному устройству (оно отвечает по адресу 0)
 * и читает его дескрипторы в dev_desc / cfg_desc. 0 — успех. */
int usb_enumerate(usb_dev_t* d) {
    d->addr = 0;
    d->mps0 = (d->speed == USB_SPEED_SUPER) ? 512 : (d->speed == USB_SPEED_HIGH) ? 64 : 8;
    if (hc->dev_open && hc->dev_open(d) != 0) return -1;

    /* 1. Первые 8 байт дескриптора устройства по адресу 0 (как это делает
     *    Windows: некоторые флешки иначе не принимают SET_ADDRESS). */
    memset(dev_desc, 0, sizeof(dev_desc));
    int r = XFER_ERROR;
    for (int attempt = 0; attempt < 3 && !usb_hc_dead; attempt++) {
        r = usb_control_msg(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, 8, dev_desc);
        if (r == XFER_OK) break;
        usb_mdelay(20);
    }
    if (r != XFER_OK) goto fail;
    uint8_t mps = dev_desc[7];
    if (d->speed == USB_SPEED_FULL && (mps == 8 || mps == 16 || mps == 32 || mps == 64) && mps != d->mps0) {
        d->mps0 = mps;                           /* High-Speed — всегда 64, Low-Speed — 8, SuperSpeed — 512 */
        if (hc->set_mps0 && hc->set_mps0(d) != 0) goto fail;
    }

    if (hc->set_address(d) != 0) goto fail;
    usb_mdelay(10);                              /* SET_ADDRESS recovery: >= 2 мс */

    if (usb_control_msg(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, 18, dev_desc) != XFER_OK)
        goto fail;

    /* 2. Конфигурация: сначала 9 байт ради wTotalLength, потом целиком. */
    cfg_len = 0;
    memset(cfg_desc, 0, sizeof(cfg_desc));
    if (usb_control_msg(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, 9, cfg_desc) != XFER_OK)
        goto fail;
    uint16_t total = cfg_desc[2] | (cfg_desc[3] << 8);
    if (total < 9) goto fail;
    if (total > sizeof(cfg_desc)) total = sizeof(cfg_desc);
    uint16_t got = 0;
    if (usb_control_xfer(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, total, cfg_desc, &got) != XFER_OK)
        goto fail;
    cfg_len = got < total ? got : total;
    return 0;

fail:
    usb_release(d);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* USB Mass Storage                                                           */
/* -------------------------------------------------------------------------- */
/* Запускает флешку и, если accept согласен, регистрирует её как usb0.
 * 1 — usb0 зарегистрирован. */
static int usb_msc_probe(usb_dev_t* d, uint8_t config, uint8_t iface,
                         uint8_t in, uint16_t in_mps, uint8_t in_burst,
                         uint8_t out, uint16_t out_mps, uint8_t out_burst,
                         int (*accept)(blockdev_t*)) {
    usb_ready = 0;
    usb_cache_count = 0;
    msc_dev = d;
    usb_iface = iface;
    ep_in = in;
    ep_out = out;
    uint16_t def_mps = (d->speed == USB_SPEED_SUPER) ? 1024 : (d->speed == USB_SPEED_HIGH) ? 512 : 64;
    if (!in_mps) in_mps = def_mps;
    if (!out_mps) out_mps = def_mps;
    memcpy(usb_model, "USB Flash Drive", sizeof("USB Flash Drive"));

    /* Endpoint'ы — контроллеру до SET_CONFIGURATION (так требует xHCI 4.6.6);
     * после SET_CONFIGURATION toggle у bulk-endpoint'ов = DATA0. */
    if (hc->config_bulk(d, in, in_mps, in_burst, out, out_mps, out_burst) != 0)
        return 0;
    if (usb_control_msg(d, RT_STD_OUT, USB_REQ_SET_CONFIG, config, 0, 0, 0) != XFER_OK)
        return 0;
    usb_mdelay(10);

    /* Ждём готовности носителя (после включения флешка отвечает
     * «UNIT ATTENTION», пока у неё не спросят sense-данные). Пустой
     * картридер (их много в ноутбуках) и не отвечающее устройство бросаем
     * сразу, а не через 20 попыток. */
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    int ready = 0, no_medium = 0, dead = 0;
    for (int i = 0; i < 20 && !usb_hc_dead; i++) {
        int r = usb_bot_scsi(tur, 6, 0, 0, 0);
        if (r == 0) { ready = 1; break; }
        if (r == 1) {
            if (usb_request_sense() == SENSE_NO_MEDIUM) {
                if (++no_medium >= 3) {
                    kprintd("usb: port %s: no medium (empty card reader?), skipped\n", usb_path(d));
                    return 0;
                }
            } else {
                no_medium = 0;
            }
        } else if (++dead >= 3) {
            kprintw("usb: port %s: mass storage does not respond, skipped\n", usb_path(d));
            return 0;
        }
        usb_mdelay(100);
    }
    if (usb_hc_dead) return 0;
    if (!ready) kprintw("usb: port %s: medium is not ready, trying anyway\n", usb_path(d));

    uint8_t inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq_resp[36];
    memset(inq_resp, 0, sizeof(inq_resp));
    if (usb_bot_scsi(inq, 6, inq_resp, 36, 1) == 0) {
        if ((inq_resp[0] & 0x1F) != 0x00) {
            kprintd("usb: port %s: SCSI device type %x is not a disk\n", usb_path(d), inq_resp[0] & 0x1F);
            return 0;
        }
        char vid[9], pid[17], model[41];
        copy_trimmed(vid, &inq_resp[8], 8);
        copy_trimmed(pid, &inq_resp[16], 16);
        int n = 0;
        for (int i = 0; vid[i] && n < 40; i++) model[n++] = vid[i];
        if (n && pid[0] && n < 40) model[n++] = ' ';
        for (int i = 0; pid[i] && n < 40; i++) model[n++] = pid[i];
        model[n] = 0;
        if (n) memcpy(usb_model, model, n + 1);
    }

    uint8_t rc_cmd[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t rc_resp[8];
    memset(rc_resp, 0, sizeof(rc_resp));
    int r = usb_bot_scsi(rc_cmd, 10, rc_resp, 8, 1);
    if (r == 1) {                                /* часто проходит со второй попытки */
        usb_request_sense();
        r = usb_bot_scsi(rc_cmd, 10, rc_resp, 8, 1);
    }
    if (r != 0) {
        kprintw("usb: port %s: READ CAPACITY failed\n", usb_path(d));
        return 0;
    }
    uint32_t last_lba = ((uint32_t)rc_resp[0] << 24) | (rc_resp[1] << 16) | (rc_resp[2] << 8) | rc_resp[3];
    uint32_t blk_size = ((uint32_t)rc_resp[4] << 24) | (rc_resp[5] << 16) | (rc_resp[6] << 8) | rc_resp[7];
    if (blk_size != 512) {
        kprintw("usb: port %s: %d-byte sectors are not supported\n", usb_path(d), blk_size);
        return 0;
    }
    usb_total_sectors = last_lba + 1;
    usb_failures = 0;
    usb_ready = 1;

    blockdev_t bd;
    memset(&bd, 0, sizeof(bd));
    strncpy(bd.name, "usb0", BLK_NAME_LEN - 1);
    strncpy(bd.model, usb_model, sizeof(bd.model) - 1);
    bd.sector_size = 512;
    bd.total_sectors = usb_total_sectors;
    bd.flags = BLK_FLAG_REMOVABLE;
    bd.read = usb_blk_read;
    bd.write = usb_blk_write;

    if (accept && !accept(&bd)) {
        kprintd("usb: port %s: %s is not the boot disk, skipped\n", usb_path(d), usb_model);
        usb_ready = 0;
        return 0;
    }

    blockdev_register(&bd);
    kprinti("usb: mounted %s (%s), %d MB\n", bd.name, bd.model, bd.total_sectors / 2048);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* USB-хабы (USB 2.0, глава 11)                                               */
/* -------------------------------------------------------------------------- */
static int hub_port_feature(usb_dev_t* hub, uint8_t port, int set, uint16_t feature) {
    return usb_control_msg(hub, RT_PORT_OUT, set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE,
                           feature, port, 0, 0);
}

static int hub_port_status(usb_dev_t* hub, uint8_t port, uint16_t* status, uint16_t* change) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    uint16_t got = 0;
    if (usb_control_xfer(hub, RT_PORT_IN, USB_REQ_GET_STATUS, 0, port, 4, b, &got) != XFER_OK || got < 4)
        return -1;
    *status = b[0] | (b[1] << 8);
    *change = b[2] | (b[3] << 8);
    return 0;
}

/* Сбросить биты изменений порта (C_PORT_CONNECTION ... C_PORT_RESET) */
static void hub_clear_changes(usb_dev_t* hub, uint8_t port, uint16_t change) {
    for (int bit = 0; bit < 5; bit++)
        if (change & (1u << bit)) hub_port_feature(hub, port, 0, HUB_FEAT_C_CONNECTION + bit);
}

/* Сброс порта хаба. 0 — порт включён, в *speed скорость устройства. */
static int hub_port_reset(usb_dev_t* hub, uint8_t port, uint8_t* speed) {
    for (int attempt = 0; attempt < 3 && !usb_hc_dead; attempt++) {
        if (hub_port_feature(hub, port, 1, HUB_FEAT_PORT_RESET) != XFER_OK) return -1;
        uint16_t st = 0, ch = 0;
        int done = 0;
        /* хаб держит reset 10..20 мс, для High-Speed ещё и chirp (USB 2.0 7.1.7.5) */
        for (uint32_t ms = 0; ms < HUB_RESET_TIMEOUT_MS; ms += 10) {
            usb_mdelay(10);
            if (hub_port_status(hub, port, &st, &ch) != 0) return -1;
            if (!(st & HPS_RESET)) { done = 1; break; }
        }
        hub_clear_changes(hub, port, ch & HPC_ALL);
        if (!(st & HPS_CONNECTION)) return -1;  /* устройство отключили */
        if (done && (st & HPS_ENABLE)) {
            *speed = (st & HPS_LOW_SPEED) ? USB_SPEED_LOW :
                     (st & HPS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
            usb_mdelay(10);                      /* TRSTRCY */
            return 0;
        }
        usb_mdelay(50);
    }
    return -1;
}

/* Устройство подключено к порту хаба: сброс, адрес, опознание. 1 — usb0 найден. */
static int hub_port_probe(usb_dev_t* hub, uint8_t port, int (*accept)(blockdev_t*)) {
    for (int attempt = 0; attempt < 2 && !usb_hc_dead; attempt++) {
        uint8_t speed = USB_SPEED_FULL;
        if (hub_port_reset(hub, port, &speed) != 0) {
            kprintd("usb: port %s.%d: port reset failed\n", usb_path(hub), port);
            break;
        }
        if (speed == USB_SPEED_LOW) {            /* клавиатура/мышь: флешкой быть не может */
            kprintd("usb: port %s.%d: low-speed device, skipped\n", usb_path(hub), port);
            break;
        }
        usb_dev_t* d = usb_alloc_dev(hub, port, speed);
        if (!d) break;
        if (usb_enumerate(d) == 0) {
            /* Опознанное, но не наше устройство (сканер отпечатков, камера,
             * Bluetooth, чужая флешка...) просто остаётся на своём адресе,
             * как в Linux: порт не трогаем. */
            return usb_attach(d, accept);
        }
        /* бывает, что устройство не отвечает с первого раза — ещё один сброс */
        kprintd("usb: port %s: %s-speed device does not answer\n", usb_path(d), usb_speed_name(speed));
    }
    /* Устройство так и не ответило и может остаться на адресе 0 — выключаем
     * порт, чтобы оно не отзывалось вместо следующих устройств. */
    hub_port_feature(hub, port, 0, HUB_FEAT_PORT_ENABLE);
    return 0;
}

/* Обход портов хаба. Устройства, которые уже были подключены, видны сразу;
 * к пустым портам ещё HUB_CONNECT_WINDOW_MS ждём медленные устройства
 * (после сброса хаба флешке нужно время, чтобы снова подключиться). */
static int hub_scan_ports(usb_dev_t* hub, uint8_t nports, int (*accept)(blockdev_t*)) {
    uint32_t pending = ((1u << nports) - 1) << 1;   /* биты 1..nports */
    uint32_t waited = 0;
    int errors = 0;
    for (;;) {
        int debounced = 0;
        for (uint8_t p = 1; p <= nports && !usb_hc_dead; p++) {
            if (!(pending & (1u << p))) continue;
            uint16_t st, ch;
            if (hub_port_status(hub, p, &st, &ch) != 0) {
                if (++errors >= 3) {
                    kprintw("usb: hub at port %s does not respond, skipped\n", usb_path(hub));
                    return 0;
                }
                continue;
            }
            errors = 0;
            if (ch & HPC_ALL) hub_clear_changes(hub, p, ch & HPC_ALL);
            if (!(st & HPS_CONNECTION)) continue;
            pending &= ~(1u << p);
            if (st & HPS_OVERCURRENT) {
                kprintw("usb: port %s.%d: over-current, skipped\n", usb_path(hub), p);
                continue;
            }
            if (st & HPS_LOW_SPEED) {            /* до сброса не трогаем вовсе */
                kprintd("usb: port %s.%d: low-speed device, skipped\n", usb_path(hub), p);
                continue;
            }
            if (!debounced) {                    /* TATTDB: подключение должно устояться */
                usb_mdelay(100);
                debounced = 1;
            }
            if (hub_port_probe(hub, p, accept)) return 1;
        }
        if (!pending || usb_hc_dead || waited >= HUB_CONNECT_WINDOW_MS) break;
        usb_mdelay(HUB_POLL_MS);
        waited += HUB_POLL_MS;
    }
    return 0;
}

static int usb_hub_probe(usb_dev_t* hub, uint8_t config, int (*accept)(blockdev_t*)) {
    if (hub->speed == USB_SPEED_SUPER) {
        kprintw("usb: port %s: SuperSpeed hubs are not supported yet, skipped\n", usb_path(hub));
        return 0;
    }
    if (hub->depth >= USB_MAX_HUB_DEPTH) {
        kprintw("usb: port %s: too many hubs in a chain, skipped\n", usb_path(hub));
        return 0;
    }
    if (usb_control_msg(hub, RT_STD_OUT, USB_REQ_SET_CONFIG, config, 0, 0, 0) != XFER_OK)
        return 0;

    uint8_t hd[16];
    uint16_t got = 0;
    memset(hd, 0, sizeof(hd));
    if (usb_control_xfer(hub, RT_HUB_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_HUB << 8, 0,
                         sizeof(hd), hd, &got) != XFER_OK || got < 7 || hd[1] != USB_DESC_HUB) {
        kprintw("usb: port %s: cannot read hub descriptor\n", usb_path(hub));
        return 0;
    }
    uint8_t nports = hd[2];
    uint16_t characteristics = hd[3] | (hd[4] << 8);
    uint32_t pgood_ms = (uint32_t)hd[5] * 2;     /* bPwrOn2PwrGood, в 2-мс единицах */
    if (nports == 0) return 0;
    if (nports > HUB_MAX_PORTS) {
        kprintw("usb: port %s: hub has %d ports, only %d are used\n", usb_path(hub), nports, HUB_MAX_PORTS);
        nports = HUB_MAX_PORTS;
    }
    kprintd("usb: port %s: %d-port %s-speed hub\n", usb_path(hub), nports, usb_speed_name(hub->speed));

    /* xHCI должен знать, что это хаб (порты, TT think time), до того, как
     * за ним появятся устройства. Хаб работает в режиме одного TT. */
    if (hc->hub_config && hc->hub_config(hub, nports, (characteristics >> 5) & 3, 0) != 0) {
        kprintw("usb: port %s: controller does not accept the hub\n", usb_path(hub));
        return 0;
    }

    /* Питание портов (хабы без управления питанием эту команду просто
     * принимают) и ожидание его стабилизации: не меньше 100 мс. */
    for (uint8_t p = 1; p <= nports; p++) hub_port_feature(hub, p, 1, HUB_FEAT_PORT_POWER);
    usb_mdelay(pgood_ms > 100 ? pgood_ms : 100);

    return hub_scan_ports(hub, nports, accept);
}

/* -------------------------------------------------------------------------- */
/* Опознание устройства с уже назначенным адресом (после usb_enumerate):      */
/* хаб — обходим его порты, флешка — пробуем как usb0.                         */
/* 1 — usb0 найден и зарегистрирован                                           */
/* -------------------------------------------------------------------------- */
int usb_attach(usb_dev_t* d, int (*accept)(blockdev_t*)) {
    uint8_t  dev_class = dev_desc[4];
    uint16_t vid = dev_desc[8] | (dev_desc[9] << 8);
    uint16_t pid = dev_desc[10] | (dev_desc[11] << 8);
    uint8_t  config = cfg_desc[5];
    usb_identified++;

    /* Интерфейс хаба или Mass Storage / Bulk-Only (alt setting 0) с парой
     * bulk-endpoint'ов. cfg_desc нужно разобрать сейчас: его перезапишут
     * устройства за хабом. */
    int is_hub = (dev_class == USB_CLASS_HUB);
    int msc = 0, in_msc = 0;
    uint8_t iface = 0, in = 0, out = 0, in_burst = 0, out_burst = 0;
    uint16_t in_mps = 0, out_mps = 0;
    uint8_t* last_ep = 0;                        /* к нему относится SS Endpoint Companion */
    for (uint16_t off = 0; off + 2 <= cfg_len; ) {
        uint8_t len  = cfg_desc[off];
        uint8_t type = cfg_desc[off + 1];
        if (len < 2 || off + len > cfg_len) break;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (in_msc && in && out) msc = 1;
            in_msc = 0;
            last_ep = 0;
            if (cfg_desc[off + 3] == 0) {        /* alt setting 0 */
                if (cfg_desc[off + 5] == USB_CLASS_HUB) is_hub = 1;
                if (!msc && cfg_desc[off + 5] == USB_CLASS_MASS_STORAGE && cfg_desc[off + 7] == USB_PROTO_BOT) {
                    in_msc = 1;
                    iface = cfg_desc[off + 2];
                    in = out = 0;
                    in_burst = out_burst = 0;
                }
            }
        } else if (type == USB_DESC_ENDPOINT && len >= 7 && in_msc) {
            uint8_t  ep_addr = cfg_desc[off + 2];
            uint16_t mps = (cfg_desc[off + 4] | (cfg_desc[off + 5] << 8)) & 0x7FF;
            last_ep = 0;
            if ((cfg_desc[off + 3] & 0x03) == 0x02) {    /* bulk */
                if (ep_addr & 0x80) { in  = ep_addr & 0x0F; in_mps  = mps; last_ep = &in_burst; }
                else                { out = ep_addr & 0x0F; out_mps = mps; last_ep = &out_burst; }
            }
        } else if (type == USB_DESC_SS_EP_COMP && len >= 6 && last_ep) {
            *last_ep = cfg_desc[off + 2] & 0x0F;         /* bMaxBurst */
            last_ep = 0;
        }
        off += len;
    }
    if (in_msc && in && out) msc = 1;

    int found = 0;
    if (is_hub) {
        found = usb_hub_probe(d, config, accept);
    } else if (msc) {
        kprintd("usb: port %s: %s-speed mass storage %4x:%4x\n", usb_path(d), usb_speed_name(d->speed), vid, pid);
        found = usb_msc_probe(d, config, iface, in, in_mps, in_burst, out, out_mps, out_burst, accept);
    } else {
        kprintd("usb: port %s: %s-speed device %4x:%4x, skipped\n", usb_path(d), usb_speed_name(d->speed), vid, pid);
    }
    if (!found) usb_release(d);
    return found;
}

/* -------------------------------------------------------------------------- */
/* Поиск контроллеров на PCI                                                  */
/* -------------------------------------------------------------------------- */
#define PCI_PROG_EHCI           0x20
#define PCI_PROG_XHCI           0x30

int usb_init(int (*accept)(blockdev_t* dev)) {
    if (usb_ready) return 1;
    int found[2] = { 0, 0 };                     /* xHCI, EHCI */

    /* Два прохода: сначала все xHCI, потом все EHCI (см. начало файла). */
    for (int pass = 0; pass < 2; pass++) {
        uint8_t want = pass == 0 ? PCI_PROG_XHCI : PCI_PROG_EHCI;
        for (uint16_t bus = 0; bus < 256; bus++) {
            for (uint8_t dev = 0; dev < 32; dev++) {
                uint32_t reg0 = pci_read_config_32((uint8_t)bus, dev, 0, 0x00);
                if ((reg0 & 0xFFFF) == 0xFFFF) continue;

                uint32_t regC = pci_read_config_32((uint8_t)bus, dev, 0, 0x0C);
                uint8_t header_type = (regC >> 16) & 0xFF;
                int max_func = (header_type & 0x80) ? 8 : 1;

                for (uint8_t fn = 0; fn < max_func; fn++) {
                    uint32_t r0 = pci_read_config_32((uint8_t)bus, dev, fn, 0x00);
                    if ((r0 & 0xFFFF) == 0xFFFF) continue;

                    uint32_t r8 = pci_read_config_32((uint8_t)bus, dev, fn, 0x08);
                    uint8_t cls  = (r8 >> 24) & 0xFF;
                    uint8_t sub  = (r8 >> 16) & 0xFF;
                    uint8_t prog = (r8 >> 8)  & 0xFF;
                    if (cls != 0x0C || sub != 0x03 || prog != want) continue;

                    found[pass]++;
                    int ok = (pass == 0) ? xhci_probe((uint8_t)bus, dev, fn, accept)
                                         : ehci_probe((uint8_t)bus, dev, fn, accept);
                    if (ok) return 1;
                }
            }
        }
    }

    if (!found[0] && !found[1])
        kprintd("usb: no xHCI/EHCI controller found on PCI\n");
    else
        kprintd("usb: no usable USB mass storage (xHCI: %d, EHCI: %d controllers)\n", found[0], found[1]);
    return 0;
}
