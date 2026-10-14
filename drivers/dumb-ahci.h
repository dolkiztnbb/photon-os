#include <stdint.h>
#include <setup.h>

// Константы
#define SATA_SIG_ATA   0x00000101
#define FIS_TYPE_REG_H2D 0x27
#define HBA_PxIS_TFES   (1 << 30)

// --- СТРУКТУРЫ AHCI (БЕЗ ПРОПУСКОВ) ---

typedef struct {
    uint32_t clb;       uint32_t clbu;
    uint32_t fb;        uint32_t fbu;
    uint32_t is;        uint32_t ie;
    uint32_t cmd;       uint32_t rsv0;
    uint32_t tfd;       uint32_t sig;
    uint32_t ssts;      uint32_t sctl;
    uint32_t serr;      uint32_t sact;
    uint32_t ci;        uint32_t sntf;
    uint32_t fbs;       uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef struct {
    uint32_t cap;       uint32_t ghc;
    uint32_t is;        uint32_t pi;
    uint32_t vs;        uint32_t ccc_ctl;
    uint32_t ccc_pts;   uint32_t em_loc;
    uint32_t em_ctl;    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint8_t  cfl:5;     uint8_t  a:1;    uint8_t  w:1;    uint8_t  p:1;
    uint8_t  r:1;       uint8_t  b:1;    uint8_t  c:1;    uint8_t  rsv0:1;
    uint8_t  pmp:4;     uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;      uint32_t ctbau;
    uint32_t rsv1[4];
} hba_cmd_header_t;

typedef struct {
    uint32_t dba;       uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;    uint32_t rsv1:9;    uint32_t i:1;
} hba_prdt_entry_t;

typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    hba_prdt_entry_t prdt_entry[1]; // Читаем 1 блок данных
} hba_cmd_tbl_t;

// --- ФУНКЦИИ УПРАВЛЕНИЯ ---

// Остановка и запуск движка порта (нужно для смены адресов)
void port_stop(volatile hba_port_t *port) {
    port->cmd &= ~(1 << 0); // ST
    port->cmd &= ~(1 << 4); // FRE

    int timeout = 1000000;
    while (timeout--) {
        if (!(port->cmd & (1 << 15)) && !(port->cmd & (1 << 14))) {
            return; // Успешно остановились
        }
    }
}

void port_start(volatile hba_port_t *port) {
    while (port->cmd & (1 << 15)); // Ждем, пока CR сбросится
    port->cmd |= (1 << 4); // FRE
    port->cmd |= (1 << 0); // ST
}

// Поиск свободного слота команд (обычно просто берем 0)
int find_cmd_slot(volatile hba_port_t *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) return i;
        slots >>= 1;
    }
    return -1;
}

// Основная функция чтения
int ahci_read(volatile hba_port_t *port, uint32_t lba_low, uint32_t lba_high, uint32_t count, uint16_t *buf) {
    port->is = (uint32_t)-1; // Очистка статуса прерываний
    volatile int slot = find_cmd_slot(port);
    // kprintd("Slot %d",slot);
    if (slot == -1) return 0;
    // kprintd("Trying to read %X:%X",lba_low,lba_high);

    volatile hba_cmd_header_t *cmdheader = (hba_cmd_header_t*)(uintptr_t)port->clb;
    cmdheader += slot;
    cmdheader->cfl = 5;     // Длина FIS = 5 dwords (20 байт)
    cmdheader->w = 0;       // Чтение (0)
    cmdheader->prdtl = 1;   // 1 запись в таблице PRDT
    // kprintd("Header set\n");

    volatile hba_cmd_tbl_t *cmdtbl = (hba_cmd_tbl_t*)(uintptr_t)cmdheader->ctba;
    // Очистка таблицы команд
    for(int i = 0; i < sizeof(hba_cmd_tbl_t); i++) ((uint8_t*)cmdtbl)[i] = 0;

    // Настройка PRDT (указываем адрес нашего буфера в памяти)
    cmdtbl->prdt_entry[0].dba = (uint32_t)(uintptr_t)buf;
    cmdtbl->prdt_entry[0].dbc = (count << 9) - 1; // count * 512 байт
    cmdtbl->prdt_entry[0].i = 1; // Прерывание по завершению
    // kprintd("PRDT set\n");

    // Формируем FIS типа 0x27 (Host to Device)
    volatile uint8_t *cfis = cmdtbl->cfis;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = (1 << 7);     // Это команда
    cfis[2] = 0x25;         // READ DMA EXT (LBA48)
    // kprintd("FIS-ll set\n");
    
    cfis[4] = (uint8_t)lba_low;
    cfis[5] = (uint8_t)(lba_low >> 8);
    cfis[6] = (uint8_t)(lba_low >> 16);
    cfis[7] = (1 << 6);     // LBA Mode
    // kprintd("FIS-lh set");
    
    cfis[8] = (uint8_t)(lba_low >> 24);
    cfis[9] = (uint8_t)lba_high;
    cfis[10] = (uint8_t)(lba_high >> 8);
    // kprintd("FIS-hl set");
    
    cfis[12] = (uint8_t)count;
    cfis[13] = (uint8_t)(count >> 8);
    // kprintd("FIS-hh set\n");

    // Ждем готовности (не более 1 сек)
    volatile int timeout = 0;
    while ((port->tfd & (0x80 | 0x08)) && timeout < 1000000) outb(0x80,0);
    if (timeout >= 1000000) return 0;
    // kprintd("Timeout OK\n");

    port->ci = (1 << slot); // ЗАПУСК!

    for(int i=0;i<10;i++)outb(0x80,0);
    // Ожидание завершения
    while (1) {
        if(!(port->ci & (1 << slot))) break;
        if (port->is & HBA_PxIS_TFES) return 0; // Ошибка транзакции
    }
    // kprintd("Transaction OK\n");

    return 1;
}

volatile uint32_t   __pht_ahci_port=0;
volatile hba_mem_t* __pht_hba=0;

// --- ТОЧКА ВХОДА ИЗ ВАШЕГО ЯДРА ---

uint8_t initialize_ahci_and_read(uint32_t abar) {
    volatile hba_mem_t *hba = (hba_mem_t*)(uintptr_t)abar;
    
    // 1. Ищем порт
    int port_idx = -1;
    for (int i = 0; i < 32; i++) {
        if (hba->pi & (1 << i)) {
            uint32_t ssts = hba->ports[i].ssts;
            uint8_t det = ssts & 0x0F;
			
            if (det == 0x03 && hba->ports[i].sig == SATA_SIG_ATA) {
                port_idx = i;
                break;
            }
        }
    }

    if (port_idx == -1) return -1; // Диск не найден

    hba->ghc |= (1 << 31); // AE (AHCI Enable)

    // 2. Делаем полный сброс контроллера
    hba->ghc |= (1 << 0);  // HR (HBA Reset)
    while (hba->ghc & 1);  // Ждем, пока бит сбросится сам (обычно < 1 мс)

    // 3. После сброса AE выключается, включаем его снова
    hba->ghc |= (1 << 31); 

    __pht_ahci_port = port_idx;
    __pht_hba = hba;
    volatile hba_port_t *port = &hba->ports[__pht_ahci_port];

    // 2. Инициализация памяти для команд (Адреса должны быть выровнены!)
    port_stop(port);

    // ВАЖНО: Эти адреса должны быть свободны! 
    port->clb  = __PHT_MEM_AHCI_CMD_LIST; 
    port->fb   = __PHT_MEM_AHCI_FIS;
    
    // Заголовки команд (32 штуки по 32 байта каждая)
    hba_cmd_header_t *hdr = (hba_cmd_header_t*)__PHT_MEM_AHCI_CMD_LIST;
    for (int i = 0; i < 32; i++) {
        hdr[i].prdtl = 1;
        hdr[i].ctba = __PHT_MEM_AHCI_CMDTBL + (i * 0x1000); // Таблица команд для каждого слота
    }
    port_start(port);

    // 3. ЧТЕНИЕ СЕКТОРА
    uint16_t *buf = (uint16_t*)__PHT_MEM_AHCI_BUFFER; // Куда читать (4MB)
    if (ahci_read(port, 0, 0, 1, buf)) {
        if(ahci_read(port, 0, 0, 1, buf)){
            // Успех! В buf[0] и buf[1] лежат первые байты вашего MBR (должны быть инструкции или данные)
            // Если это MBR, то buf[255] должен быть 0xAA55
            if (buf[255] == 0xAA55 && buf[253] == 0x3003) {
                return 0;
            }
        }
    }
    return 1;
}

uint8_t ahci_read_sector(uint32_t lba, char* buffer) {
    // Читаем сразу в тот буфер, который нам передали!
    // ВНИМАНИЕ: buffer должен быть физическим адресом (в ваших 16МБ это так)
    if (ahci_read(&__pht_hba->ports[__pht_ahci_port], lba, 0, 1, (uint16_t*)buffer)) {
        return 0; // Успех
    }
    return 1; // Ошибка
}
