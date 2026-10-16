/*
 * drivers/ehci.c — драйвер контроллера EHCI (USB 2.0) для USB-стека ядра
 * (drivers/usb.c). Работает в защищённом режиме через DMA и MMIO, без
 * прерываний (опрос).
 *
 *   - забирает контроллер у BIOS по правилам EHCI (USBLEGSUP handoff);
 *   - в корневом порту EHCI работает только High-Speed: флешка или хаб (в
 *     т.ч. встроенный «rate matching hub» (RMH) чипсетов Intel 5-9 серий, к
 *     которому подключены все разъёмы платы). Full/Low-Speed устройства прямо
 *     в корневом порту отдаются companion-контроллеру UHCI/OHCI, как это
 *     делает BIOS;
 *   - Full-Speed устройства за High-Speed хабом — через его transaction
 *     translator (split-транзакции, EHCI 4.12, их делает сам контроллер);
 *   - если на контроллере не нашлось нужной флешки, он останавливается, а его
 *     порты отдаются companion-контроллерам, если они есть (как при BIOS).
 *     Обратно BIOS контроллер НЕ возвращается — так же, как в Linux и
 *     Windows: после сброса BIOS его обычно заново не опрашивает
 *     (USB-клавиатуре это всё равно не помогает), а SMM-обработчик BIOS на
 *     таком редком пути может повесить машину.
 *
 * ВСЕ ожидания ограничены по времени.
 */

#include <drivers/usb_hc.h>
#include <drivers/pci.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>

/* ---- регистры EHCI ---------------------------------------------------------- */
/* capability registers */
#define EHCI_CAPLENGTH          0x00
#define EHCI_HCSPARAMS          0x04
#define EHCI_HCCPARAMS          0x08
/* operational registers (база = cap + CAPLENGTH) */
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

/* USBLEGSUP (EHCI 2.1.7) в конфигурационном пространстве PCI */
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

/* qTD (EHCI 3.5) вместе с 64-битными расширениями (приложение B): старшие
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

#define QH_CONTROL              0
#define QH_BULK_OUT             1
#define QH_BULK_IN              2

/* Ядро работает с тождественным отображением (виртуальный = физический адрес),
 * поэтому адреса этих статических массивов можно отдавать контроллеру. */
static ehci_qh_t  qh_pool[4]        __attribute__((aligned(4096)));
static ehci_qtd_t qtd_pool[8]       __attribute__((aligned(4096)));
static uint8_t    dma_buffer[4096]  __attribute__((aligned(4096)));
static uint8_t    setup_pkt[8]      __attribute__((aligned(32)));

/* ---- состояние ---------------------------------------------------------------- */
typedef struct {
    uint8_t  bus, dev, fn;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    uint32_t n_ports;
    uint32_t n_cc;               /* число companion-контроллеров (UHCI/OHCI) */
} ehci_ctrl_t;

static ehci_ctrl_t hc;
static uint8_t     next_dev_addr = 1;

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
        usb_udelay(1);
        timeout_us--;
    }
}

static int ehci_fail(const char* why) {
    if (!usb_hc_dead) kprintw("usb: EHCI %x:%x.%x: %s, controller disabled\n", hc.bus, hc.dev, hc.fn, why);
    usb_hc_dead = 1;
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
    if (usb_hc_dead) return XFER_DEAD;

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
        usb_udelay(1);
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

/* -------------------------------------------------------------------------- */
/* Control Transfer (Endpoint 0)                                              */
/* -------------------------------------------------------------------------- */
static int ehci_control(usb_dev_t* d, uint8_t req_type, uint8_t req, uint16_t val,
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
    return res;
}

/* SET_ADDRESS: адреса раздаём по порядку, отдельно на каждом контроллере */
static int ehci_set_address(usb_dev_t* d) {
    if (next_dev_addr > 127) return -1;
    uint8_t addr = next_dev_addr++;
    if (ehci_control(d, RT_STD_OUT, USB_REQ_SET_ADDRESS, addr, 0, 0, 0, 0) != XFER_OK) return -1;
    d->addr = addr;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Bulk Transfer — data toggle ведёт контроллер в overlay QH (DTC = 0)         */
/* -------------------------------------------------------------------------- */
static int ehci_config_bulk(usb_dev_t* d, uint8_t in, uint16_t in_mps, uint8_t in_burst,
                            uint8_t out, uint16_t out_mps, uint8_t out_burst) {
    (void)in_burst; (void)out_burst;
    qh_setup(&qh_pool[QH_BULK_OUT], d, out, out_mps, 0);  /* DTC = 0, toggle = DATA0 */
    qh_setup(&qh_pool[QH_BULK_IN],  d, in,  in_mps,  0);
    return 0;
}

static int ehci_bulk(usb_dev_t* d, int is_in, void* data, uint32_t len, uint32_t* actual) {
    (void)d;
    if (actual) *actual = 0;
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

/* после CLEAR_FEATURE(ENDPOINT_HALT) toggle на обеих сторонах -> DATA0 */
static void ehci_reset_bulk(usb_dev_t* d, int is_in) {
    (void)d;
    qh_pool[is_in ? QH_BULK_IN : QH_BULK_OUT].token &= ~QTD_TOGGLE;
}

static const usb_hc_t ehci_ops = {
    .name        = "EHCI",
    .set_address = ehci_set_address,
    .control     = ehci_control,
    .config_bulk = ehci_config_bulk,
    .bulk        = ehci_bulk,
    .reset_bulk  = ehci_reset_bulk,
};

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
    usb_mdelay(50);                          /* TDRSTR для корневого порта */
    op_write(reg, op_read(reg) & ~(PORT_RWC | PORT_PE | PORT_RESET));
    if (op_wait(reg, PORT_RESET, 0, 20000) != 0) {
        kprintd("usb: port %d: reset does not complete\n", p);
        return 0;
    }
    usb_mdelay(10);                          /* TRSTRCY */

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
                usb_pci_write8(hc.bus, hc.dev, hc.fn, eecp + 3, 1);    /* HC OS Owned */
                for (int ms = 0; ms < 1000 && (cap & LEGSUP_BIOS_OWNED); ms += 10) {
                    usb_mdelay(10);
                    cap = pci_read_config_32(hc.bus, hc.dev, hc.fn, eecp);
                }
                if (cap & LEGSUP_BIOS_OWNED) {
                    kprintw("usb: BIOS does not release EHCI %x:%x.%x, taking it by force\n",
                            hc.bus, hc.dev, hc.fn);
                    usb_pci_write8(hc.bus, hc.dev, hc.fn, eecp + 2, 0); /* снять BIOS Owned */
                }
            } else {
                usb_pci_write8(hc.bus, hc.dev, hc.fn, eecp + 3, 1);
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
    usb_mdelay(5);

    if (hcsparams & HCS_PPC) {           /* питание портов включается программно */
        for (uint32_t p = 0; p < hc.n_ports; p++) {
            uint32_t v = op_read(EHCI_PORTSC(p));
            op_write(EHCI_PORTSC(p), (v & ~(PORT_RWC | PORT_PE)) | PORT_PP);
        }
        usb_mdelay(20);
    }
    usb_mdelay(100);                     /* устройства успевают подключиться (debounce) */
    return 0;
}

/* Пробуем один контроллер. 1 — флешка найдена и принята (контроллер остаётся за нами). */
int ehci_probe(uint8_t bus, uint8_t dev, uint8_t fn, int (*accept)(blockdev_t*)) {
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
    usb_bus_begin(&ehci_ops);
    next_dev_addr = 1;

    uint32_t cmd = pci_read_config_32(bus, dev, fn, 0x04);
    pci_write_config_32(bus, dev, fn, 0x04, (cmd & 0xFFFF) | 0x06); /* Mem Space + Bus Master */

    /* Регистры EHCI занимают < 4 КБ, но BAR может начинаться не с начала страницы. */
    usb_map_mmio(bar0, 0x1000);

    if (ehci_start_controller(bar0) != 0) {
        if (hc.op) ehci_stop_controller();
        return 0;
    }

    for (uint32_t p = 0; p < hc.n_ports && !usb_hc_dead; p++) {
        int silent = 0;      /* устройство не ответило и может остаться на адресе 0 */
        for (int attempt = 0; attempt < 2 && !usb_hc_dead; attempt++) {
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
            hc.bus, hc.dev, hc.fn, usb_bus_identified());
    return 0;
}
