#ifndef USB_HC_H
#define USB_HC_H

/*
 * drivers/usb_hc.h — внутренний интерфейс USB-стека ядра.
 *
 * Стек разделён на две части:
 *   - ядро стека (drivers/usb.c): поиск контроллеров, опознание устройств,
 *     USB-хабы и USB Mass Storage (BOT/SCSI) — не зависит от контроллера;
 *   - драйверы контроллеров: drivers/ehci.c (USB 2.0) и drivers/xhci.c
 *     (USB 3.x). Они запускают контроллер, обходят его корневые порты и
 *     выполняют передачи через операции usb_hc_t.
 *
 * Снаружи стека используйте drivers/usb.h.
 */

#include <libc/stdint>
#include <drivers/blockdev.h>

/* ---- стандартные запросы USB (USB 2.0, глава 9) ---------------------------- */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09

/* bmRequestType */
#define RT_STD_IN               0x80   /* стандартный, к устройству, IN */
#define RT_STD_OUT              0x00
#define RT_STD_EP_OUT           0x02   /* стандартный, к endpoint'у */
#define RT_CLASS_IFACE_OUT      0x21
#define RT_HUB_IN               0xA0   /* класс hub, к самому хабу */
#define RT_HUB_OUT              0x20
#define RT_PORT_IN              0xA3   /* класс hub, к порту хаба */
#define RT_PORT_OUT             0x23

/* Скорость устройства. Первые три значения совпадают с полем EPS в QH
 * EHCI. */
#define USB_SPEED_FULL          0
#define USB_SPEED_LOW           1
#define USB_SPEED_HIGH          2
#define USB_SPEED_SUPER         3      /* USB 3.x, только xHCI */

/* Результаты передачи */
#define XFER_OK                 0
#define XFER_STALL             (-1)   /* устройство ответило STALL */
#define XFER_ERROR             (-2)   /* ошибка на шине (CRC, babble, ...) */
#define XFER_TIMEOUT           (-3)
#define XFER_DEAD              (-4)   /* контроллер перестал работать */

#define CTRL_TIMEOUT_MS         500
#define BULK_TIMEOUT_MS         3000

#define USB_MAX_DEVICES         32     /* устройств на одном контроллере */
#define USB_MAX_HUB_DEPTH       5      /* до 5 хабов между контроллером и устройством (USB 2.0, 4.1.1) */

/* Устройство на шине.
 *
 * Full/Low-Speed устройства за High-Speed хабом работают через transaction
 * translator (TT) этого хаба: EHCI нужен адрес хаба и номер его порта
 * (EHCI 4.12), xHCI — номер слота хаба и тот же порт. */
typedef struct usb_dev {
    struct usb_dev* parent;      /* хаб, в который включено устройство; 0 — корневой порт */
    struct usb_dev* tt;          /* Full/Low-Speed за High-Speed хабом: этот хаб (его TT) */
    uint8_t  port;               /* порт: корневой (у EHCI с 0, у xHCI с 1) или хаба (с 1) */
    uint8_t  depth;              /* сколько хабов между контроллером и устройством */
    uint8_t  speed;              /* USB_SPEED_* */
    uint8_t  addr;               /* адрес на шине; 0 — ещё не назначен */
    uint16_t mps0;               /* максимальный размер пакета EP0 */
    uint8_t  tt_hub;             /* EHCI: адрес High-Speed хаба с TT */
    uint8_t  tt_port;            /* порт этого хаба */
    /* только для xHCI */
    uint8_t  slot;               /* номер слота устройства; 0 — нет */
    uint8_t  root_port;          /* корневой порт, через который подключено устройство */
    uint32_t route;              /* route string: порты хабов по 4 бита (xHCI 8.9) */
} usb_dev_t;

/* Операции контроллера. Необязательные можно оставить 0. */
typedef struct usb_hc {
    const char* name;            /* "EHCI" / "xHCI" — для сообщений */
    /* Новое устройство после сброса порта: подготовить EP0 по адресу 0. */
    int  (*dev_open)(usb_dev_t* d);
    /* Устройство больше не нужно (не ответило или не наше). */
    void (*dev_close)(usb_dev_t* d);
    /* Узнали настоящий размер пакета EP0 (d->mps0). */
    int  (*set_mps0)(usb_dev_t* d);
    /* Назначить устройству адрес (записывается в d->addr). */
    int  (*set_address)(usb_dev_t* d);
    /* Control-передача на EP0. */
    int  (*control)(usb_dev_t* d, uint8_t req_type, uint8_t req, uint16_t val,
                    uint16_t idx, uint16_t len, void* data, uint16_t* actual);
    /* Bulk-endpoint'ы флешки: номера, размер пакета и (USB 3) max burst.
     * Вызывается до SET_CONFIGURATION, toggle обоих endpoint'ов — DATA0. */
    int  (*config_bulk)(usb_dev_t* d, uint8_t in, uint16_t in_mps, uint8_t in_burst,
                        uint8_t out, uint16_t out_mps, uint8_t out_burst);
    /* Bulk-передача (не больше 4 КБ) на endpoint, настроенный config_bulk. */
    int  (*bulk)(usb_dev_t* d, int is_in, void* data, uint32_t len, uint32_t* actual);
    /* После CLEAR_FEATURE(ENDPOINT_HALT): вернуть endpoint контроллера в
     * исходное состояние (toggle -> DATA0). */
    void (*reset_bulk)(usb_dev_t* d, int is_in);
    /* Устройство — хаб: число портов, TT think time, multi-TT. */
    int  (*hub_config)(usb_dev_t* hub, uint8_t nports, uint8_t ttt, int mtt);
} usb_hc_t;

/* --- ядро стека (drivers/usb.c) --- */
extern int usb_hc_dead;          /* контроллер не отвечает: все передачи сразу с ошибкой */

/* Начало работы с очередным контроллером: сброс таблицы устройств. */
void        usb_bus_begin(const usb_hc_t* hc);
int         usb_bus_identified(void);    /* сколько устройств опознано на контроллере */
usb_dev_t*  usb_alloc_dev(usb_dev_t* parent, uint8_t port, uint8_t speed);
/* Адрес и дескрипторы только что сброшенного устройства. 0 — успех;
 * при неудаче устройство уже закрыто (dev_close). */
int         usb_enumerate(usb_dev_t* d);
/* Опознанное устройство: хаб — обход портов, флешка — попытка usb0.
 * 1 — usb0 найден; иначе устройство закрыто. */
int         usb_attach(usb_dev_t* d, int (*accept)(blockdev_t*));
const char* usb_path(const usb_dev_t* d);
const char* usb_speed_name(uint8_t speed);

/* Задержки: чтение порта 0x80 (~1 мкс), не зависят от прерываний. */
void        usb_udelay(uint32_t us);
void        usb_mdelay(uint32_t ms);
/* Байтовая запись в конфигурационное пространство PCI. */
void        usb_pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t val);
/* Отобразить регистры контроллера (MMIO) без кэширования. */
void        usb_map_mmio(uint32_t base, uint32_t size);

/* --- драйверы контроллеров --- */
/* 1 — флешка найдена и принята (контроллер остаётся за ядром) */
int ehci_probe(uint8_t bus, uint8_t dev, uint8_t fn, int (*accept)(blockdev_t*));
int xhci_probe(uint8_t bus, uint8_t dev, uint8_t fn, int (*accept)(blockdev_t*));

#endif /* USB_HC_H */
