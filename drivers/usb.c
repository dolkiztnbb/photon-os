/*
 * drivers/usb.c — 32-битный драйвер USB 2.0 EHCI + USB Mass Storage (BOT / SCSI)
 * Работает полностью в защищённом режиме через DMA и MMIO, без прерываний (опрос).
 *
 * Что умеет:
 *   - перебирает ВСЕ EHCI-контроллеры на PCI (класс 0C/03/20);
 *   - забирает контроллер у BIOS по правилам EHCI (USBLEGSUP handoff);
 *   - High-Speed флешка, воткнутая прямо в корневой порт контроллера;
 *     Full/Low-Speed устройства (клавиатуры, мыши, флешки USB 1.1) отдаются
 *     companion-контроллеру UHCI/OHCI, как это делает BIOS;
 *   - если на контроллере не нашлось нужной флешки, он сбрасывается и
 *     возвращается BIOS (чтобы не ломать BIOS-эмуляцию USB-клавиатуры).
 *
 * Чего (пока) нет: USB-хабов (в т.ч. встроенного «rate matching hub» в
 * чипсетах Intel 5-й серии и новее — там все порты EHCI идут через него)
 * и xHCI (USB 3.x). В этих случаях флешка ядру не видна, и оно работает с
 * копией образа в ОЗУ (rd0), которую сделал загрузчик.
 *
 * ВСЕ ожидания ограничены по времени: если контроллер или флешка ведут себя
 * не так, как ожидается, драйвер сдаётся, и загрузка продолжается.
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
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_BOT_RESET       0xFF   /* Bulk-Only Mass Storage Reset (class) */

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09
#define USB_PROTO_BOT           0x50

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

#define QH_EPS_HIGH             (2u << 12)
#define QH_DTC                  (1u << 14)  /* toggle берётся из qTD (нужно для control) */
#define QH_HEAD                 (1u << 15)  /* H: голова async-списка (Head of Reclamation List) */
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
    uint32_t legsup;             /* смещение USBLEGSUP в PCI config, 0 = нет */
    uint32_t saved_legctlsts;    /* USBLEGCTLSTS до захвата — вернём BIOS при отказе */
    int      bios_owned;         /* BIOS владел контроллером до нас */
} ehci_ctrl_t;

static ehci_ctrl_t hc;
static int hc_dead = 0;          /* контроллер не отвечает — все передачи сразу с ошибкой */

static uint8_t  next_dev_addr = 1;
static uint8_t  usb_dev_addr = 0;
static uint8_t  usb_mps0 = 64;
static uint8_t  usb_iface = 0;
static uint8_t  ep_in = 0, ep_out = 0;
static uint16_t ep_in_mps = 512, ep_out_mps = 512;
static uint32_t usb_total_sectors = 0;
static char     usb_model[41] = "USB 2.0 Flash Drive";
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
/* Control Transfer (Endpoint 0)                                              */
/* -------------------------------------------------------------------------- */
static int usb_control_msg(uint8_t dev, uint8_t req_type, uint8_t req, uint16_t val,
                           uint16_t idx, uint16_t len, void* data) {
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
    memset((void*)qh, 0, sizeof(ehci_qh_t));
    qh->qhlp = ((uint32_t)qh) | LINK_TYPE_QH;
    qh->ep_char = (dev & 0x7F) | QH_EPS_HIGH | QH_DTC | QH_HEAD | ((uint32_t)usb_mps0 << 16);
    qh->ep_cap = QH_MULT_1;

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
    if (res == XFER_OK && len > 0 && is_in && data) {
        memcpy(data, dma_buffer, len);
    }
    return res;
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
    return res;
}

static void bulk_qh_init(ehci_qh_t* qh, uint8_t ep, uint16_t mps) {
    memset((void*)qh, 0, sizeof(ehci_qh_t));
    qh->qhlp = ((uint32_t)qh) | LINK_TYPE_QH;
    /* H = 1: каждый QH стоит в async-списке один (ссылается сам на себя), и
     * без головы списка QEMU вообще не обрабатывает очередь. */
    qh->ep_char = (usb_dev_addr & 0x7F) | ((uint32_t)(ep & 0x0F) << 8) | QH_EPS_HIGH | QH_HEAD |
                  ((uint32_t)mps << 16);
    qh->ep_cap = QH_MULT_1;
    qh->next_qtd = LINK_TERMINATE;
    qh->alt_next_qtd = LINK_TERMINATE;
}

/* CLEAR_FEATURE(ENDPOINT_HALT): снимает STALL, toggle на обеих сторонах -> DATA0 */
static void usb_clear_halt(int is_in) {
    uint8_t ep = is_in ? (uint8_t)(ep_in | 0x80) : ep_out;
    usb_control_msg(usb_dev_addr, 0x02, USB_REQ_CLEAR_FEATURE, 0, ep, 0, 0);
    qh_pool[is_in ? QH_BULK_IN : QH_BULK_OUT].token &= ~QTD_TOGGLE;
}

/* Reset Recovery из спецификации BOT 5.3.4 */
static void bot_reset_recovery(void) {
    usb_control_msg(usb_dev_addr, 0x21, USB_REQ_BOT_RESET, 0, usb_iface, 0, 0);
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

static void usb_request_sense(void) {
    uint8_t cmd[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    usb_bot_scsi(cmd, 6, sense, 18, 1);
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
/* Инициализация накопителя на включённом (после сброса) порту                 */
/* -------------------------------------------------------------------------- */
static void copy_trimmed(char* dst, const uint8_t* src, int n) {
    int len = n;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
    for (int i = 0; i < len; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : '?';
    dst[len] = 0;
}

static int usb_init_mass_storage(uint32_t port) {
    uint8_t buf[256];
    usb_ready = 0;
    usb_cache_count = 0;
    usb_dev_addr = 0;
    usb_mps0 = 64;                       /* для High-Speed размер пакета EP0 всегда 64 */

    /* 1. Первые 8 байт дескриптора устройства по адресу 0 (как это делает
     *    Windows: некоторые флешки иначе не принимают SET_ADDRESS). */
    memset(buf, 0, sizeof(buf));
    if (usb_control_msg(0, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 8, buf) != XFER_OK) {
        kprintd("usb: port %d: device does not answer\n", port);
        return -1;
    }
    if (buf[7] == 8 || buf[7] == 16 || buf[7] == 32 || buf[7] == 64) usb_mps0 = buf[7];

    uint8_t addr = next_dev_addr++;
    if (next_dev_addr > 127) next_dev_addr = 1;
    if (usb_control_msg(0, 0x00, USB_REQ_SET_ADDRESS, addr, 0, 0, 0) != XFER_OK) return -1;
    mdelay(10);                          /* SET_ADDRESS recovery: >= 2 мс */
    usb_dev_addr = addr;

    if (usb_control_msg(usb_dev_addr, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 18, buf) != XFER_OK)
        return -1;
    uint8_t dev_class = buf[4];
    if (dev_class == USB_CLASS_HUB) {
        kprintw("usb: port %d: USB hub found - hubs are not supported yet\n", port);
        return -1;
    }

    /* 2. Конфигурация: сначала 9 байт ради wTotalLength, потом целиком. */
    if (usb_control_msg(usb_dev_addr, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, 9, buf) != XFER_OK)
        return -1;
    uint16_t total_len = buf[2] | (buf[3] << 8);
    uint8_t config_value = buf[5];
    if (total_len < 9) return -1;
    if (total_len > sizeof(buf)) total_len = sizeof(buf);
    if (usb_control_msg(usb_dev_addr, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, total_len, buf) != XFER_OK)
        return -1;

    /* 3. Ищем интерфейс Mass Storage / Bulk-Only (alt setting 0) и его bulk-endpoint'ы */
    ep_in = 0; ep_out = 0;
    int in_msc = 0, iface_found = 0;
    uint16_t offset = 0;
    while (offset + 2 <= total_len) {
        uint8_t len  = buf[offset];
        uint8_t type = buf[offset + 1];
        if (len < 2 || offset + len > total_len) break;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (iface_found) break;      /* следующий интерфейс — endpoint'ы уже собраны */
            in_msc = buf[offset + 3] == 0 && buf[offset + 5] == USB_CLASS_MASS_STORAGE &&
                     buf[offset + 7] == USB_PROTO_BOT;
            if (in_msc) {
                usb_iface = buf[offset + 2];
                iface_found = 1;
            }
        } else if (type == USB_DESC_ENDPOINT && len >= 7 && in_msc) {
            uint8_t ep_addr = buf[offset + 2];
            uint8_t ep_attr = buf[offset + 3];
            uint16_t mps = (buf[offset + 4] | (buf[offset + 5] << 8)) & 0x7FF;
            if ((ep_attr & 0x03) == 0x02) {
                if (ep_addr & 0x80) { ep_in  = ep_addr & 0x0F; ep_in_mps  = mps ? mps : 512; }
                else                { ep_out = ep_addr & 0x0F; ep_out_mps = mps ? mps : 512; }
            }
        }
        offset += len;
    }
    if (!iface_found || !ep_in || !ep_out) {
        kprintd("usb: port %d: not a USB mass storage (BOT) device, class %x\n", port, dev_class);
        return -1;
    }

    if (usb_control_msg(usb_dev_addr, 0x00, USB_REQ_SET_CONFIG, config_value, 0, 0, 0) != XFER_OK)
        return -1;
    mdelay(10);

    /* После SET_CONFIGURATION toggle у bulk-endpoint'ов = DATA0 */
    bulk_qh_init(&qh_pool[QH_BULK_OUT], ep_out, ep_out_mps);
    bulk_qh_init(&qh_pool[QH_BULK_IN],  ep_in,  ep_in_mps);

    /* 4. Ждём готовности носителя (после включения флешка отвечает
     *    «UNIT ATTENTION», пока у неё не спросят sense-данные). */
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    int ready = 0;
    for (int i = 0; i < 20 && !hc_dead; i++) {
        int r = usb_bot_scsi(tur, 6, 0, 0, 0);
        if (r == 0) { ready = 1; break; }
        if (r == 1) usb_request_sense();
        mdelay(100);
    }
    if (hc_dead) return -1;
    if (!ready) kprintw("usb: port %d: medium is not ready, trying anyway\n", port);

    uint8_t inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq_resp[36];
    memset(inq_resp, 0, sizeof(inq_resp));
    if (usb_bot_scsi(inq, 6, inq_resp, 36, 1) == 0) {
        if ((inq_resp[0] & 0x1F) != 0x00) {
            kprintd("usb: port %d: SCSI device type %x is not a disk\n", port, inq_resp[0] & 0x1F);
            return -1;
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
    if (r == 1) {                        /* часто проходит со второй попытки */
        usb_request_sense();
        r = usb_bot_scsi(rc_cmd, 10, rc_resp, 8, 1);
    }
    if (r != 0) {
        kprintw("usb: port %d: READ CAPACITY failed\n", port);
        return -1;
    }
    uint32_t last_lba = ((uint32_t)rc_resp[0] << 24) | (rc_resp[1] << 16) | (rc_resp[2] << 8) | rc_resp[3];
    uint32_t blk_size = ((uint32_t)rc_resp[4] << 24) | (rc_resp[5] << 16) | (rc_resp[6] << 8) | rc_resp[7];
    if (blk_size != 512) {
        kprintw("usb: port %d: %d-byte sectors are not supported\n", port, blk_size);
        return -1;
    }
    usb_total_sectors = last_lba + 1;
    usb_failures = 0;
    usb_ready = 1;
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
        kprintd("usb: port %d: full-speed device, handing it to the companion controller\n", p);
        ehci_release_port(p);
        return 0;
    }
    op_write(reg, v);                        /* сбросить биты изменений (RWC), PE остаётся 1 */
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Захват и возврат контроллера                                               */
/* -------------------------------------------------------------------------- */
static void ehci_bios_handoff(uint32_t hccparams) {
    hc.legsup = 0;
    hc.bios_owned = 0;
    uint32_t eecp = HCC_EECP(hccparams);
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        uint32_t cap = pci_read_config_32(hc.bus, hc.dev, hc.fn, eecp);
        if ((cap & 0xFF) == LEGSUP_CAP_ID) {
            hc.legsup = eecp;
            hc.saved_legctlsts = pci_read_config_32(hc.bus, hc.dev, hc.fn, eecp + 4);
            if (cap & LEGSUP_BIOS_OWNED) {
                hc.bios_owned = 1;
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

/* Флешка на этом контроллере не нашлась: останавливаем его, отдаём все порты
 * companion-контроллерам и возвращаем контроллер BIOS, чтобы по возможности
 * не ломать BIOS-эмуляцию USB-клавиатуры (драйвера HID у ядра нет). */
static void ehci_release_controller(void) {
    op_write(EHCI_USBCMD, op_read(EHCI_USBCMD) & ~(CMD_RS | CMD_ASE | CMD_PSE));
    op_wait(EHCI_USBSTS, STS_HALTED, STS_HALTED, 20000);
    op_write(EHCI_CONFIGFLAG, 0);
    op_write(EHCI_USBCMD, CMD_HCRESET);
    op_wait(EHCI_USBCMD, CMD_HCRESET, 0, 250000);

    if (hc.legsup) {
        pci_write_config_32(hc.bus, hc.dev, hc.fn, hc.legsup + 4, hc.saved_legctlsts);
        if (hc.bios_owned) pci_write_config_8(hc.bus, hc.dev, hc.fn, hc.legsup + 3, 0);
    }
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
        if (hc.op) ehci_release_controller();
        return 0;
    }

    for (uint32_t p = 0; p < hc.n_ports && !hc_dead; p++) {
        if (!ehci_reset_port(p)) continue;
        kprintd("usb: High-Speed device connected to port %d\n", p);
        if (usb_init_mass_storage(p) != 0) {
            usb_ready = 0;
            ehci_disable_port(p);
            continue;
        }

        blockdev_t d;
        memset(&d, 0, sizeof(d));
        strncpy(d.name, "usb0", BLK_NAME_LEN - 1);
        strncpy(d.model, usb_model, sizeof(d.model) - 1);
        d.sector_size = 512;
        d.total_sectors = usb_total_sectors;
        d.flags = BLK_FLAG_REMOVABLE;
        d.read = usb_blk_read;
        d.write = usb_blk_write;

        if (accept && !accept(&d)) {
            kprintd("usb: port %d: %s is not the boot disk, skipped\n", p, usb_model);
            usb_ready = 0;
            ehci_disable_port(p);
            continue;
        }

        blockdev_register(&d);
        kprinti("usb: mounted %s (%s), %d MB\n", d.name, d.model, d.total_sectors / 2048);
        return 1;
    }

    ehci_release_controller();
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
