#include "ahci.h"
#include <setup.h>

// --- СТРУКТУРЫ AHCI (БЕЗ ПРОПУСКОВ) ---

extern void kprintd(const char* fmt, ...);

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
    kprintd("Error: Port stop timeout! CMD: %x\n", port->cmd);
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
    if (!port || !buf) return 0;

    port->is = (uint32_t)-1;
    volatile int slot = find_cmd_slot(port);
    if (slot == -1) return 0;

    if (!port->clb) return 0; // Защита от NULL CLB

    volatile hba_cmd_header_t *cmdheader = (hba_cmd_header_t*)(uintptr_t)port->clb;
    cmdheader += slot;

    if (!cmdheader->ctba) return 0; // <--- ЗАЩИТА: предотвращает запись в 0x0

    cmdheader->cfl = 5;
    cmdheader->w = 0;
    cmdheader->prdtl = 1;

    volatile hba_cmd_tbl_t *cmdtbl = (hba_cmd_tbl_t*)(uintptr_t)cmdheader->ctba;
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

// Основная функция записи секторов
int ahci_write(volatile hba_port_t *port, uint32_t lba_low, uint32_t lba_high, uint32_t count, const uint16_t *buf) {
    port->is = (uint32_t)-1; // Очистка статуса прерываний
    volatile int slot = find_cmd_slot(port);
    if (slot == -1) return 0;

    volatile hba_cmd_header_t *cmdheader = (hba_cmd_header_t*)(uintptr_t)port->clb;
    cmdheader += slot;
    cmdheader->cfl = 5;     // Длина FIS = 5 dwords (20 байт)
    cmdheader->w = 1;       // ВАЖНО: Запись (1)
    cmdheader->prdtl = 1;   // 1 запись в таблице PRDT

    volatile hba_cmd_tbl_t *cmdtbl = (hba_cmd_tbl_t*)(uintptr_t)cmdheader->ctba;
    // Очистка таблицы команд
    for(int i = 0; i < sizeof(hba_cmd_tbl_t); i++) ((uint8_t*)cmdtbl)[i] = 0;

    // Настройка PRDT (указываем адрес нашего буфера с данными в памяти)
    cmdtbl->prdt_entry[0].dba = (uint32_t)(uintptr_t)buf;
    cmdtbl->prdt_entry[0].dbc = (count << 9) - 1; // count * 512 байт
    cmdtbl->prdt_entry[0].i = 1; // Прерывание по завершению

    // Формируем FIS типа 0x27 (Host to Device)
    volatile uint8_t *cfis = cmdtbl->cfis;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = (1 << 7);     // Это команда
    cfis[2] = 0x35;         // ВАЖНО: WRITE DMA EXT (LBA48)

    cfis[4] = (uint8_t)lba_low;
    cfis[5] = (uint8_t)(lba_low >> 8);
    cfis[6] = (uint8_t)(lba_low >> 16);
    cfis[7] = (1 << 6);     // LBA Mode

    cfis[8] = (uint8_t)(lba_low >> 24);
    cfis[9] = (uint8_t)lba_high;
    cfis[10] = (uint8_t)(lba_high >> 8);

    cfis[12] = (uint8_t)count;
    cfis[13] = (uint8_t)(count >> 8);

    // Ждем готовности порта (BSY и DRQ должны быть сброшены)
    volatile int timeout = 0;
    while ((port->tfd & (0x80 | 0x08)) && timeout < 1000000) {
        outb(0x80, 0);
        timeout++;
    }
    if (timeout >= 1000000) return 0;

    port->ci = (1 << slot); // ЗАПУСК КОМАНДЫ!

    // Небольшая задержка
    for(int i = 0; i < 10; i++) outb(0x80, 0);

    // Ожидание завершения операции контроллером
    while (1) {
        if(!(port->ci & (1 << slot))) break;
        if (port->is & HBA_PxIS_TFES) return 0; // Ошибка транзакции (Task File Error)
    }

    return 1; // Успех
}

volatile uint32_t   __pht_ahci_port=0;
volatile hba_mem_t* __pht_hba=0;

// --- ТОЧКА ВХОДА ИЗ ВАШЕГО ЯДРА ---

uint8_t initialize_ahci_and_read(uint32_t abar) {
    if (abar == 0) return 2;   // контроллера нет (IDE-only машина, USB, CD...) — раньше тут был NULL deref
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

    if (port_idx == -1) return 2; // Диск не найден

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

    // 3. ПРОБНОЕ ЧТЕНИЕ СЕКТОРА 0 (дважды — после сброса первая команда иногда теряется)
    uint16_t *buf = (uint16_t*)__PHT_MEM_AHCI_BUFFER; // Куда читать (4MB)
    if (ahci_read(port, 0, 0, 1, buf)) {
	    kprintd("Pass 1: Read OK %x\n",port_idx);
        if(ahci_read(port, 0, 0, 1, buf)){
            kprintd("Pass 2: Read OK %x\n",port_idx);
            // Диск читается. Чей это диск (наш образ или чужой) — решает
            // drivers/blockdev.c, сравнивая сектор с MBR из bootinfo.
            return 0;
        }
    }
    return 1;
}

uint8_t ahci_read_sector(uint32_t lba, char* buffer) {
    if (!__pht_hba || !buffer) {
        kprinte("AHCI error: controller not initialized or NULL buffer!\n");
        return 1;
    }
    if (ahci_read(&__pht_hba->ports[__pht_ahci_port], lba, 0, 1, (uint16_t*)buffer)) {
        return 0;
    }
    return 1;
}

uint8_t ahci_write_sector(uint32_t lba, const char* buffer) {
    if (!__pht_hba || !buffer) {
        kprinte("AHCI error: controller not initialized or NULL buffer!\n");
        return 1;
    }
    if (ahci_write(&__pht_hba->ports[__pht_ahci_port], lba, 0, 1, (const uint16_t*)buffer)) {
        return 0;
    }
    return 1;
}