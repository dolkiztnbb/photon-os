/*
 * drivers/usb.c — 32-битный драйвер USB 2.0 EHCI + USB Mass Storage (BOT / SCSI)
 * Работает полностью в защищённом режиме через DMA и MMIO.
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

#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_SET_CONFIG      0x09

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_ENDPOINT       0x05

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY      0x25
#define SCSI_READ10             0x28
#define SCSI_WRITE10            0x2A

#define CBW_SIGNATURE           0x43425355 // "USBC"
#define CSW_SIGNATURE           0x53425355 // "USBS"

/* EHCI qTD - выравнивание 32 байта */
typedef struct __attribute__((packed, aligned(32))) {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qtd_t;

/* EHCI QH - выравнивание 64 байта */
typedef struct __attribute__((packed, aligned(64))) {
    uint32_t qhlp;
    uint32_t ep_char;
    uint32_t ep_cap;
    uint32_t current_qtd;
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
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

/* ВАЖНО: Буферы должны быть выровнены по размеру страницы (4096) */
static ehci_qh_t  qh_pool[4]   __attribute__((aligned(4096)));
static ehci_qtd_t qtd_pool[8]  __attribute__((aligned(4096)));
static uint8_t    dma_buffer[4096] __attribute__((aligned(4096)));

static volatile uint8_t* ehci_base = 0;
static volatile uint8_t* ehci_op   = 0;

static uint8_t usb_dev_addr = 1;
static uint8_t ep_in = 0;
static uint8_t ep_out = 0;
static uint32_t usb_total_sectors = 0;
static char usb_model[41] = "USB 2.0 Flash Drive";
static uint32_t bot_tag_counter = 0x1000;

static void io_delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms * 1000; i++) {
        port_byte_in(0x80);
    }
}

static void ehci_write_op32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(ehci_op + reg) = val;
}

static uint32_t ehci_read_op32(uint32_t reg) {
    return *(volatile uint32_t*)(ehci_op + reg);
}

/* -------------------------------------------------------------------------- */
/* Правильное выполнение передачи с сохранением Data Toggle                   */
/* -------------------------------------------------------------------------- */
static int ehci_exec_transfer(ehci_qh_t* qh, ehci_qtd_t* first_qtd, ehci_qtd_t* last_qtd, uint32_t timeout_ms) {
    uint32_t cmd = ehci_read_op32(0x00);
    if (cmd & (1 << 5)) {
        ehci_write_op32(0x00, cmd & ~(1 << 5));
        while (ehci_read_op32(0x04) & (1 << 5)); 
    }

    ehci_write_op32(0x04, 0x3F);

    // Загружаем qTD в QH, сохраняя бит Data Toggle (бит 31)
    qh->current_qtd = 0;
    qh->next_qtd = (uint32_t)first_qtd;
    qh->token &= ~0xFF; 
    
    ehci_write_op32(0x18, (uint32_t)qh);

    ehci_write_op32(0x00, ehci_read_op32(0x00) | (1 << 0) | (1 << 5));
    while (!(ehci_read_op32(0x04) & (1 << 5)));

    int status = -2; 
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        uint32_t tok = last_qtd->token;
        if (!(tok & (1 << 7))) { 
            if (tok & 0x7E) status = -1;
            else            status = 0;
            break;
        }
        port_byte_in(0x80);
    }

    cmd = ehci_read_op32(0x00);
    ehci_write_op32(0x00, cmd & ~(1 << 5));
    while (ehci_read_op32(0x04) & (1 << 5));

    ehci_write_op32(0x04, 0x3F);

    return status;
}

/* -------------------------------------------------------------------------- */
/* Control Transfer (Endpoint 0)                                              */
/* -------------------------------------------------------------------------- */
static int usb_control_msg(uint8_t dev, uint8_t req_type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void* data) {
    static uint8_t setup_pkt[8] __attribute__((aligned(16)));
    setup_pkt[0] = req_type;
    setup_pkt[1] = req;
    setup_pkt[2] = val & 0xFF;
    setup_pkt[3] = (val >> 8) & 0xFF;
    setup_pkt[4] = idx & 0xFF;
    setup_pkt[5] = (idx >> 8) & 0xFF;
    setup_pkt[6] = len & 0xFF;
    setup_pkt[7] = (len >> 8) & 0xFF;

    ehci_qh_t* qh = &qh_pool[0];
    memset(qh, 0, sizeof(ehci_qh_t));
    qh->qhlp = ((uint32_t)qh) | 0x02; 
    // Control Endpoint (DTC = 1)
    qh->ep_char = (dev & 0x7F) | (2 << 12) | (1 << 14) | (1 << 15) | (64 << 16);
    qh->ep_cap = 0x40000000;

    ehci_qtd_t* qtd_setup = &qtd_pool[0];
    ehci_qtd_t* qtd_data  = &qtd_pool[1];
    ehci_qtd_t* qtd_stat  = &qtd_pool[2];
    memset(qtd_pool, 0, sizeof(qtd_pool));

    qtd_setup->next_qtd = (len > 0) ? (uint32_t)qtd_data : (uint32_t)qtd_stat;
    qtd_setup->alt_next_qtd = 1;
    qtd_setup->token = (2 << 8) | (3 << 10) | (8 << 16) | (1 << 7); 
    qtd_setup->buffer[0] = (uint32_t)setup_pkt;

    ehci_qtd_t* last = qtd_stat;

    if (len > 0) {
        if (data && !(req_type & 0x80)) memcpy(dma_buffer, data, len);
        qtd_data->next_qtd = (uint32_t)qtd_stat;
        qtd_data->alt_next_qtd = 1;
        uint32_t pid = (req_type & 0x80) ? 1 : 0;
        qtd_data->token = (pid << 8) | (3 << 10) | (1 << 31) | (len << 16) | (1 << 7); 
        qtd_data->buffer[0] = (uint32_t)dma_buffer;
    }

    qtd_stat->next_qtd = 1;
    qtd_stat->alt_next_qtd = 1;
    uint32_t stat_pid = (len > 0 && !(req_type & 0x80)) ? 1 : ((len > 0) ? 0 : 1);
    qtd_stat->token = (stat_pid << 8) | (3 << 10) | (1 << 31) | (1 << 7); 

    int res = ehci_exec_transfer(qh, qtd_setup, last, 1000);
    if (res == 0 && len > 0 && (req_type & 0x80) && data) {
        memcpy(data, dma_buffer, len);
    }
    return res;
}

/* -------------------------------------------------------------------------- */
/* Bulk Transfer (BOT) — Hardware Managed Data Toggle (DTC=0)                 */
/* -------------------------------------------------------------------------- */
static int usb_bulk_transfer(ehci_qh_t* qh, void* data, uint32_t len, int is_in) {
    if (len > sizeof(dma_buffer)) return -1;
    if (!is_in && data) {
        memcpy(dma_buffer, data, len);
    }

    ehci_qtd_t* qtd = &qtd_pool[3];
    memset(qtd, 0, sizeof(ehci_qtd_t));
    qtd->next_qtd = 1;
    qtd->alt_next_qtd = 1;

    uint32_t pid = is_in ? 1 : 0;
    // Data Toggle takes from QH overlay
    qtd->token = (pid << 8) | (3 << 10) | (len << 16) | (1 << 7);
    qtd->buffer[0] = (uint32_t)dma_buffer;

    int res = ehci_exec_transfer(qh, qtd, qtd, 2000); 
    if (res == 0 && is_in && data) {
        memcpy(data, dma_buffer, len);
    }
    return res;
}

/* -------------------------------------------------------------------------- */
/* SCSI Commands Wrapper                                                      */
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

    ehci_qh_t* qh_out = &qh_pool[1];
    ehci_qh_t* qh_in  = &qh_pool[2];

    if (usb_bulk_transfer(qh_out, &cbw, 31, 0) != 0) return -1;
    if (data_len > 0) {
        if (usb_bulk_transfer(is_in ? qh_in : qh_out, data, data_len, is_in) != 0) return -2;
    }
    
    usb_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    if (usb_bulk_transfer(qh_in, &csw, 13, 1) != 0) return -3;
    if (csw.signature != CSW_SIGNATURE || csw.status != 0) return -4;
    return 0;
}

static int usb_blk_read(blockdev_t* dev, uint32_t lba, void* buf) {
    (void)dev;
    uint8_t cmd[10];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = SCSI_READ10;
    cmd[2] = (lba >> 24) & 0xFF;
    cmd[3] = (lba >> 16) & 0xFF;
    cmd[4] = (lba >> 8) & 0xFF;
    cmd[5] = lba & 0xFF;
    cmd[8] = 1; 

    // QEMU sometimes needs a tiny delay between rapid read operations
    io_delay_ms(1);
    return (usb_bot_scsi(cmd, 10, buf, 512, 1) == 0) ? 0 : 1;
}

static int usb_blk_write(blockdev_t* dev, uint32_t lba, const void* buf) {
    (void)dev;
    uint8_t cmd[10];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = SCSI_WRITE10;
    cmd[2] = (lba >> 24) & 0xFF;
    cmd[3] = (lba >> 16) & 0xFF;
    cmd[4] = (lba >> 8) & 0xFF;
    cmd[5] = lba & 0xFF;
    cmd[8] = 1;

    io_delay_ms(1);
    return (usb_bot_scsi(cmd, 10, (void*)buf, 512, 0) == 0) ? 0 : 1;
}

/* -------------------------------------------------------------------------- */
/* Инициализация накопителя                                                   */
/* -------------------------------------------------------------------------- */
static int usb_init_mass_storage(void) {
    uint8_t buf[256];

    if (usb_control_msg(0, 0x00, USB_REQ_SET_ADDRESS, usb_dev_addr, 0, 0, NULL) != 0) return -1;
    io_delay_ms(20);

    if (usb_control_msg(usb_dev_addr, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 18, buf) != 0) return -1;
    if (usb_control_msg(usb_dev_addr, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, 255, buf) != 0) return -1;

    uint16_t total_len = buf[2] | (buf[3] << 8);
    uint16_t offset = buf[0];
    ep_in = 0; ep_out = 0;

    while (offset < total_len && offset < 250) {
        uint8_t len  = buf[offset];
        uint8_t type = buf[offset + 1];
        if (len == 0) break;
        if (type == USB_DESC_ENDPOINT) {
            uint8_t ep_addr = buf[offset + 2];
            uint8_t ep_attr = buf[offset + 3];
            if ((ep_attr & 0x03) == 0x02) { 
                if (ep_addr & 0x80) ep_in  = ep_addr & 0x0F;
                else                ep_out = ep_addr & 0x0F;
            }
        }
        offset += len;
    }

    if (!ep_in || !ep_out) return -1;

    if (usb_control_msg(usb_dev_addr, 0x00, USB_REQ_SET_CONFIG, 1, 0, 0, NULL) != 0) return -1;
    io_delay_ms(20);

    // Инициализация QH для Bulk Endpoints (DTC = 0)
    ehci_qh_t* qh_out = &qh_pool[1];
    memset(qh_out, 0, sizeof(ehci_qh_t));
    qh_out->qhlp = ((uint32_t)qh_out) | 0x02;
    qh_out->ep_char = (usb_dev_addr & 0x7F) | ((ep_out & 0x0F) << 8) | (2 << 12) | (1 << 14) | (512 << 16);
    qh_out->ep_cap = 0x40000000;
    qh_out->next_qtd = 1;

    ehci_qh_t* qh_in = &qh_pool[2];
    memset(qh_in, 0, sizeof(ehci_qh_t));
    qh_in->qhlp = ((uint32_t)qh_in) | 0x02;
    qh_in->ep_char = (usb_dev_addr & 0x7F) | ((ep_in & 0x0F) << 8) | (2 << 12) | (1 << 14) | (512 << 16);
    qh_in->ep_cap = 0x40000000;
    qh_in->next_qtd = 1;

    uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 10; i++) {
        if (usb_bot_scsi(tur, 6, NULL, 0, 0) == 0) break;
        io_delay_ms(100);
    }

    uint8_t inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq_resp[36];
    if (usb_bot_scsi(inq, 6, inq_resp, 36, 1) == 0) {
        char vid[9], pid[17];
        memcpy(vid, &inq_resp[8], 8);  vid[8] = '\0';
        memcpy(pid, &inq_resp[16], 16); pid[16] = '\0';
        snkprintf(usb_model, "%s %s", vid, pid);
    }

    uint8_t rc_cmd[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t rc_resp[8];
    if (usb_bot_scsi(rc_cmd, 10, rc_resp, 8, 1) == 0) {
        uint32_t last_lba = (rc_resp[0] << 24) | (rc_resp[1] << 16) | (rc_resp[2] << 8) | rc_resp[3];
        usb_total_sectors = last_lba + 1;
    } else {
        usb_total_sectors = 4096;
    }

    blockdev_t dev;
    memset(&dev, 0, sizeof(dev));
    strncpy(dev.name, "usb0", BLK_NAME_LEN - 1);
    strncpy(dev.model, usb_model, sizeof(dev.model) - 1);
    dev.sector_size = 512;
    dev.total_sectors = usb_total_sectors;
    dev.flags = 0; 
    dev.read = usb_blk_read;
    dev.write = usb_blk_write;

    blockdev_register(&dev);
    kprinti("usb: mounted %s (%s), %d MB\n", dev.name, dev.model, dev.total_sectors / 2048);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Инициализация контроллера EHCI                                             */
/* -------------------------------------------------------------------------- */
void usb_init(void) {
    uint32_t bar0 = 0;
    uint8_t found_bus = 0, found_dev = 0, found_fn = 0;
    int found = 0;

    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t dev = 0; dev < 32 && !found; dev++) {
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
                    bar0 = pci_read_config_32((uint8_t)bus, dev, fn, 0x10) & 0xFFFFFFF0;
                    found_bus = (uint8_t)bus;
                    found_dev = dev;
                    found_fn = fn;
                    found = 1;
                    kprintd("usb: found EHCI at %x:%x.%x (BAR0: 0x%X)\n", bus, dev, fn, bar0);
                    break;
                }
            }
        }
    }

    if (!found || !bar0) {
        kprintd("usb: no EHCI controller found on PCI\n");
        return;
    }

    uint32_t cmd = pci_read_config_32(found_bus, found_dev, found_fn, 0x04);
    pci_write_config_32(found_bus, found_dev, found_fn, 0x04, cmd | 0x06); // Bus Master + Mem Space

    for (int i = 0; i < 4; i++) {
        vmm_identity_map_page(page_directory, bar0 + (i * 4096), 0x03);
    }

    ehci_base = (volatile uint8_t*)bar0;
    uint8_t caplength = ehci_base[0];
    ehci_op = ehci_base + caplength;

    ehci_write_op32(0x00, 0); 
    io_delay_ms(20);
    ehci_write_op32(0x00, (1 << 1)); 
    while (ehci_read_op32(0x00) & (1 << 1));

    ehci_write_op32(0x08, 0); 
    ehci_write_op32(0x10, 0); 
    ehci_write_op32(0x40, 1); 
    io_delay_ms(50);

    /* Запускаем контроллер (Run/Stop = 1): без этого HC остаётся в halted-состоянии
       и асинхронное расписание никогда не выполняется -> передачи «висят» до таймаута. */
    ehci_write_op32(0x00, ehci_read_op32(0x00) | (1 << 0));
    io_delay_ms(10);
    for (int _i = 0; _i < 100 && (ehci_read_op32(0x04) & (1 << 12)); _i++) {
        io_delay_ms(1);
    }

    uint32_t hcsparams = *(volatile uint32_t*)(ehci_base + 0x04);
    uint32_t n_ports = hcsparams & 0x0F;

    for (uint32_t p = 0; p < n_ports; p++) {
        uint32_t port_reg = 0x44 + (p * 4);
        uint32_t val = ehci_read_op32(port_reg);

        if (val & (1 << 0)) { 
            ehci_write_op32(port_reg, (val & ~0x2A) | (1 << 8));
            io_delay_ms(50);
            ehci_write_op32(port_reg, val & ~(0x2A | (1 << 8)));
            io_delay_ms(50);

            val = ehci_read_op32(port_reg);
            if (val & (1 << 2)) {
                kprintd("usb: High-Speed device connected to port %d\n", p);
                if (usb_init_mass_storage() == 0) break;
            }
        }
    }
}