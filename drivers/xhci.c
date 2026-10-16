/*
 * drivers/xhci.c — драйвер контроллера xHCI (USB 3.x) для USB-стека ядра
 * (drivers/usb.c). Работает в защищённом режиме через DMA и MMIO, без
 * прерываний: результаты команд и передач читаются с кольца событий опросом.
 *
 *   - забирает контроллер у BIOS (USB Legacy Support, xHCI 7.1.1) и, как и
 *     EHCI-драйвер, обратно его НЕ возвращает (см. drivers/ehci.c);
 *   - корневые порты USB 2.0 и USB 3.x (какой порт какой — из Supported
 *     Protocol Capability, xHCI 7.2): SuperSpeed, High-Speed и Full-Speed
 *     устройства прямо в порту, без companion-контроллеров;
 *   - хабы USB 2.0 (в т.ч. USB 2.0-половина хабов USB 3.0) — общим кодом
 *     хабов из drivers/usb.c: контроллеру сообщаются route string, слот и
 *     порт хаба с TT; SuperSpeed-хабов пока нет;
 *   - контексты по 32 и 64 байта, scratchpad-буферы, версии 0.96 и новее
 *     (Renesas/NEC uPD720200, Intel, AMD, ASMedia...). Все структуры лежат
 *     ниже 4 ГБ, поэтому 64-битные контроллеры работают как 32-битные.
 *
 * Как устроен обмен (xHCI 4.3-4.6): у каждого устройства есть слот (Enable
 * Slot) с контекстом в DCBAA, у каждого endpoint'а — своё кольцо TRB.
 * Передача — TRB на кольце endpoint'а и запись в doorbell слота; команда —
 * TRB на кольце команд и doorbell 0. О завершении контроллер пишет событие на
 * кольцо событий.
 *
 * ВСЕ ожидания ограничены по времени.
 */

#include <drivers/usb_hc.h>
#include <drivers/pci.h>
#include <cpu/malloc/kmalloc.h>
#include <kernel/debug.h>
#include <libc/mem.h>
#include <libc/string.h>

/* ---- регистры xHCI ---------------------------------------------------------- */
/* capability registers (от начала BAR0) */
#define XHCI_CAPLENGTH          0x00
#define XHCI_HCSPARAMS1         0x04
#define XHCI_HCSPARAMS2         0x08
#define XHCI_HCCPARAMS1         0x10
#define XHCI_DBOFF              0x14
#define XHCI_RTSOFF             0x18

#define HCS1_MAX_SLOTS(x)       ((x) & 0xFF)
#define HCS1_MAX_PORTS(x)       (((x) >> 24) & 0xFF)
#define HCS2_SCRATCHPADS(x)     ((((x) >> 21) & 0x1F) << 5 | (((x) >> 27) & 0x1F))
#define HCC1_CSZ                (1u << 2)   /* контексты по 64 байта */
#define HCC1_PPC                (1u << 3)   /* питание портов включается программно */
#define HCC1_XECP(x)            (((x) >> 16) & 0xFFFF)

/* operational registers (база = cap + CAPLENGTH) */
#define XHCI_USBCMD             0x00
#define XHCI_USBSTS             0x04
#define XHCI_PAGESIZE           0x08
#define XHCI_CRCR               0x18
#define XHCI_DCBAAP             0x30
#define XHCI_CONFIG             0x38
#define XHCI_PORTSC(n)          (0x400 + 0x10 * ((n) - 1))

#define CMD_RUN                 (1u << 0)
#define CMD_HCRST               (1u << 1)
#define CMD_INTE                (1u << 2)
#define CMD_HSEE                (1u << 3)

#define STS_HCH                 (1u << 0)   /* HCHalted */
#define STS_HSE                 (1u << 2)   /* Host System Error */
#define STS_EINT                (1u << 3)
#define STS_PCD                 (1u << 4)
#define STS_SRE                 (1u << 10)
#define STS_CNR                 (1u << 11)  /* Controller Not Ready */
#define STS_HCE                 (1u << 12)  /* Host Controller Error */

#define CRCR_RCS                (1u << 0)

/* PORTSC (xHCI 5.4.8) */
#define PORT_CCS                (1u << 0)
#define PORT_PED                (1u << 1)   /* RW1C: запись 1 ВЫКЛЮЧАЕТ порт */
#define PORT_OCA                (1u << 3)
#define PORT_PR                 (1u << 4)
#define PORT_PLS(x)             (((x) >> 5) & 0xF)
#define PORT_PP                 (1u << 9)
#define PORT_SPEED(x)           (((x) >> 10) & 0xF)
#define PORT_CSC                (1u << 17)
#define PORT_PEC                (1u << 18)
#define PORT_WRC                (1u << 19)
#define PORT_OCC                (1u << 20)
#define PORT_PRC                (1u << 21)
#define PORT_PLC                (1u << 22)
#define PORT_CEC                (1u << 23)
#define PORT_CAS                (1u << 24)  /* cold attach: каналу USB 3 нужен warm reset */
#define PORT_WPR                (1u << 31)  /* warm reset (только USB 3) */
#define PORT_CHANGES            (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)
/* биты, которые при записи надо сохранить как есть (RO и RWS) */
#define PORT_KEEP               (PORT_CCS | PORT_OCA | (0xFu << 5) | PORT_PP | (0xFu << 10) | \
                                 (3u << 14) | (7u << 25) | (1u << 30))

#define PLS_INACTIVE            6
#define PLS_COMPLIANCE          10

/* runtime registers: interrupter 0 */
#define XHCI_IR0                0x20
#define IR_IMAN                 0x00
#define IR_ERSTSZ               0x08
#define IR_ERSTBA               0x10
#define IR_ERDP                 0x18
#define IMAN_IP                 (1u << 0)
#define ERDP_EHB                (1u << 3)

/* extended capabilities (xHCI 7) */
#define XECP_LEGACY             1
#define XECP_PROTOCOL           2
#define LEGACY_BIOS_OWNED       (1u << 16)
#define LEGACY_OS_OWNED         (1u << 24)
#define LEGACY_SMI_KEEP         ((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))  /* все разрешения SMI = 0 */
#define LEGACY_SMI_EVENTS       (0x7u << 29)                               /* RW1C: сбросить события */

/* ---- TRB (xHCI 6.4) ---------------------------------------------------------- */
#define TRB_NORMAL              1
#define TRB_SETUP               2
#define TRB_DATA                3
#define TRB_STATUS              4
#define TRB_LINK                6
#define TRB_ENABLE_SLOT         9
#define TRB_DISABLE_SLOT        10
#define TRB_ADDRESS_DEV         11
#define TRB_CONFIG_EP           12
#define TRB_EVAL_CTX            13
#define TRB_RESET_EP            14
#define TRB_STOP_EP             15
#define TRB_SET_DEQ             16
#define TRB_EV_TRANSFER         32
#define TRB_EV_COMMAND          33
#define TRB_EV_NEC_COMMAND      48  /* NEC/Renesas: событие завершения команды (как в Linux) */

#define TRB_C                   (1u << 0)   /* cycle */
#define TRB_TC                  (1u << 1)   /* link: toggle cycle */
#define TRB_ISP                 (1u << 2)   /* событие и на коротком пакете */
#define TRB_IOC                 (1u << 5)
#define TRB_IDT                 (1u << 6)   /* данные (setup-пакет) прямо в TRB */
#define TRB_BSR                 (1u << 9)   /* Address Device без SET_ADDRESS */
#define TRB_TYPE(t)             ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)         (((c) >> 10) & 0x3F)
#define TRB_DIR_IN              (1u << 16)
#define TRB_TRT(t)              ((uint32_t)(t) << 16)   /* setup: 2 = OUT data, 3 = IN data */
#define TRB_EP(dci)             ((uint32_t)(dci) << 16)
#define TRB_SLOT(s)             ((uint32_t)(s) << 24)

/* completion codes */
#define CC_SUCCESS              1
#define CC_STALL                6
#define CC_SHORT_PACKET         13
#define CC_STOPPED              26
#define CC_STOPPED_LEN          27
#define CC_STOPPED_SHORT        28

/* endpoint context: тип endpoint'а */
#define EP_TYPE_BULK_OUT        2
#define EP_TYPE_CONTROL         4
#define EP_TYPE_BULK_IN         6

/* ---- память контроллера -------------------------------------------------------- */
#define XHCI_MAX_SLOTS          32
#define CMD_RING_TRBS           64
#define EVT_RING_TRBS           256
#define EP0_RING_TRBS           64
#define BULK_RING_TRBS          64
#define CMD_TIMEOUT_MS          5000   /* как в Linux */
#define PORT_RESET_TIMEOUT_MS   800
#define ROOT_CONNECT_WINDOW_MS  1000   /* сколько ждать подключения к пустым корневым портам */
#define ROOT_POLL_MS            50

typedef struct { uint32_t d0, d1, d2, d3; } trb_t;

/* Кольцо TRB: последний TRB — Link на начало (xHCI 4.9.2). */
typedef struct {
    volatile trb_t* trb;
    uint32_t size;
    uint32_t enq;                /* куда пишем следующий TRB */
    uint32_t cycle;              /* наш cycle bit */
} xring_t;

/* Ядро работает с тождественным отображением, адреса статических массивов —
 * физические. Выравнивание и границы — по таблице 6-1 спецификации. */
static volatile uint32_t dcbaa[2 * (XHCI_MAX_SLOTS + 1)]          __attribute__((aligned(4096)));
static uint8_t           dev_ctx[XHCI_MAX_SLOTS][32 * 64]         __attribute__((aligned(4096)));
static uint8_t           in_ctx[33 * 64]                          __attribute__((aligned(4096)));
static volatile trb_t    cmd_trbs[CMD_RING_TRBS]                  __attribute__((aligned(4096)));
static volatile trb_t    evt_trbs[EVT_RING_TRBS]                  __attribute__((aligned(4096)));
static volatile uint32_t erst[4]                                  __attribute__((aligned(64)));
static volatile trb_t    ep0_trbs[XHCI_MAX_SLOTS][EP0_RING_TRBS]  __attribute__((aligned(4096)));
static volatile trb_t    bulk_trbs[2][BULK_RING_TRBS]             __attribute__((aligned(4096)));
static uint8_t           xdma[4096]                               __attribute__((aligned(4096)));

/* scratchpad-буферы: их число знает только контроллер — берём из kmalloc */
static uint8_t*  sp_pages = 0;
static uint32_t* sp_array = 0;
static uint32_t  sp_allocated = 0;

/* ---- состояние ---------------------------------------------------------------- */
typedef struct {
    uint8_t  bus, dev, fn;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* ir0;       /* interrupter 0 */
    volatile uint32_t* db;       /* doorbell array */
    uint32_t mapped;             /* сколько байт BAR0 отображено */
    uint16_t version;
    uint32_t max_slots;          /* включено слотов */
    uint32_t max_ports;
    uint32_t ctx_size;           /* 32 или 64 */
    uint32_t scratchpads;
    int      no_bsr;             /* Address Device с BSR = 1 не работает: сразу BSR = 0 */
} xhci_ctrl_t;

static xhci_ctrl_t xc;
static uint8_t  port_proto[256];         /* 2 — порт USB 2.0, 3 — USB 3.x, 0 — неизвестно */
static uint8_t  port_slot_type[256];

static xring_t  cmd_ring;
static xring_t  ep0_ring[XHCI_MAX_SLOTS + 1];
static uint32_t evt_deq = 0, evt_cycle = 1;

/* bulk-endpoint'ы флешки: [0] — OUT, [1] — IN */
static xring_t  bulk_ring[2];
static uint8_t  bulk_slot = 0;
static uint8_t  bulk_dci[2];
static uint16_t bulk_mps[2];
static uint8_t  bulk_burst[2];
static uint8_t  bulk_toggle_clean[2];    /* Reset Endpoint уже сбросил toggle */

/* ---- доступ к регистрам ---------------------------------------------------------- */
static uint32_t op_read(uint32_t reg)              { return *(volatile uint32_t*)(xc.op + reg); }
static void     op_write(uint32_t reg, uint32_t v) { *(volatile uint32_t*)(xc.op + reg) = v; }
static volatile uint32_t* portsc(uint32_t p)       { return (volatile uint32_t*)(xc.op + XHCI_PORTSC(p)); }

/* 64-битный регистр: младшая половина, потом старшая (xHCI 5.1) */
static void op_write64(uint32_t reg, uint32_t lo) {
    op_write(reg, lo);
    op_write(reg + 4, 0);
}

static void ir_write(uint32_t reg, uint32_t v) { *(volatile uint32_t*)(xc.ir0 + reg) = v; }
static void ir_write64(uint32_t reg, uint32_t lo) {
    ir_write(reg, lo);
    ir_write(reg + 4, 0);
}

/* Ждёт (reg & mask) == want не дольше timeout_ms. 0 — дождались. */
static int op_wait(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_ms) {
    for (uint32_t us = timeout_ms * 1000; ; us--) {
        if ((op_read(reg) & mask) == want) return 0;
        if (!us) return -1;
        usb_udelay(1);
    }
}

static void barrier(void) { __sync_synchronize(); }

static int xhci_fail(const char* why) {
    if (!usb_hc_dead) kprintw("usb: xHCI %x:%x.%x: %s, controller disabled\n", xc.bus, xc.dev, xc.fn, why);
    usb_hc_dead = 1;
    return XFER_DEAD;
}

/* Контроллер жив? (не остановился сам и не сообщил о внутренней ошибке) */
static int xhci_alive(void) {
    uint32_t sts = op_read(XHCI_USBSTS);
    if (sts & STS_HSE) { xhci_fail("host system error"); return 0; }
    if (sts & STS_HCE) { xhci_fail("host controller error"); return 0; }
    if (sts & STS_HCH) { xhci_fail("controller halted"); return 0; }
    return 1;
}

/* ---- кольца ------------------------------------------------------------------- */
static void ring_init(xring_t* r, volatile trb_t* trbs, uint32_t n) {
    memset((void*)trbs, 0, n * sizeof(trb_t));
    r->trb = trbs;
    r->size = n;
    r->enq = 0;
    r->cycle = 1;
    trbs[n - 1].d0 = (uint32_t)trbs;             /* Link на начало, пока не наш (C = 0) */
    trbs[n - 1].d3 = TRB_TYPE(TRB_LINK) | TRB_TC;
}

/* Положить TRB. Cycle bit пишется последним: до этого TRB для контроллера не
 * существует. Дойдя до Link, отдаём его контроллеру и меняем свой cycle. Ни
 * одна наша передача не состоит из нескольких связанных TRB, поэтому Link
 * всегда оказывается между ними (Chain = 0). */
static volatile trb_t* ring_push(xring_t* r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile trb_t* t = &r->trb[r->enq];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    barrier();
    t->d3 = (d3 & ~TRB_C) | r->cycle;
    if (++r->enq == r->size - 1) {
        volatile trb_t* link = &r->trb[r->size - 1];
        barrier();
        link->d3 = TRB_TYPE(TRB_LINK) | TRB_TC | r->cycle;
        r->cycle ^= 1;
        r->enq = 0;
    }
    return t;
}

/* текущая позиция кольца для контекста endpoint'а / Set TR Dequeue: адрес | DCS */
static uint32_t ring_pos(const xring_t* r) {
    return (uint32_t)&r->trb[r->enq] | r->cycle;
}

/* Следующее событие, 1 — есть. */
static int xhci_event(trb_t* ev) {
    volatile trb_t* e = &evt_trbs[evt_deq];
    uint32_t d3 = e->d3;
    if ((d3 & TRB_C) != evt_cycle) return 0;
    barrier();
    ev->d0 = e->d0;
    ev->d1 = e->d1;
    ev->d2 = e->d2;
    ev->d3 = d3;
    if (++evt_deq == EVT_RING_TRBS) {
        evt_deq = 0;
        evt_cycle ^= 1;
    }
    ir_write64(IR_ERDP, (uint32_t)&evt_trbs[evt_deq] | ERDP_EHB);
    return 1;
}

static void xhci_doorbell(uint32_t slot, uint32_t target) {
    barrier();
    xc.db[slot] = target;
}

/* Команда. Возвращает completion code (CC_SUCCESS...) или -1. */
static int xhci_cmd(uint32_t d0, uint32_t d3, uint8_t* slot_out) {
    if (usb_hc_dead) return -1;
    uint32_t trb = (uint32_t)ring_push(&cmd_ring, d0, 0, 0, d3);
    xhci_doorbell(0, 0);
    for (uint32_t us = CMD_TIMEOUT_MS * 1000; us; us--) {
        trb_t ev;
        while (xhci_event(&ev)) {
            /* события передач (в т.ч. «остановлено» после Stop Endpoint) и
             * изменения портов здесь не нужны */
            uint32_t type = TRB_GET_TYPE(ev.d3);
            if ((type == TRB_EV_COMMAND || type == TRB_EV_NEC_COMMAND) && ev.d0 == trb) {
                if (slot_out) *slot_out = (uint8_t)(ev.d3 >> 24);
                return (int)(ev.d2 >> 24);
            }
        }
        if (!xhci_alive()) return -1;
        usb_udelay(1);
    }
    xhci_fail("command timeout");
    return -1;
}

/* Ждёт завершения передачи на endpoint'е dci слота slot. t_first/t_data/
 * t_last — адреса TRB этой передачи: события от старых (прерванных) передач
 * пропускаются. В *left — сколько байт не передано в TRB с данными. */
static int xhci_wait_xfer(uint8_t slot, uint8_t dci, uint32_t t_first, uint32_t t_data,
                          uint32_t t_last, uint32_t timeout_ms, uint32_t* left) {
    *left = 0;
    for (uint32_t us = timeout_ms * 1000; us; us--) {
        trb_t ev;
        while (xhci_event(&ev)) {
            if (TRB_GET_TYPE(ev.d3) != TRB_EV_TRANSFER) continue;
            if ((ev.d3 >> 24) != slot || ((ev.d3 >> 16) & 0x1F) != dci) continue;
            uint32_t ptr = ev.d0, cc = ev.d2 >> 24;
            if (ptr != t_first && ptr != t_data && ptr != t_last) continue;
            if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
                if (ptr == t_data) *left = ev.d2 & 0xFFFFFF;
                if (ptr == t_last) return XFER_OK;
                continue;                        /* короткий пакет в стадии данных: ждём статус */
            }
            if (cc == CC_STOPPED || cc == CC_STOPPED_LEN || cc == CC_STOPPED_SHORT) continue;
            if (cc == CC_STALL) return XFER_STALL;
            /* на исправном железе этого не бывает — пригодится для отладки */
            kprintd("usb: xHCI slot %d ep %d: transfer error, code %d\n", slot, dci, cc);
            return XFER_ERROR;
        }
        if (!xhci_alive()) return XFER_DEAD;
        usb_udelay(1);
    }
    kprintd("usb: xHCI slot %d ep %d: transfer timeout\n", slot, dci);
    return XFER_TIMEOUT;
}

/* Endpoint после ошибки или таймаута: остановить и перевести его dequeue на
 * текущую позицию кольца (недоделанные TRB пропускаются). Halted endpoint
 * останавливает Reset Endpoint (он же сбрасывает toggle), работающий — Stop
 * Endpoint. 1 — был Reset Endpoint. */
static int xhci_ep_recover(uint8_t slot, uint8_t dci, xring_t* r) {
    int reset = xhci_cmd(0, TRB_TYPE(TRB_RESET_EP) | TRB_EP(dci) | TRB_SLOT(slot), 0) == CC_SUCCESS;
    if (!reset) xhci_cmd(0, TRB_TYPE(TRB_STOP_EP) | TRB_EP(dci) | TRB_SLOT(slot), 0);
    xhci_cmd(ring_pos(r), TRB_TYPE(TRB_SET_DEQ) | TRB_EP(dci) | TRB_SLOT(slot), 0);
    return reset;
}

/* ---- контексты ----------------------------------------------------------------- */
/* выходной контекст устройства: 0 — slot context, dci — endpoint */
static uint32_t* out_ctx(uint8_t slot, uint32_t dci) {
    return (uint32_t*)(dev_ctx[slot - 1] + dci * xc.ctx_size);
}

/* входной контекст: 0 — Input Control Context, 1 — slot, dci + 1 — endpoint */
static uint32_t* in_ctx_at(uint32_t idx) {
    return (uint32_t*)(in_ctx + idx * xc.ctx_size);
}

static void dcbaa_set(uint8_t slot, uint32_t addr) {
    dcbaa[slot * 2] = addr;
    dcbaa[slot * 2 + 1] = 0;
}

static uint32_t xhci_speed(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return 2;
        case USB_SPEED_HIGH:  return 3;
        case USB_SPEED_SUPER: return 4;
        default:              return 1;
    }
}

static void fill_slot_ctx(uint32_t* s, const usb_dev_t* d) {
    s[0] = (d->route & 0xFFFFF) | (xhci_speed(d->speed) << 20) | (1u << 27);  /* Context Entries = 1 */
    s[1] = (uint32_t)d->root_port << 16;
    s[2] = 0;
    if ((d->speed == USB_SPEED_FULL || d->speed == USB_SPEED_LOW) && d->tt && d->tt->slot)
        s[2] = d->tt->slot | ((uint32_t)d->tt_port << 8);
    s[3] = 0;
}

static void fill_ep0_ctx(uint32_t* e, const usb_dev_t* d) {
    e[0] = 0;
    e[1] = (3u << 1) | (EP_TYPE_CONTROL << 3) | ((uint32_t)d->mps0 << 16);  /* CErr = 3 */
    e[2] = ring_pos(&ep0_ring[d->slot]);
    e[3] = 0;
    e[4] = 8;                                    /* средняя длина TRB для control */
}

/* ---- операции контроллера ------------------------------------------------------ */
static void x_dev_close(usb_dev_t* d) {
    if (!d->slot) return;
    xhci_cmd(0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), 0);
    dcbaa_set(d->slot, 0);
    if (bulk_slot == d->slot) bulk_slot = 0;
    d->slot = 0;
}

/* Address Device: bsr = 1 — только подготовить EP0 (устройство остаётся на
 * адресе 0), bsr = 0 — контроллер сам посылает SET_ADDRESS. */
static int x_address(usb_dev_t* d, int bsr) {
    memset(in_ctx, 0, sizeof(in_ctx));
    in_ctx_at(0)[1] = 0x3;                       /* добавить slot и EP0 */
    fill_slot_ctx(in_ctx_at(1), d);
    fill_ep0_ctx(in_ctx_at(2), d);
    int cc = xhci_cmd((uint32_t)in_ctx, TRB_TYPE(TRB_ADDRESS_DEV) | (bsr ? TRB_BSR : 0) | TRB_SLOT(d->slot), 0);
    if (cc != CC_SUCCESS) {
        if (cc >= 0) kprintd("usb: port %s: Address Device failed (code %d)\n", usb_path(d), cc);
        return -1;
    }
    return 0;
}

static int x_dev_open(usb_dev_t* d) {
    uint8_t slot = 0;
    d->slot = 0;
    int cc = xhci_cmd(0, TRB_TYPE(TRB_ENABLE_SLOT) | ((uint32_t)port_slot_type[d->root_port] << 16), &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > xc.max_slots) {
        if (cc >= 0) kprintw("usb: port %s: no free xHCI slot (code %d)\n", usb_path(d), cc);
        if (cc == CC_SUCCESS && slot) {          /* слот вне нашей таблицы — вернуть */
            xhci_cmd(0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot), 0);
        }
        return -1;
    }
    memset(dev_ctx[slot - 1], 0, sizeof(dev_ctx[0]));
    ring_init(&ep0_ring[slot], ep0_trbs[slot - 1], EP0_RING_TRBS);
    dcbaa_set(slot, (uint32_t)dev_ctx[slot - 1]);
    d->slot = slot;
    /* Как Windows и Linux: сначала EP0 по адресу 0 (BSR = 1), SET_ADDRESS —
     * после первых 8 байт дескриптора. Если контроллер так не умеет —
     * классическая схема: адрес сразу. */
    if (!xc.no_bsr) {
        if (x_address(d, 1) == 0) return 0;
        xc.no_bsr = 1;
    }
    if (x_address(d, 0) == 0) {
        d->addr = out_ctx(slot, 0)[3] & 0xFF;
        return 0;
    }
    x_dev_close(d);
    return -1;
}

/* Новый размер пакета EP0 (Full-Speed устройство сообщило свой) */
static int x_set_mps0(usb_dev_t* d) {
    if (!d->slot) return -1;
    memset(in_ctx, 0, sizeof(in_ctx));
    in_ctx_at(0)[1] = 0x2;                       /* только EP0 */
    fill_ep0_ctx(in_ctx_at(2), d);
    return xhci_cmd((uint32_t)in_ctx, TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(d->slot), 0) == CC_SUCCESS ? 0 : -1;
}

static int x_set_address(usb_dev_t* d) {
    if (!d->slot) return -1;
    if (d->addr) return 0;                       /* уже назначен в x_dev_open (без BSR) */
    if (x_address(d, 0) != 0) {
        xc.no_bsr = 1;                           /* следующая попытка — без BSR */
        return -1;
    }
    d->addr = out_ctx(d->slot, 0)[3] & 0xFF;     /* адрес выбирает контроллер */
    return 0;
}

static int x_control(usb_dev_t* d, uint8_t req_type, uint8_t req, uint16_t val,
                     uint16_t idx, uint16_t len, void* data, uint16_t* actual) {
    if (actual) *actual = 0;
    if (usb_hc_dead) return XFER_DEAD;
    if (!d->slot || len > sizeof(xdma)) return XFER_ERROR;
    int is_in = (req_type & 0x80) != 0;
    xring_t* r = &ep0_ring[d->slot];

    /* TRT (тип стадии данных) есть только с версии 1.0 */
    uint32_t trt = (xc.version >= 0x100 && len) ? (is_in ? 3 : 2) : 0;
    uint32_t t_setup = (uint32_t)ring_push(r, req_type | ((uint32_t)req << 8) | ((uint32_t)val << 16),
                                           idx | ((uint32_t)len << 16), 8,
                                           TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT(trt));
    uint32_t t_data = 0;
    if (len) {
        if (!is_in && data) memcpy(xdma, data, len);
        t_data = (uint32_t)ring_push(r, (uint32_t)xdma, 0, len,
                                     TRB_TYPE(TRB_DATA) | (is_in ? (TRB_DIR_IN | TRB_ISP) : 0));
    }
    /* статус-стадия идёт в обратную сторону (без данных — IN) */
    uint32_t t_status = (uint32_t)ring_push(r, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                                            ((len && is_in) ? 0 : TRB_DIR_IN));
    xhci_doorbell(d->slot, 1);                   /* target 1 = EP0 */

    uint32_t left = 0;
    int res = xhci_wait_xfer(d->slot, 1, t_setup, t_data, t_status, CTRL_TIMEOUT_MS, &left);
    if (res == XFER_OK && len) {
        uint32_t got = len - (left > len ? len : left);
        if (is_in && data) memcpy(data, xdma, got);
        if (actual) *actual = (uint16_t)got;
    }
    if (res != XFER_OK && res != XFER_DEAD) xhci_ep_recover(d->slot, 1, r);
    return res;
}

/* Добавить (и/или убрать) bulk-endpoint'ы флешки: Configure Endpoint. */
static int xhci_config_bulk_eps(usb_dev_t* d, uint32_t drop, uint32_t add) {
    memset(in_ctx, 0, sizeof(in_ctx));
    in_ctx_at(0)[0] = drop;
    in_ctx_at(0)[1] = add | 1;                   /* slot context — ради Context Entries */
    uint32_t* s = in_ctx_at(1);
    memcpy(s, out_ctx(d->slot, 0), 16);
    uint32_t entries = bulk_dci[0] > bulk_dci[1] ? bulk_dci[0] : bulk_dci[1];
    s[0] = (s[0] & ~(0x1Fu << 27)) | (entries << 27);
    for (int i = 0; i < 2; i++) {
        if (!(add & (1u << bulk_dci[i]))) continue;
        uint32_t* e = in_ctx_at(bulk_dci[i] + 1u);
        e[0] = 0;
        e[1] = (3u << 1) | ((uint32_t)(i ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT) << 3) |
               ((uint32_t)bulk_burst[i] << 8) | ((uint32_t)bulk_mps[i] << 16);
        e[2] = ring_pos(&bulk_ring[i]);
        e[3] = 0;
        e[4] = 3072;                             /* средняя длина TRB для bulk (xHCI 4.14.1.1) */
    }
    int cc = xhci_cmd((uint32_t)in_ctx, TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), 0);
    if (cc != CC_SUCCESS && cc >= 0)
        kprintw("usb: port %s: Configure Endpoint failed (code %d)\n", usb_path(d), cc);
    return cc == CC_SUCCESS ? 0 : -1;
}

static int x_config_bulk(usb_dev_t* d, uint8_t in, uint16_t in_mps, uint8_t in_burst,
                         uint8_t out, uint16_t out_mps, uint8_t out_burst) {
    if (!d->slot) return -1;
    bulk_slot = d->slot;
    bulk_dci[0] = (uint8_t)(out * 2);            /* DCI: EP n OUT = 2n, IN = 2n + 1 */
    bulk_dci[1] = (uint8_t)(in * 2 + 1);
    bulk_mps[0] = out_mps;
    bulk_mps[1] = in_mps;
    bulk_burst[0] = out_burst;
    bulk_burst[1] = in_burst;
    bulk_toggle_clean[0] = bulk_toggle_clean[1] = 0;
    ring_init(&bulk_ring[0], bulk_trbs[0], BULK_RING_TRBS);
    ring_init(&bulk_ring[1], bulk_trbs[1], BULK_RING_TRBS);
    if (xhci_config_bulk_eps(d, 0, (1u << bulk_dci[0]) | (1u << bulk_dci[1])) != 0) {
        bulk_slot = 0;
        return -1;
    }
    return 0;
}

static int x_bulk(usb_dev_t* d, int is_in, void* data, uint32_t len, uint32_t* actual) {
    if (actual) *actual = 0;
    if (usb_hc_dead) return XFER_DEAD;
    if (!d->slot || d->slot != bulk_slot || len > sizeof(xdma)) return XFER_ERROR;
    int i = is_in ? 1 : 0;
    xring_t* r = &bulk_ring[i];

    if (!is_in && data && len) memcpy(xdma, data, len);
    uint32_t t = (uint32_t)ring_push(r, (uint32_t)xdma, 0, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    xhci_doorbell(d->slot, bulk_dci[i]);

    uint32_t left = 0;
    int res = xhci_wait_xfer(d->slot, bulk_dci[i], t, t, t, BULK_TIMEOUT_MS, &left);
    if (res == XFER_OK) {
        uint32_t done = len - (left > len ? len : left);
        if (is_in && data && done) memcpy(data, xdma, done);
        if (actual) *actual = done;
        bulk_toggle_clean[i] = 0;
    } else if (res != XFER_DEAD) {
        bulk_toggle_clean[i] = (uint8_t)xhci_ep_recover(d->slot, bulk_dci[i], r);
    }
    return res;
}

/* После CLEAR_FEATURE(ENDPOINT_HALT) toggle устройства = DATA0. У нас его
 * сбрасывает Reset Endpoint (после STALL он уже был), а у работающего
 * endpoint'а — только пересоздание: Configure Endpoint с drop + add (так же
 * делает Linux). */
static void x_reset_bulk(usb_dev_t* d, int is_in) {
    int i = is_in ? 1 : 0;
    if (!d->slot || d->slot != bulk_slot) return;
    if (bulk_toggle_clean[i]) {
        bulk_toggle_clean[i] = 0;
        return;
    }
    xhci_cmd(0, TRB_TYPE(TRB_STOP_EP) | TRB_EP(bulk_dci[i]) | TRB_SLOT(d->slot), 0);
    xhci_config_bulk_eps(d, 1u << bulk_dci[i], 1u << bulk_dci[i]);
}

/* Устройство — хаб: контроллеру нужны число портов и TT think time, чтобы
 * обслуживать устройства за ним (xHCI 6.2.2). */
static int x_hub_config(usb_dev_t* hub, uint8_t nports, uint8_t ttt, int mtt) {
    if (!hub->slot) return -1;
    memset(in_ctx, 0, sizeof(in_ctx));
    in_ctx_at(0)[1] = 1;                         /* только slot context */
    uint32_t* s = in_ctx_at(1);
    memcpy(s, out_ctx(hub->slot, 0), 16);
    s[0] |= (1u << 26) | (mtt ? (1u << 25) : 0); /* Hub, MTT */
    s[1] = (s[1] & 0x00FFFFFF) | ((uint32_t)nports << 24);
    if (hub->speed == USB_SPEED_HIGH)
        s[2] = (s[2] & ~(3u << 16)) | ((uint32_t)(ttt & 3) << 16);
    /* у xHCI 0.95 эти поля принимает Evaluate Context, позже — Configure Endpoint */
    uint32_t type = xc.version > 0x95 ? TRB_CONFIG_EP : TRB_EVAL_CTX;
    return xhci_cmd((uint32_t)in_ctx, TRB_TYPE(type) | TRB_SLOT(hub->slot), 0) == CC_SUCCESS ? 0 : -1;
}

static const usb_hc_t xhci_ops = {
    .name        = "xHCI",
    .dev_open    = x_dev_open,
    .dev_close   = x_dev_close,
    .set_mps0    = x_set_mps0,
    .set_address = x_set_address,
    .control     = x_control,
    .config_bulk = x_config_bulk,
    .bulk        = x_bulk,
    .reset_bulk  = x_reset_bulk,
    .hub_config  = x_hub_config,
};

/* -------------------------------------------------------------------------- */
/* Корневые порты                                                             */
/* -------------------------------------------------------------------------- */
static uint8_t port_speed(uint32_t p, uint32_t v) {
    switch (PORT_SPEED(v)) {                     /* Protocol Speed ID по умолчанию (xHCI 7.2.2.1.1) */
        case 1:  return USB_SPEED_FULL;
        case 2:  return USB_SPEED_LOW;
        case 3:  return USB_SPEED_HIGH;
        case 0:  return port_proto[p] == 3 ? USB_SPEED_SUPER : USB_SPEED_HIGH;
        default: return USB_SPEED_SUPER;         /* 4 = 5 Гбит/с, 5+ = 10 Гбит/с и выше */
    }
}

/* Сброс порта. 1 — порт включён, в *speed скорость устройства. Порт USB 3
 * после HCRST обычно уже включён, но устройство на нём могло остаться в
 * настроенном BIOS состоянии — сбрасываем всегда (hot reset; если канал в
 * Inactive/Compliance или hot reset не помог — warm reset). */
static int xhci_port_reset(uint32_t p, uint8_t* speed) {
    volatile uint32_t* reg = portsc(p);
    uint32_t v = *reg;
    if (!(v & PORT_CCS)) return 0;
    int usb3 = port_proto[p] == 3;
    int warm = usb3 && (PORT_PLS(v) == PLS_INACTIVE || PORT_PLS(v) == PLS_COMPLIANCE);

    for (int attempt = 0; attempt < 2; attempt++) {
        *reg = (v & PORT_KEEP) | (warm ? PORT_WPR : PORT_PR);
        int done = 0;
        for (uint32_t ms = 0; ms < PORT_RESET_TIMEOUT_MS; ms++) {
            usb_mdelay(1);
            v = *reg;
            if (!(v & PORT_PR) && (v & (PORT_PRC | PORT_WRC))) { done = 1; break; }
        }
        *reg = (v & PORT_KEEP) | (v & PORT_CHANGES);   /* сбросить биты изменений */
        v = *reg;
        if (!(v & PORT_CCS)) return 0;           /* устройство отключили */
        if (done && (v & PORT_PED)) {
            *speed = port_speed(p, v);
            usb_mdelay(10);                      /* TRSTRCY */
            return 1;
        }
        if (!usb3 || warm) break;
        warm = 1;
    }
    kprintd("usb: port %d: port reset failed (PORTSC %x)\n", p, v);
    return 0;
}

/* Устройство в корневом порту: сброс, слот, адрес, опознание. 1 — usb0 найден. */
static int xhci_port_probe(uint32_t p, int (*accept)(blockdev_t*)) {
    for (int attempt = 0; attempt < 2 && !usb_hc_dead; attempt++) {
        uint8_t speed = USB_SPEED_HIGH;
        if (!xhci_port_reset(p, &speed)) return 0;
        if (speed == USB_SPEED_LOW) {            /* клавиатура/мышь: флешкой быть не может */
            kprintd("usb: port %d: low-speed device, skipped\n", p);
            return 0;
        }
        usb_dev_t* d = usb_alloc_dev(0, (uint8_t)p, speed);
        if (!d) return 0;
        if (usb_enumerate(d) == 0) return usb_attach(d, accept);
        kprintd("usb: port %d: %s-speed device does not answer\n", p, usb_speed_name(speed));
    }
    return 0;
}

/* Обход корневых портов. Каналы USB 3 после сброса контроллера поднимаются
 * не сразу, поэтому пустые порты ещё ROOT_CONNECT_WINDOW_MS опрашиваем. */
static int xhci_scan_ports(int (*accept)(blockdev_t*)) {
    static uint8_t pending[256];
    uint32_t left = 0;
    for (uint32_t p = 1; p <= xc.max_ports; p++) {
        pending[p] = 1;
        left++;
    }
    for (uint32_t waited = 0; ; waited += ROOT_POLL_MS) {
        for (uint32_t p = 1; p <= xc.max_ports && !usb_hc_dead; p++) {
            if (!pending[p]) continue;
            uint32_t v = *portsc(p);
            if (!(v & PORT_CCS)) {
                /* канал USB 3 не поднялся (Inactive/Compliance или cold attach) —
                 * один раз warm reset, дальше ждём подключения как обычно */
                if (port_proto[p] == 3 && pending[p] == 1 && ((v & PORT_CAS) ||
                    PORT_PLS(v) == PLS_INACTIVE || PORT_PLS(v) == PLS_COMPLIANCE)) {
                    *portsc(p) = (v & PORT_KEEP) | PORT_WPR;
                    pending[p] = 2;
                }
                continue;
            }
            pending[p] = 0;
            left--;
            if (v & PORT_OCA) {
                kprintw("usb: port %d: over-current, skipped\n", p);
                continue;
            }
            if (xhci_port_probe(p, accept)) return 1;
        }
        if (!left || usb_hc_dead || waited >= ROOT_CONNECT_WINDOW_MS) break;
        usb_mdelay(ROOT_POLL_MS);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Захват, запуск и остановка контроллера                                     */
/* -------------------------------------------------------------------------- */
/* отобразить BAR0 хотя бы до смещения end */
static void xhci_map_upto(uint32_t end) {
    if (end <= xc.mapped) return;
    uint32_t size = (end + 0xFFFFu) & ~0xFFFFu;  /* кусками по 64 КБ */
    usb_map_mmio((uint32_t)xc.cap, size);
    xc.mapped = size;
}

static void xhci_bios_handoff(volatile uint32_t* cap) {
    uint32_t v = cap[0];
    cap[0] = v | LEGACY_OS_OWNED;
    if (v & LEGACY_BIOS_OWNED) {
        for (int ms = 0; ms < 1000 && (cap[0] & LEGACY_BIOS_OWNED); ms += 10) usb_mdelay(10);
        if (cap[0] & LEGACY_BIOS_OWNED) {
            kprintw("usb: BIOS does not release xHCI %x:%x.%x, taking it by force\n", xc.bus, xc.dev, xc.fn);
            cap[0] = (cap[0] & ~LEGACY_BIOS_OWNED) | LEGACY_OS_OWNED;
        }
    }
    /* выключаем SMI от контроллера и сбрасываем их события, как Linux */
    cap[1] = (cap[1] & LEGACY_SMI_KEEP) | LEGACY_SMI_EVENTS;
}

/* Extended capabilities: захват у BIOS и типы корневых портов. */
static void xhci_parse_caps(uint32_t hccparams1) {
    memset(port_proto, 0, sizeof(port_proto));
    memset(port_slot_type, 0, sizeof(port_slot_type));
    uint32_t off = HCC1_XECP(hccparams1) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        xhci_map_upto(off + 16);
        volatile uint32_t* c = (volatile uint32_t*)(xc.cap + off);
        uint32_t v = c[0];
        if ((v & 0xFF) == XECP_LEGACY) {
            xhci_bios_handoff(c);
        } else if ((v & 0xFF) == XECP_PROTOCOL) {
            uint8_t  major = (uint8_t)(v >> 24);
            uint32_t first = c[2] & 0xFF, count = (c[2] >> 8) & 0xFF;
            for (uint32_t p = first; p < first + count && p < 256; p++) {
                port_proto[p] = major;
                port_slot_type[p] = (uint8_t)(c[3] & 0x1F);
            }
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

static int xhci_setup_scratchpads(void) {
    uint32_t n = xc.scratchpads;
    dcbaa_set(0, 0);
    if (!n) return 0;
    if (n > sp_allocated) {
        uint8_t*  pages = (uint8_t*)kmalloc_aligned_reserved(n * 4096, 4096);
        uint32_t* array = (uint32_t*)kmalloc_aligned_reserved((n * 8 + 4095) & ~4095u, 4096);
        if (!pages || !array) {
            kprintw("usb: xHCI %x:%x.%x: no memory for %d scratchpad pages\n", xc.bus, xc.dev, xc.fn, n);
            if (pages) kfree(pages);
            if (array) kfree(array);
            return -1;
        }
        if (sp_pages) kfree(sp_pages);
        if (sp_array) kfree(sp_array);
        sp_pages = pages;
        sp_array = array;
        sp_allocated = n;
    }
    memset(sp_pages, 0, n * 4096);
    for (uint32_t i = 0; i < n; i++) {
        sp_array[2 * i] = (uint32_t)(sp_pages + i * 4096);
        sp_array[2 * i + 1] = 0;
    }
    dcbaa_set(0, (uint32_t)sp_array);
    return 0;
}

/* Флешки нет: просто останавливаем контроллер (дальше он к памяти не
 * обращается). BIOS его обратно не получает — см. drivers/ehci.c. */
static void xhci_stop(void) {
    op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) & ~(CMD_RUN | CMD_INTE));
    op_wait(XHCI_USBSTS, STS_HCH, STS_HCH, 32);
}

/* Сброс и запуск контроллера (xHCI 4.2). 0 — контроллер работает. */
static int xhci_start(void) {
    if (op_wait(XHCI_USBSTS, STS_CNR, 0, 2000) != 0) {
        xhci_fail("not ready");
        return -1;
    }
    /* HCRST можно ставить только остановленному контроллеру */
    op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) & ~(CMD_RUN | CMD_INTE | CMD_HSEE));
    if (op_wait(XHCI_USBSTS, STS_HCH, STS_HCH, 100) != 0) {
        xhci_fail("does not halt");
        return -1;
    }
    op_write(XHCI_USBCMD, CMD_HCRST);
    usb_mdelay(1);                               /* Intel: сразу после HCRST регистры не трогать */
    if (op_wait(XHCI_USBCMD, CMD_HCRST, 0, 2000) != 0 || op_wait(XHCI_USBSTS, STS_CNR, 0, 2000) != 0) {
        xhci_fail("reset does not complete");
        return -1;
    }
    if (!(op_read(XHCI_PAGESIZE) & 1)) {
        xhci_fail("4 KB pages are not supported");
        return -1;
    }

    op_write(XHCI_CONFIG, (op_read(XHCI_CONFIG) & ~0xFFu) | xc.max_slots);

    memset((void*)dcbaa, 0, sizeof(dcbaa));
    if (xhci_setup_scratchpads() != 0) return -1;
    op_write64(XHCI_DCBAAP, (uint32_t)dcbaa);

    ring_init(&cmd_ring, cmd_trbs, CMD_RING_TRBS);
    op_write64(XHCI_CRCR, (uint32_t)cmd_trbs | CRCR_RCS);

    /* кольцо событий: один сегмент; порядок записи — ERSTSZ, ERDP, ERSTBA */
    memset((void*)evt_trbs, 0, sizeof(evt_trbs));
    evt_deq = 0;
    evt_cycle = 1;
    erst[0] = (uint32_t)evt_trbs;
    erst[1] = 0;
    erst[2] = EVT_RING_TRBS;
    erst[3] = 0;
    ir_write(IR_IMAN, IMAN_IP);                  /* прерывания выключены, сбросить ожидающее */
    ir_write(IR_ERSTSZ, 1);
    ir_write64(IR_ERDP, (uint32_t)evt_trbs);
    ir_write64(IR_ERSTBA, (uint32_t)erst);

    op_write(XHCI_USBSTS, STS_HSE | STS_EINT | STS_PCD | STS_SRE);
    op_write(XHCI_USBCMD, CMD_RUN);
    if (op_wait(XHCI_USBSTS, STS_HCH, 0, 100) != 0) {
        xhci_fail("does not start");
        return -1;
    }

    if (*(volatile uint32_t*)(xc.cap + XHCI_HCCPARAMS1) & HCC1_PPC) {
        for (uint32_t p = 1; p <= xc.max_ports; p++) {
            uint32_t v = *portsc(p);
            if (!(v & PORT_PP)) *portsc(p) = (v & PORT_KEEP) | PORT_PP;
        }
        usb_mdelay(20);
    }
    usb_mdelay(100);                             /* подключение устройств, каналы USB 3 */
    return 0;
}

/* Пробуем один контроллер. 1 — флешка найдена и принята (контроллер остаётся за нами). */
int xhci_probe(uint8_t bus, uint8_t dev, uint8_t fn, int (*accept)(blockdev_t*)) {
    uint32_t bar0 = pci_read_config_32(bus, dev, fn, 0x10);
    if (bar0 & 0x01) return 0;                       /* I/O BAR — не xHCI */
    if ((bar0 & 0x06) == 0x04 && pci_read_config_32(bus, dev, fn, 0x14) != 0) {
        kprintw("usb: xHCI %x:%x.%x is mapped above 4 GB, skipped\n", bus, dev, fn);
        return 0;
    }
    bar0 &= 0xFFFFFFF0;
    if (!bar0) return 0;

    memset(&xc, 0, sizeof(xc));
    xc.bus = bus; xc.dev = dev; xc.fn = fn;
    xc.cap = (volatile uint8_t*)bar0;
    usb_bus_begin(&xhci_ops);
    bulk_slot = 0;

    uint32_t cmd = pci_read_config_32(bus, dev, fn, 0x04);
    pci_write_config_32(bus, dev, fn, 0x04, (cmd & 0xFFFF) | 0x06); /* Mem Space + Bus Master */
    xhci_map_upto(0x1000);

    /* CAPLENGTH и HCIVERSION читаем одним dword'ом, как Linux: не все
     * контроллеры любят байтовые обращения к MMIO */
    uint32_t cap0 = *(volatile uint32_t*)(xc.cap + XHCI_CAPLENGTH);
    uint8_t caplength = cap0 & 0xFF;
    xc.version = (uint16_t)(cap0 >> 16);
    kprintd("usb: found xHCI %x.%2x at %x:%x.%x (BAR0: 0x%x)\n",
            xc.version >> 8, xc.version & 0xFF, bus, dev, fn, bar0);
    if (caplength < 0x20 || caplength == 0xFF) {
        kprintw("usb: xHCI %x:%x.%x: bad CAPLENGTH %x, skipped\n", bus, dev, fn, caplength);
        return 0;
    }

    uint32_t hcs1 = *(volatile uint32_t*)(xc.cap + XHCI_HCSPARAMS1);
    uint32_t hcs2 = *(volatile uint32_t*)(xc.cap + XHCI_HCSPARAMS2);
    uint32_t hcc1 = *(volatile uint32_t*)(xc.cap + XHCI_HCCPARAMS1);
    uint32_t dboff = *(volatile uint32_t*)(xc.cap + XHCI_DBOFF) & ~0x3u;
    uint32_t rtsoff = *(volatile uint32_t*)(xc.cap + XHCI_RTSOFF) & ~0x1Fu;
    xc.max_ports = HCS1_MAX_PORTS(hcs1);
    xc.max_slots = HCS1_MAX_SLOTS(hcs1);
    if (xc.max_slots > XHCI_MAX_SLOTS) xc.max_slots = XHCI_MAX_SLOTS;
    xc.ctx_size = (hcc1 & HCC1_CSZ) ? 64 : 32;
    xc.scratchpads = HCS2_SCRATCHPADS(hcs2);
    if (!xc.max_slots || !xc.max_ports) {
        kprintw("usb: xHCI %x:%x.%x: no ports or slots, skipped\n", bus, dev, fn);
        return 0;
    }

    /* все нужные регистры: порты, interrupter 0, doorbell'ы */
    uint32_t end = caplength + XHCI_PORTSC(xc.max_ports) + 0x10;
    if (rtsoff + XHCI_IR0 + 0x20 > end) end = rtsoff + XHCI_IR0 + 0x20;
    if (dboff + 4 * (xc.max_slots + 1) > end) end = dboff + 4 * (xc.max_slots + 1);
    xhci_map_upto(end);
    xc.op = xc.cap + caplength;
    xc.ir0 = xc.cap + rtsoff + XHCI_IR0;
    xc.db = (volatile uint32_t*)(xc.cap + dboff);

    xhci_parse_caps(hcc1);                       /* в т.ч. захват у BIOS */

    int usb3 = 0;
    for (uint32_t p = 1; p <= xc.max_ports; p++) if (port_proto[p] == 3) usb3++;
    kprintd("usb: xHCI: %d ports (USB 3: %d), %d slots, ctx %d, scratchpads %d\n",
            xc.max_ports, usb3, xc.max_slots, xc.ctx_size, xc.scratchpads);

    if (xhci_start() != 0) {
        xhci_stop();
        return 0;
    }

    if (xhci_scan_ports(accept)) return 1;

    xhci_stop();
    kprintd("usb: xHCI %x:%x.%x: no boot disk (devices: %d), controller stopped\n",
            bus, dev, fn, usb_bus_identified());
    return 0;
}
