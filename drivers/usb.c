/*
 * drivers/usb.c — 32-битный драйвер USB 2.0: EHCI + USB-хабы + USB Mass Storage (BOT / SCSI)
 * Работает полностью в защищённом режиме через DMA и MMIO, без прерываний (опрос).
 *
 * Что умеет:
 *   - перебирает ВСЕ EHCI-контроллеры на PCI (класс 0C/03/20);
 *   - забирает контроллер у BIOS по правилам EHCI (USBLEGSUP handoff);
 *   - USB-хабы: USB 2.0 (High-Speed) и USB 1.1 (Full-Speed), цепочки до 5
 *     хабов. В том числе встроенный «rate matching hub» (RMH) чипсетов Intel
 *     5-9 серий: там к корневому порту EHCI подключён только он, а все
 *     разъёмы платы — к нему;
 *   - High-Speed флешки на любом порту (корневом или хаба) и Full-Speed
 *     (USB 1.1) флешки за High-Speed хабом — через его transaction translator
 *     (split-транзакции, EHCI 4.12, их делает сам контроллер);
 *   - Full/Low-Speed устройства прямо в корневом порту отдаются companion-
 *     контроллеру UHCI/OHCI, как это делает BIOS; Low-Speed устройства за
 *     хабами (клавиатуры, мыши) драйвер не трогает вовсе;
 *   - если на контроллере не нашлось нужной флешки, он останавливается, а
 *     его порты отдаются companion-контроллерам, если они есть (как при
 *     BIOS). Обратно BIOS контроллер НЕ возвращается — так же, как в Linux и Windows: после
 *     сброса BIOS его обычно заново не опрашивает (USB-клавиатуре это всё
 *     равно не помогает), а SMM-обработчик BIOS на таком редком пути может
 *     повесить машину;
 *
 * Чего (пока) нет: xHCI (USB 3.x). На платах, где порты обслуживает только
 * xHCI (Intel 100-й серии и новее, AMD Ryzen, Intel 7-9 серий в режиме xHCI),
 * флешка ядру не видна, и оно работает с копией образа в ОЗУ (rd0), которую
 * сделал загрузчик. Драйвера HID тоже нет: USB-клавиатура на том же
 * EHCI-контроллере, что и флешка, после захвата контроллера перестаёт работать.
 *
 * ВСЕ ожидания ограничены по времени: если контроллер или устройство ведут
 * себя не так, как ожидается, драйвер сдаётся, и загрузка продолжается.
 */

#include <drivers/usb.h>
#include <drivers/blockdev.h>
#include <drivers/pci.h>
#include <cpu/ports.h>
#include <cpu/vmm.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>

extern uint32_t page_directory[1024];
extern void vmm_identity_map_page(uint32_t* pd, uintptr_t phys_addr, uint32_t flags);

/* ---- стандартные запросы USB --------------------------------------------- */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_CLEAR_TT_BUFFER 0x08   /* класс hub (USB 2.0, 11.24.2.3) */
#define USB_REQ_BOT_RESET       0xFF   /* Bulk-Only Mass Storage Reset (class) */

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HUB            0x29

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09
#define USB_PROTO_BOT           0x50

/* bmRequestType */
#define RT_STD_IN               0x80   /* стандартный, к устройству, IN */
#define RT_STD_OUT              0x00
#define RT_STD_EP_OUT           0x02   /* стандартный, к endpoint'у */
#define RT_CLASS_IFACE_OUT      0x21
#define RT_HUB_IN               0xA0   /* класс hub, к самому хабу */
#define RT_PORT_IN              0xA3   /* класс hub, к порту хаба */
#define RT_PORT_OUT             0x23

/* Скорости — в кодировке поля EPS у QH (EHCI 3.6.2) */
#define USB_SPEED_FULL          0
#define USB_SPEED_LOW           1
#define USB_SPEED_HIGH          2

/* ---- хабы (USB 2.0, глава 11.24) ------------------------------------------ */
#define HUB_FEAT_PORT_ENABLE    1
#define HUB_FEAT_PORT_RESET     4
#define HUB_FEAT_PORT_POWER     8
#define HUB_FEAT_C_CONNECTION   16     /* C_PORT_*: 16 + номер бита в wPortChange */

/* wPortStatus */
#define HPS_CONNECTION          (1u << 0)
#define HPS_ENABLE              (1u << 1)
#define HPS_OVERCURRENT         (1u << 3)
#define HPS_RESET               (1u << 4)
#define HPS_LOW_SPEED           (1u << 9)
#define HPS_HIGH_SPEED          (1u << 10)
/* wPortChange: биты 0..4 = C_CONNECTION, C_ENABLE, C_SUSPEND, C_OVER_CURRENT, C_RESET */
#define HPC_ALL                 0x1Fu

#define USB_MAX_DEVICES         32     /* устройств на одном контроллере */
#define USB_MAX_HUB_DEPTH       5      /* до 5 хабов между контроллером и устройством (USB 2.0, 4.1.1) */
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

/* ---- регистры EHCI ---------------------------------------------------------- */
/* capability registers (от начала BAR0) */
#define EHCI_CAPLENGTH          0x00
#define EHCI_HCSPARAMS          0x04
#define EHCI_HCCPARAMS          0x08
/* operational registers (от BAR0 + CAPLENGTH) */
#define EHCI_USBCMD             0x00
#define EHCI_USBSTS             0x04
#define EHCI_USBINTR            0x08
#define EHCI_CTRLDSSEGMENT      0x10
#define EHCI_PERIODICLISTBASE   0x14
#define EHCI_ASYNCLISTADDR      0x18
#define EHCI_CONFIGFLAG         0x40
#define EHCI_PORTSC(n)          (0x44 + 4 * (n))

#define CMD_RS                  (1u << 0)   /* Run/Stop */
#define CMD_HCRESET             (1u << 1)
#define CMD_PSE                 (1u << 4)   /* Periodic Schedule Enable */
#define CMD_ASE                 (1u << 5)   /* Asynchronous Schedule Enable */
#define CMD_ITC_8               (8u << 16)  /* порог прерываний: 8 микрокадров (по умолчанию) */

/* ВНИМАНИЕ: биты USBSTS не совпадают с битами USBCMD! Состояние асинхронного
 * расписания — бит 15 (ASS), а бит 5 — это «Interrupt on Async Advance»,
 * который взводится только после звонка в doorbell (USBCMD бит 6). Именно
 * ожидание бита 5 вместо бита 15 и вешало загрузку навсегда. */
#define STS_HSE                 (1u << 4)   /* Host System Error */
#define STS_RWC_MASK            0x3Fu       /* биты 0..5 сбрасываются записью 1 */
#define STS_HALTED              (1u << 12)  /* HCHalted */
#define STS_ASS                 (1u << 15)  /* Asynchronous Schedule Status */

#define PORT_CCS                (1u << 0)   /* устройство подключено */
#define PORT_CSC                (1u << 1)
#define PORT_PE                 (1u << 2)   /* порт включён (только High-Speed) */
#define PORT_PEC                (1u << 3)
#define PORT_OCC                (1u << 5)
#define PORT_RESET              (1u << 8)
#define PORT_LS_MASK            (3u << 10)
#define PORT_LS_K               (1u << 10)  /* K-state: Low-Speed устройство */
#define PORT_PP                 (1u << 12)  /* питание порта */
#define PORT_OWNER              (1u << 13)  /* 1 = порт у companion-контроллера */
#define PORT_RWC                (PORT_CSC | PORT_PEC | PORT_OCC)

#define HCS_N_PORTS(x)          ((x) & 0x0F)
#define HCS_PPC                 (1u << 4)
#define HCS_N_CC(x)             (((x) >> 12) & 0x0F)
#define HCC_EECP(x)             (((x) >> 8) & 0xFF)

/* USBLEGSUP / USBLEGCTLSTS — в конфигурационном пространстве PCI по смещению EECP */
#define LEGSUP_CAP_ID           0x01
#define LEGSUP_BIOS_OWNED       (1u << 16)
#define LEGSUP_OS_OWNED         (1u << 24)

/* ---- структуры расписания --------------------------------------------------- */
#define LINK_TERMINATE          1u
#define LINK_TYPE_QH            (1u << 1)

#define QTD_ACTIVE              (1u << 7)
#define QTD_HALTED              (1u << 6)
#define QTD_BUFERR              (1u << 5)
#define QTD_BABBLE              (1u << 4)
#define QTD_XACTERR             (1u << 3)
#define QTD_MISSED              (1u << 2)
#define QTD_ERRORS              (QTD_HALTED | QTD_BUFERR | QTD_BABBLE | QTD_XACTERR | QTD_MISSED)
#define QTD_PID_OUT             (0u << 8)
#define QTD_PID_IN              (1u << 8)
#define QTD_PID_SETUP           (2u << 8)
#define QTD_CERR_3              (3u << 10)
#define QTD_IOC                 (1u << 15)
#define QTD_BYTES(n)            (((uint32_t)(n) & 0x7FFF) << 16)
#define QTD_BYTES_LEFT(tok)     (((tok) >> 16) & 0x7FFF)
#define QTD_TOGGLE              (1u << 31)

#define QH_EPS(speed)           ((uint32_t)(speed) << 12)
#define QH_DTC                  (1u << 14)  /* toggle берётся из qTD (нужно для control) */
#define QH_HEAD                 (1u << 15)  /* H: голова async-списка (Head of Reclamation List) */
#define QH_CTRL_EP              (1u << 27)  /* C: control endpoint Full/Low-Speed устройства */
#define QH_HUB_ADDR(a)          ((uint32_t)((a) & 0x7F) << 16)  /* хаб с transaction translator */
#define QH_HUB_PORT(p)          ((uint32_t)((p) & 0x7F) << 23)  /* ... и его порт */
#define QH_MULT_1               (1u << 30)

/* qTD (EHCI 3.5). У контроллеров с 64-битной адресацией (HCCPARAMS бит 0 —
 * это почти все настоящие Intel/AMD) после buffer[5] идут ещё 5 dword: старшие
 * половины адресов буферов. Контроллер читает их ВСЕГДА, поэтому они должны
 * быть частью структуры и содержать нули (раньше там оказывался соседний qTD,
 * и контроллер лез по DMA за пределы 4 ГБ). Для 32-битных контроллеров они
 * просто не используются. */
typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t buffer_hi[5];
    uint32_t          pad[3];
} ehci_qtd_t;                       /* 64 байта */

/* QH (EHCI 3.6) с тем же 64-битным хвостом в области overlay. */
typedef struct __attribute__((packed, aligned(64))) {
    volatile uint32_t qhlp;
    volatile uint32_t ep_char;
    volatile uint32_t ep_cap;
    volatile uint32_t current_qtd;
    /* overlay — копия текущего qTD */
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t buffer_hi[5];
    uint32_t          pad[3];
} ehci_qh_t;

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

/* Результаты передачи */
#define XFER_OK                 0
#define XFER_STALL             (-1)   /* устройство ответило STALL */
#define XFER_ERROR             (-2)   /* ошибка на шине (CRC, babble, ...) */
#define XFER_TIMEOUT           (-3)
#define XFER_DEAD              (-4)   /* контроллер перестал работать */

#define QH_CONTROL              0
#define QH_BULK_OUT             1
#define QH_BULK_IN              2

#define CTRL_TIMEOUT_MS         500
#define BULK_TIMEOUT_MS         3000

/* Ядро работает с тождественным отображением (виртуальный = физический адрес),
 * поэтому адреса этих статических массивов можно отдавать контроллеру. */
static ehci_qh_t  qh_pool[4]        __attribute__((aligned(4096)));
static ehci_qtd_t qtd_pool[8]       __attribute__((aligned(4096)));
static uint8_t    dma_buffer[4096]  __attribute__((aligned(4096)));
static uint8_t    setup_pkt[8]      __attribute__((aligned(32)));

/* Кэш упреждающего чтения: ядро читает ФС по одному сектору, а одна
 * SCSI-команда по USB стоит 3 передачи. Читаем сразу 8 секторов (4 КБ). */
#define USB_CACHE_SECTORS       8
static uint8_t    usb_cache[USB_CACHE_SECTORS * 512];
static uint32_t   usb_cache_lba = 0;
static uint32_t   usb_cache_count = 0;  /* 0 = кэш пуст */

/* ---- состояние ---------------------------------------------------------------- */
typedef struct {
    uint8_t  bus, dev, fn;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    uint32_t n_ports;
    uint32_t n_cc;               /* число companion-контроллеров (UHCI/OHCI) */
} ehci_ctrl_t;

static ehci_ctrl_t hc;
static int hc_dead = 0;          /* контроллер не отвечает — все передачи сразу с ошибкой */

/* Устройство на шине. Для Full/Low-Speed устройств за High-Speed хабом
 * контроллер сам делает split-транзакции через transaction translator (TT)
 * этого хаба — ему нужно знать адрес хаба и номер порта (EHCI 4.12). */
typedef struct usb_dev {
    struct usb_dev* parent;      /* хаб, в который включено устройство; 0 — корневой порт */
    struct usb_dev* tt;          /* Full/Low-Speed за High-Speed хабом: этот хаб (его TT) */
    uint8_t  port;               /* номер порта: корневого (с 0) или хаба (с 1) */
    uint8_t  depth;              /* сколько хабов между контроллером и устройством */
    uint8_t  speed;              /* USB_SPEED_* */
    uint8_t  addr;               /* адрес на шине; 0 — ещё не назначен */
    uint8_t  mps0;               /* максимальный размер пакета EP0 */
    uint8_t  tt_hub;             /* Full/Low-Speed: адрес High-Speed хаба с TT */
    uint8_t  tt_port;            /*                 и порт этого хаба */
} usb_dev_t;

static usb_dev_t usb_devs[USB_MAX_DEVICES];
static int       usb_ndevs = 0;
static int       usb_identified = 0;    /* опознанных устройств на контроллере (для сообщений) */
static uint8_t   next_dev_addr = 1;

/* дескрипторы последнего опрошенного устройства */
static uint8_t   dev_desc[18];
static uint8_t   cfg_desc[512];
static uint16_t  cfg_len = 0;

static usb_dev_t* msc_dev = 0;   /* флешка, с которой работает usb0 */
static uint8_t  usb_iface = 0;
static uint8_t  ep_in = 0, ep_out = 0;
static uint16_t ep_in_mps = 512, ep_out_mps = 512;
static uint32_t usb_total_sectors = 0;
static char     usb_model[41] = "USB Flash Drive";
static uint32_t bot_tag_counter = 0x1000;
static int      usb_ready = 0;   /* usb0 зарегистрирован и работает */
static int      usb_failures = 0;/* подряд неудачных обращений к usb0 */

/* ---- задержки ------------------------------------------------------------------- */
/* ~1 мкс: чтение порта 0x80 идёт через медленную шину LPC/ISA. Не зависит от
 * прерываний и таймера, поэтому работает и внутри системного вызова. */
static void udelay(uint32_t us) {
    while (us--) port_byte_in(0x80);
}

static void mdelay(uint32_t ms) {
    while (ms--) udelay(1000);
}

/* ---- доступ к регистрам ---------------------------------------------------------- */
static uint32_t op_read(uint32_t reg) {
    return *(volatile uint32_t*)(hc.op + reg);
}

static void op_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(hc.op + reg) = val;
}

/* Ждёт (reg & mask) == want не дольше timeout_us. 0 — дождались. */
static int op_wait(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_us) {
    for (;;) {
        if ((op_read(reg) & mask) == want) return 0;
        if (timeout_us == 0) return -1;
        udelay(1);
        timeout_us--;
    }
}

/* Байтовая запись в конфигурационное пространство PCI (в drivers/pci.c есть
 * только 32-битная, а семафоры USBLEGSUP принято трогать по байту). */
static void pci_write_config_8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    port_byte_out(PCI_CONFIG_DATA + (off & 3), val);
}

static int ehci_fail(const char* why) {
    if (!hc_dead) kprintw("usb: EHCI %x:%x.%x: %s, controller disabled\n", hc.bus, hc.dev, hc.fn, why);
    hc_dead = 1;
    return XFER_DEAD;
}

/* Включить/выключить асинхронное расписание и дождаться, пока контроллер
 * это подтвердит (USBSTS.ASS догоняет USBCMD.ASE, EHCI 4.8). */
static int ehci_async(int on) {
    uint32_t cmd = op_read(EHCI_USBCMD);
    if (on) cmd |= CMD_ASE | CMD_RS;
    else    cmd &= ~CMD_ASE;
    op_write(EHCI_USBCMD, cmd);
    return op_wait(EHCI_USBSTS, STS_ASS, on ? STS_ASS : 0, 100000);
}

/* -------------------------------------------------------------------------- */
/* Выполнение одной цепочки qTD на QH (QH — единственный в async-списке)       */
/* -------------------------------------------------------------------------- */
static int ehci_run(ehci_qh_t* qh, ehci_qtd_t* first, ehci_qtd_t* last, uint32_t timeout_ms) {
    if (hc_dead) return XFER_DEAD;

    if ((op_read(EHCI_USBCMD) & CMD_ASE) || (op_read(EHCI_USBSTS) & STS_ASS)) {
        if (ehci_async(0) != 0) return ehci_fail("async schedule does not stop");
    }
    op_write(EHCI_USBSTS, STS_RWC_MASK);

    /* Готовим overlay: со старой передачи сохраняем только data toggle. */
    qh->current_qtd = 0;
    qh->next_qtd = (uint32_t)first;
    qh->alt_next_qtd = LINK_TERMINATE;
    qh->token &= QTD_TOGGLE;
    for (int i = 0; i < 5; i++) {
        qh->buffer[i] = 0;
        qh->buffer_hi[i] = 0;
    }

    op_write(EHCI_ASYNCLISTADDR, (uint32_t)qh);
    if (ehci_async(1) != 0) return ehci_fail("async schedule does not start");

    int res = XFER_TIMEOUT;
    for (uint32_t us = timeout_ms * 1000; us; us--) {
        uint32_t tok = last->token;
        if (!(tok & QTD_ACTIVE)) {
            if (!(tok & QTD_ERRORS))                               res = XFER_OK;
            else if (tok & (QTD_BABBLE | QTD_BUFERR | QTD_XACTERR)) res = XFER_ERROR;
            else if (tok & QTD_HALTED)                             res = XFER_STALL;
            else                                                   res = XFER_ERROR;
            break;
        }
        /* одна из предыдущих qTD остановила очередь (STALL на SETUP/DATA) */
        uint32_t qtok = qh->token;
        if ((qtok & QTD_HALTED) && !(qtok & QTD_ACTIVE)) {
            res = (qtok & (QTD_BABBLE | QTD_BUFERR | QTD_XACTERR)) ? XFER_ERROR : XFER_STALL;
            break;
        }
        uint32_t sts = op_read(EHCI_USBSTS);
        if (sts & STS_HSE) return ehci_fail("host system error");
        if (sts & STS_HALTED) return ehci_fail("controller halted");
        udelay(1);
    }

    if (ehci_async(0) != 0) return ehci_fail("async schedule does not stop");
    op_write(EHCI_USBSTS, STS_RWC_MASK);
    return res;
}

/* -------------------------------------------------------------------------- */
/* QH для endpoint'а устройства                                               */
/* -------------------------------------------------------------------------- */
static void qh_setup(ehci_qh_t* qh, const usb_dev_t* d, uint8_t ep, uint16_t mps, int control) {
    memset((void*)qh, 0, sizeof(ehci_qh_t));
    qh->qhlp = ((uint32_t)qh) | LINK_TYPE_QH;
    /* H = 1: каждый QH стоит в async-списке один (ссылается сам на себя), и
     * без головы списка QEMU вообще не обрабатывает очередь. */
    qh->ep_char = (d->addr & 0x7F) | ((uint32_t)(ep & 0x0F) << 8) | QH_EPS(d->speed) | QH_HEAD |
                  ((uint32_t)(mps & 0x7FF) << 16);
    if (control) {
        qh->ep_char |= QH_DTC;                   /* toggle берётся из qTD */
        if (d->speed != USB_SPEED_HIGH) qh->ep_char |= QH_CTRL_EP;
    }
    qh->ep_cap = QH_MULT_1;
    if (d->speed != USB_SPEED_HIGH)              /* split-транзакции через TT хаба */
        qh->ep_cap |= QH_HUB_ADDR(d->tt_hub) | QH_HUB_PORT(d->tt_port);
    qh->next_qtd = LINK_TERMINATE;
    qh->alt_next_qtd = LINK_TERMINATE;
}

static void usb_clear_tt_buffer(usb_dev_t* d, uint8_t ep, int bulk, int is_in);

/* -------------------------------------------------------------------------- */
/* Control Transfer (Endpoint 0)                                              */
/* -------------------------------------------------------------------------- */
static int usb_control_xfer(usb_dev_t* d, uint8_t req_type, uint8_t req, uint16_t val,
                            uint16_t idx, uint16_t len, void* data, uint16_t* actual) {
    if (actual) *actual = 0;
    if (len > sizeof(dma_buffer)) return XFER_ERROR;
    int is_in = (req_type & 0x80) != 0;

    setup_pkt[0] = req_type;
    setup_pkt[1] = req;
    setup_pkt[2] = val & 0xFF;
    setup_pkt[3] = (val >> 8) & 0xFF;
    setup_pkt[4] = idx & 0xFF;
    setup_pkt[5] = (idx >> 8) & 0xFF;
    setup_pkt[6] = len & 0xFF;
    setup_pkt[7] = (len >> 8) & 0xFF;

    ehci_qh_t* qh = &qh_pool[QH_CONTROL];
    qh_setup(qh, d, 0, d->mps0, 1);

    ehci_qtd_t* qtd_setup = &qtd_pool[0];
    ehci_qtd_t* qtd_data  = &qtd_pool[1];
    ehci_qtd_t* qtd_stat  = &qtd_pool[2];
    memset((void*)qtd_pool, 0, 3 * sizeof(ehci_qtd_t));

    qtd_setup->next_qtd = (len > 0) ? (uint32_t)qtd_data : (uint32_t)qtd_stat;
    qtd_setup->alt_next_qtd = LINK_TERMINATE;
    qtd_setup->token = QTD_ACTIVE | QTD_PID_SETUP | QTD_CERR_3 | QTD_BYTES(8);
    qtd_setup->buffer[0] = (uint32_t)setup_pkt;

    if (len > 0) {
        if (data && !is_in) memcpy(dma_buffer, data, len);
        /* короткий пакет в стадии данных: alt_next = T, контроллер сразу
         * переходит к статус-стадии (EHCI 4.10.2) */
        qtd_data->next_qtd = (uint32_t)qtd_stat;
        qtd_data->alt_next_qtd = LINK_TERMINATE;
        qtd_data->token = QTD_ACTIVE | (is_in ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR_3 |
                          QTD_TOGGLE | QTD_BYTES(len);
        qtd_data->buffer[0] = (uint32_t)dma_buffer;
    }

    /* статус-стадия идёт в обратную сторону (без данных — всегда IN), DATA1 */
    qtd_stat->next_qtd = LINK_TERMINATE;
    qtd_stat->alt_next_qtd = LINK_TERMINATE;
    qtd_stat->token = QTD_ACTIVE | ((len > 0 && is_in) ? QTD_PID_OUT : QTD_PID_IN) |
                      QTD_CERR_3 | QTD_TOGGLE | QTD_IOC;

    int res = ehci_run(qh, qtd_setup, qtd_stat, CTRL_TIMEOUT_MS);
    if (res == XFER_OK && len > 0) {
        uint32_t got = len - QTD_BYTES_LEFT(qtd_data->token);
        if (got > len) got = len;
        if (is_in && data) memcpy(data, dma_buffer, got);
        if (actual) *actual = (uint16_t)got;
    }
    if (res == XFER_TIMEOUT || res == XFER_ERROR)
        usb_clear_tt_buffer(d, 0, 0, is_in);
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
    if (!d || !d->tt || d->speed == USB_SPEED_HIGH || hc_dead) return;
    uint16_t info = (uint16_t)((ep & 0x0F) | ((d->addr & 0x7F) << 4) | ((bulk ? 2 : 0) << 11));
    /* хабы у нас всегда в режиме одного TT (alt setting 0), поэтому wIndex = 1 */
    if (bulk) {
        usb_control_msg(d->tt, RT_PORT_OUT, USB_REQ_CLEAR_TT_BUFFER, info | (is_in ? 0x8000 : 0), 1, 0, 0);
    } else {                                     /* control: оба направления */
        usb_control_msg(d->tt, RT_PORT_OUT, USB_REQ_CLEAR_TT_BUFFER, info | 0x8000, 1, 0, 0);
        usb_control_msg(d->tt, RT_PORT_OUT, USB_REQ_CLEAR_TT_BUFFER, info, 1, 0, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* Bulk Transfer — data toggle ведёт контроллер в overlay QH (DTC = 0)         */
/* -------------------------------------------------------------------------- */
static int usb_bulk_transfer(int is_in, void* data, uint32_t len, uint32_t* actual) {
    if (len > sizeof(dma_buffer)) return XFER_ERROR;
    if (!is_in && data && len) memcpy(dma_buffer, data, len);

    ehci_qh_t* qh = &qh_pool[is_in ? QH_BULK_IN : QH_BULK_OUT];
    ehci_qtd_t* qtd = &qtd_pool[3];
    memset((void*)qtd, 0, sizeof(ehci_qtd_t));
    qtd->next_qtd = LINK_TERMINATE;
    qtd->alt_next_qtd = LINK_TERMINATE;
    qtd->token = QTD_ACTIVE | (is_in ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR_3 | QTD_IOC | QTD_BYTES(len);
    qtd->buffer[0] = (uint32_t)dma_buffer;

    int res = ehci_run(qh, qtd, qtd, BULK_TIMEOUT_MS);
    uint32_t done = len - QTD_BYTES_LEFT(qtd->token);
    if (done > len) done = len;
    if (actual) *actual = done;
    if (res == XFER_OK && is_in && data && done) {
        memcpy(data, dma_buffer, done);
    }
    if (res == XFER_TIMEOUT || res == XFER_ERROR)
        usb_clear_tt_buffer(msc_dev, is_in ? ep_in : ep_out, 1, is_in);
    return res;
}

static void bulk_qh_init(ehci_qh_t* qh, uint8_t ep, uint16_t mps) {
    qh_setup(qh, msc_dev, ep, mps, 0);           /* DTC = 0: toggle ведёт контроллер */
}

/* CLEAR_FEATURE(ENDPOINT_HALT): снимает STALL, toggle на обеих сторонах -> DATA0 */
static void usb_clear_halt(int is_in) {
    uint8_t ep = is_in ? (uint8_t)(ep_in | 0x80) : ep_out;
    usb_control_msg(msc_dev, RT_STD_EP_OUT, USB_REQ_CLEAR_FEATURE, 0, ep, 0, 0);
    qh_pool[is_in ? QH_BULK_IN : QH_BULK_OUT].token &= ~QTD_TOGGLE;
}

/* Reset Recovery из спецификации BOT 5.3.4 */
static void bot_reset_recovery(void) {
    usb_control_msg(msc_dev, RT_CLASS_IFACE_OUT, USB_REQ_BOT_RESET, 0, usb_iface, 0, 0);
    mdelay(10);
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

    for (int attempt = 0; attempt < 2 && !hc_dead; attempt++) {
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
    if (!usb_ready || hc_dead) return 1;

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
    if (++usb_failures >= 3 || hc_dead) {
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

static const char* speed_name(uint8_t speed) {
    return speed == USB_SPEED_HIGH ? "high" : speed == USB_SPEED_LOW ? "low" : "full";
}

/* «Адрес» устройства для сообщений: корневой порт, затем порты хабов — 0.3.2 */
static const char* usb_path(const usb_dev_t* d) {
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

static usb_dev_t* usb_alloc_dev(usb_dev_t* parent, uint8_t port, uint8_t speed) {
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
    if (speed != USB_SPEED_HIGH && parent) {
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

/* Назначает адрес только что сброшенному устройству (оно отвечает по адресу 0)
 * и читает его дескрипторы в dev_desc / cfg_desc. 0 — успех. */
static int usb_enumerate(usb_dev_t* d) {
    d->addr = 0;
    d->mps0 = (d->speed == USB_SPEED_HIGH) ? 64 : 8;

    /* 1. Первые 8 байт дескриптора устройства по адресу 0 (как это делает
     *    Windows: некоторые флешки иначе не принимают SET_ADDRESS). */
    memset(dev_desc, 0, sizeof(dev_desc));
    int r = XFER_ERROR;
    for (int attempt = 0; attempt < 3 && !hc_dead; attempt++) {
        r = usb_control_msg(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, 8, dev_desc);
        if (r == XFER_OK) break;
        mdelay(20);
    }
    if (r != XFER_OK) return -1;
    uint8_t mps = dev_desc[7];
    if (d->speed == USB_SPEED_FULL && (mps == 8 || mps == 16 || mps == 32 || mps == 64))
        d->mps0 = mps;                           /* High-Speed — всегда 64, Low-Speed — 8 */

    if (next_dev_addr > 127) return -1;
    uint8_t addr = next_dev_addr++;
    if (usb_control_msg(d, RT_STD_OUT, USB_REQ_SET_ADDRESS, addr, 0, 0, 0) != XFER_OK) return -1;
    mdelay(10);                                  /* SET_ADDRESS recovery: >= 2 мс */
    d->addr = addr;

    if (usb_control_msg(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, 18, dev_desc) != XFER_OK)
        return -1;

    /* 2. Конфигурация: сначала 9 байт ради wTotalLength, потом целиком. */
    cfg_len = 0;
    memset(cfg_desc, 0, sizeof(cfg_desc));
    if (usb_control_msg(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, 9, cfg_desc) != XFER_OK)
        return -1;
    uint16_t total = cfg_desc[2] | (cfg_desc[3] << 8);
    if (total < 9) return -1;
    if (total > sizeof(cfg_desc)) total = sizeof(cfg_desc);
    uint16_t got = 0;
    if (usb_control_xfer(d, RT_STD_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, total, cfg_desc, &got) != XFER_OK)
        return -1;
    cfg_len = got < total ? got : total;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* USB Mass Storage                                                           */
/* -------------------------------------------------------------------------- */
/* Запускает флешку и, если accept согласен, регистрирует её как usb0.
 * 1 — usb0 зарегистрирован. */
static int usb_msc_probe(usb_dev_t* d, uint8_t config, uint8_t iface,
                         uint8_t in, uint16_t in_mps, uint8_t out, uint16_t out_mps,
                         int (*accept)(blockdev_t*)) {
    usb_ready = 0;
    usb_cache_count = 0;
    msc_dev = d;
    usb_iface = iface;
    ep_in = in;
    ep_out = out;
    uint16_t def_mps = (d->speed == USB_SPEED_HIGH) ? 512 : 64;
    ep_in_mps = in_mps ? in_mps : def_mps;
    ep_out_mps = out_mps ? out_mps : def_mps;
    memcpy(usb_model, "USB Flash Drive", sizeof("USB Flash Drive"));

    if (usb_control_msg(d, RT_STD_OUT, USB_REQ_SET_CONFIG, config, 0, 0, 0) != XFER_OK)
        return 0;
    mdelay(10);

    /* После SET_CONFIGURATION toggle у bulk-endpoint'ов = DATA0 */
    bulk_qh_init(&qh_pool[QH_BULK_OUT], ep_out, ep_out_mps);
    bulk_qh_init(&qh_pool[QH_BULK_IN],  ep_in,  ep_in_mps);

    /* Ждём готовности носителя (после включения флешка отвечает
     * «UNIT ATTENTION», пока у неё не спросят sense-данные). Пустой
     * картридер (их много в ноутбуках) и не отвечающее устройство бросаем
     * сразу, а не через 20 попыток. */
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    int ready = 0, no_medium = 0, dead = 0;
    for (int i = 0; i < 20 && !hc_dead; i++) {
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
        mdelay(100);
    }
    if (hc_dead) return 0;
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
static int usb_attach(usb_dev_t* d, int (*accept)(blockdev_t*));

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
    for (int attempt = 0; attempt < 3 && !hc_dead; attempt++) {
        if (hub_port_feature(hub, port, 1, HUB_FEAT_PORT_RESET) != XFER_OK) return -1;
        uint16_t st = 0, ch = 0;
        int done = 0;
        /* хаб держит reset 10..20 мс, для High-Speed ещё и chirp (USB 2.0 7.1.7.5) */
        for (uint32_t ms = 0; ms < HUB_RESET_TIMEOUT_MS; ms += 10) {
            mdelay(10);
            if (hub_port_status(hub, port, &st, &ch) != 0) return -1;
            if (!(st & HPS_RESET)) { done = 1; break; }
        }
        hub_clear_changes(hub, port, ch & HPC_ALL);
        if (!(st & HPS_CONNECTION)) return -1;  /* устройство отключили */
        if (done && (st & HPS_ENABLE)) {
            *speed = (st & HPS_LOW_SPEED) ? USB_SPEED_LOW :
                     (st & HPS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
            mdelay(10);                          /* TRSTRCY */
            return 0;
        }
        mdelay(50);
    }
    return -1;
}

/* Устройство подключено к порту хаба: сброс, адрес, опознание. 1 — usb0 найден. */
static int hub_port_probe(usb_dev_t* hub, uint8_t port, int (*accept)(blockdev_t*)) {
    for (int attempt = 0; attempt < 2 && !hc_dead; attempt++) {
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
        kprintd("usb: port %s: %s-speed device does not answer\n", usb_path(d), speed_name(speed));
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
        for (uint8_t p = 1; p <= nports && !hc_dead; p++) {
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
                mdelay(100);
                debounced = 1;
            }
            if (hub_port_probe(hub, p, accept)) return 1;
        }
        if (!pending || hc_dead || waited >= HUB_CONNECT_WINDOW_MS) break;
        mdelay(HUB_POLL_MS);
        waited += HUB_POLL_MS;
    }
    return 0;
}

static int usb_hub_probe(usb_dev_t* hub, uint8_t config, int (*accept)(blockdev_t*)) {
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
    uint32_t pgood_ms = (uint32_t)hd[5] * 2;     /* bPwrOn2PwrGood, в 2-мс единицах */
    if (nports == 0) return 0;
    if (nports > HUB_MAX_PORTS) {
        kprintw("usb: port %s: hub has %d ports, only %d are used\n", usb_path(hub), nports, HUB_MAX_PORTS);
        nports = HUB_MAX_PORTS;
    }
    kprintd("usb: port %s: %d-port %s-speed hub\n", usb_path(hub), nports, speed_name(hub->speed));

    /* Питание портов (хабы без управления питанием эту команду просто
     * принимают) и ожидание его стабилизации: не меньше 100 мс. */
    for (uint8_t p = 1; p <= nports; p++) hub_port_feature(hub, p, 1, HUB_FEAT_PORT_POWER);
    mdelay(pgood_ms > 100 ? pgood_ms : 100);

    return hub_scan_ports(hub, nports, accept);
}

/* -------------------------------------------------------------------------- */
/* Опознание устройства с уже назначенным адресом (после usb_enumerate):      */
/* хаб — обходим его порты, флешка — пробуем как usb0.                         */
/* 1 — usb0 найден и зарегистрирован                                           */
/* -------------------------------------------------------------------------- */
static int usb_attach(usb_dev_t* d, int (*accept)(blockdev_t*)) {
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
    uint8_t iface = 0, in = 0, out = 0;
    uint16_t in_mps = 0, out_mps = 0;
    for (uint16_t off = 0; off + 2 <= cfg_len; ) {
        uint8_t len  = cfg_desc[off];
        uint8_t type = cfg_desc[off + 1];
        if (len < 2 || off + len > cfg_len) break;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (in_msc && in && out) msc = 1;
            in_msc = 0;
            if (cfg_desc[off + 3] == 0) {        /* alt setting 0 */
                if (cfg_desc[off + 5] == USB_CLASS_HUB) is_hub = 1;
                if (!msc && cfg_desc[off + 5] == USB_CLASS_MASS_STORAGE && cfg_desc[off + 7] == USB_PROTO_BOT) {
                    in_msc = 1;
                    iface = cfg_desc[off + 2];
                    in = out = 0;
                }
            }
        } else if (type == USB_DESC_ENDPOINT && len >= 7 && in_msc) {
            uint8_t  ep_addr = cfg_desc[off + 2];
            uint16_t mps = (cfg_desc[off + 4] | (cfg_desc[off + 5] << 8)) & 0x7FF;
            if ((cfg_desc[off + 3] & 0x03) == 0x02) {    /* bulk */
                if (ep_addr & 0x80) { in  = ep_addr & 0x0F; in_mps  = mps; }
                else                { out = ep_addr & 0x0F; out_mps = mps; }
            }
        }
        off += len;
    }
    if (in_msc && in && out) msc = 1;

    if (is_hub) return usb_hub_probe(d, config, accept);
    if (msc) {
        kprintd("usb: port %s: %s-speed mass storage %4x:%4x\n", usb_path(d), speed_name(d->speed), vid, pid);
        return usb_msc_probe(d, config, iface, in, in_mps, out, out_mps, accept);
    }
    kprintd("usb: port %s: %s-speed device %4x:%4x, skipped\n", usb_path(d), speed_name(d->speed), vid, pid);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Порты корневого хаба                                                       */
/* -------------------------------------------------------------------------- */
/* Отдать порт companion-контроллеру (UHCI/OHCI): Full/Low-Speed устройства
 * EHCI обслуживать не умеет, а BIOS эмулирует через companion USB-клавиатуру. */
static void ehci_release_port(uint32_t p) {
    if (!hc.n_cc) return;                /* companion'ов нет (например, Intel с RMH) */
    uint32_t v = op_read(EHCI_PORTSC(p));
    op_write(EHCI_PORTSC(p), (v & ~(PORT_RWC | PORT_PE)) | PORT_OWNER);
}

static void ehci_disable_port(uint32_t p) {
    uint32_t v = op_read(EHCI_PORTSC(p));
    op_write(EHCI_PORTSC(p), v & ~(PORT_RWC | PORT_PE));
}

/* Сброс порта. 1 — на порту High-Speed устройство и порт включён. */
static int ehci_reset_port(uint32_t p) {
    uint32_t reg = EHCI_PORTSC(p);
    uint32_t v = op_read(reg);
    if (!(v & PORT_CCS) || (v & PORT_OWNER)) return 0;

    if ((v & PORT_LS_MASK) == PORT_LS_K) {   /* Low-Speed (клавиатура/мышь) */
        ehci_release_port(p);
        return 0;
    }

    /* При установке Port Reset бит Port Enable должен быть 0 (EHCI 2.3.9) */
    op_write(reg, (v & ~(PORT_RWC | PORT_PE)) | PORT_RESET);
    mdelay(50);                              /* TDRSTR для корневого порта */
    op_write(reg, op_read(reg) & ~(PORT_RWC | PORT_PE | PORT_RESET));
    if (op_wait(reg, PORT_RESET, 0, 20000) != 0) {
        kprintd("usb: port %d: reset does not complete\n", p);
        return 0;
    }
    mdelay(10);                              /* TRSTRCY */

    v = op_read(reg);
    if (!(v & PORT_CCS)) return 0;
    if (!(v & PORT_PE)) {                    /* Full-Speed устройство */
        kprintd("usb: port %d: full-speed device, handed to companion controller\n", p);
        ehci_release_port(p);
        return 0;
    }
    op_write(reg, v);                        /* сбросить биты изменений (RWC), PE остаётся 1 */
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Захват и остановка контроллера                                             */
/* -------------------------------------------------------------------------- */
static void ehci_bios_handoff(uint32_t hccparams) {
    uint32_t eecp = HCC_EECP(hccparams);
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        uint32_t cap = pci_read_config_32(hc.bus, hc.dev, hc.fn, eecp);
        if ((cap & 0xFF) == LEGSUP_CAP_ID) {
            if (cap & LEGSUP_BIOS_OWNED) {
                pci_write_config_8(hc.bus, hc.dev, hc.fn, eecp + 3, 1);    /* HC OS Owned */
                for (int ms = 0; ms < 1000 && (cap & LEGSUP_BIOS_OWNED); ms += 10) {
                    mdelay(10);
                    cap = pci_read_config_32(hc.bus, hc.dev, hc.fn, eecp);
                }
                if (cap & LEGSUP_BIOS_OWNED) {
                    kprintw("usb: BIOS does not release EHCI %x:%x.%x, taking it by force\n",
                            hc.bus, hc.dev, hc.fn);
                    pci_write_config_8(hc.bus, hc.dev, hc.fn, eecp + 2, 0); /* снять BIOS Owned */
                }
            } else {
                pci_write_config_8(hc.bus, hc.dev, hc.fn, eecp + 3, 1);
            }
            /* выключаем SMI от контроллера, как это делает Linux */
            pci_write_config_32(hc.bus, hc.dev, hc.fn, eecp + 4, 0);
            return;
        }
        eecp = (cap >> 8) & 0xFF;
    }
}

/* Флешка на этом контроллере не нашлась: останавливаем его (больше никакого
 * DMA) и отдаём все порты companion-контроллерам UHCI/OHCI, если они есть,
 * как было при BIOS, — Low/Full-Speed клавиатуры там снова подхватит BIOS.
 * Так же Linux выключает EHCI (ehci_silence_controller).
 *
 * BIOS контроллер обратно НЕ получает: ни Linux, ни Windows этого не делают.
 * После нашего сброса BIOS обычно заново устройства не опрашивает (то есть
 * USB-клавиатуре это не помогает), а его SMM-обработчик на таком редко
 * используемом пути может повесить машину, причём без единого сообщения на
 * экране. SMI контроллера остаются выключенными. */
static void ehci_stop_controller(void) {
    op_write(EHCI_USBCMD, op_read(EHCI_USBCMD) & ~(CMD_RS | CMD_ASE | CMD_PSE));
    op_wait(EHCI_USBSTS, STS_HALTED, STS_HALTED, 20000);
    /* Без companion'ов (Intel 5-й серии и новее, порты за RMH) отдавать порты
     * некому, а запись CONFIGFLAG у Intel может вызвать SMI — не трогаем. */
    if (hc.n_cc) op_write(EHCI_CONFIGFLAG, 0);
}

/* Захват, сброс и запуск контроллера. 0 — контроллер работает. */
static int ehci_start_controller(uint32_t bar0) {
    hc_dead = 0;
    hc.cap = (volatile uint8_t*)bar0;
    uint8_t caplength = hc.cap[EHCI_CAPLENGTH];
    if (caplength < 0x0C || caplength == 0xFF) {
        kprintw("usb: EHCI %x:%x.%x: bad CAPLENGTH %x, skipped\n", hc.bus, hc.dev, hc.fn, caplength);
        return -1;
    }
    hc.op = hc.cap + caplength;

    uint32_t hcsparams = *(volatile uint32_t*)(hc.cap + EHCI_HCSPARAMS);
    uint32_t hccparams = *(volatile uint32_t*)(hc.cap + EHCI_HCCPARAMS);
    hc.n_ports = HCS_N_PORTS(hcsparams);
    hc.n_cc = HCS_N_CC(hcsparams);

    ehci_bios_handoff(hccparams);

    /* HCRESET можно ставить только остановленному контроллеру (EHCI 2.3.1) */
    op_write(EHCI_USBINTR, 0);
    op_write(EHCI_USBCMD, op_read(EHCI_USBCMD) & ~(CMD_RS | CMD_ASE | CMD_PSE));
    if (op_wait(EHCI_USBSTS, STS_HALTED, STS_HALTED, 20000) != 0) {
        ehci_fail("does not halt");
        return -1;
    }
    op_write(EHCI_USBCMD, CMD_HCRESET);
    if (op_wait(EHCI_USBCMD, CMD_HCRESET, 0, 250000) != 0) {
        ehci_fail("reset does not complete");
        return -1;
    }

    op_write(EHCI_USBINTR, 0);
    op_write(EHCI_USBSTS, STS_RWC_MASK);
    op_write(EHCI_CTRLDSSEGMENT, 0);     /* все наши структуры ниже 4 ГБ */
    op_write(EHCI_PERIODICLISTBASE, 0);
    op_write(EHCI_ASYNCLISTADDR, 0);

    /* Run/Stop = 1: без этого контроллер не выполняет расписание вообще */
    op_write(EHCI_USBCMD, CMD_ITC_8 | CMD_RS);
    if (op_wait(EHCI_USBSTS, STS_HALTED, 0, 20000) != 0) {
        ehci_fail("does not start");
        return -1;
    }

    op_write(EHCI_CONFIGFLAG, 1);        /* все порты — нашему EHCI */
    mdelay(5);

    if (hcsparams & HCS_PPC) {           /* питание портов включается программно */
        for (uint32_t p = 0; p < hc.n_ports; p++) {
            uint32_t v = op_read(EHCI_PORTSC(p));
            op_write(EHCI_PORTSC(p), (v & ~(PORT_RWC | PORT_PE)) | PORT_PP);
        }
        mdelay(20);
    }
    mdelay(100);                         /* устройства успевают подключиться (debounce) */
    return 0;
}

/* Пробуем один контроллер. 1 — флешка найдена и принята (контроллер остаётся за нами). */
static int ehci_probe_controller(uint8_t bus, uint8_t dev, uint8_t fn, int (*accept)(blockdev_t*)) {
    uint32_t bar0 = pci_read_config_32(bus, dev, fn, 0x10);
    if (bar0 & 0x01) return 0;                       /* I/O BAR — не EHCI */
    if ((bar0 & 0x06) == 0x04 && pci_read_config_32(bus, dev, fn, 0x14) != 0) {
        kprintw("usb: EHCI %x:%x.%x is mapped above 4 GB, skipped\n", bus, dev, fn);
        return 0;
    }
    bar0 &= 0xFFFFFFF0;
    if (!bar0) return 0;

    kprintd("usb: found EHCI at %x:%x.%x (BAR0: 0x%x)\n", bus, dev, fn, bar0);

    memset(&hc, 0, sizeof(hc));
    hc.bus = bus; hc.dev = dev; hc.fn = fn;

    uint32_t cmd = pci_read_config_32(bus, dev, fn, 0x04);
    pci_write_config_32(bus, dev, fn, 0x04, (cmd & 0xFFFF) | 0x06); /* Mem Space + Bus Master */

    /* Регистры EHCI занимают < 4 КБ, но BAR может начинаться не с начала страницы.
     * Отображаем без кэширования (PCD|PWT). */
    for (uint32_t page = bar0 & ~0xFFFu; page < bar0 + 0x1000; page += 0x1000) {
        vmm_identity_map_page(page_directory, page, 0x1B);
        asm volatile("invlpg (%0)" :: "r"(page) : "memory");
    }

    if (ehci_start_controller(bar0) != 0) {
        if (hc.op) ehci_stop_controller();
        return 0;
    }

    usb_ndevs = 0;
    usb_identified = 0;
    next_dev_addr = 1;
    for (uint32_t p = 0; p < hc.n_ports && !hc_dead; p++) {
        int silent = 0;      /* устройство не ответило и может остаться на адресе 0 */
        for (int attempt = 0; attempt < 2 && !hc_dead; attempt++) {
            if (!ehci_reset_port(p)) break;
            /* в корневом порту EHCI работает только High-Speed: флешка, хаб
             * (в т.ч. RMH чипсетов Intel) или что-то ещё */
            usb_dev_t* d = usb_alloc_dev(0, (uint8_t)p, USB_SPEED_HIGH);
            if (!d) { silent = 1; break; }
            if (usb_enumerate(d) == 0) {
                silent = 0;
                if (usb_attach(d, accept)) return 1;
                break;
            }
            silent = 1;
            kprintd("usb: port %d: high-speed device does not answer\n", p);
        }
        if (silent) ehci_disable_port(p);
    }

    ehci_stop_controller();
    kprintd("usb: EHCI %x:%x.%x: no boot disk (devices: %d), controller stopped\n",
            hc.bus, hc.dev, hc.fn, usb_identified);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Поиск EHCI-контроллеров на PCI                                             */
/* -------------------------------------------------------------------------- */
int usb_init(int (*accept)(blockdev_t* dev)) {
    if (usb_ready) return 1;
    int controllers = 0;

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

                if (cls == 0x0C && sub == 0x03 && prog == 0x20) {
                    controllers++;
                    if (ehci_probe_controller((uint8_t)bus, dev, fn, accept)) return 1;
                }
            }
        }
    }

    if (!controllers) kprintd("usb: no EHCI controller found on PCI\n");
    else              kprintd("usb: no usable USB mass storage on %d EHCI controller(s)\n", controllers);
    return 0;
}
