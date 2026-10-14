#include <stdint.h>
#include <cpu/ports.h> // ваши функции in/out

// Константы
#define SATA_SIG_ATA   0x00000101
#define FIS_TYPE_REG_H2D 0x27
#define HBA_PxIS_TFES   (1 << 30)

// --- СТРУКТУРЫ AHCI (БЕЗ ПРОПУСКОВ) ---

extern void kprintd(const char* fmt, ...);

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
void port_stop(volatile hba_port_t *port) ;

void port_start(volatile hba_port_t *port) ;

// Поиск свободного слота команд (обычно просто берем 0)
int find_cmd_slot(volatile hba_port_t *port) ;

// Основная функция чтения
int ahci_read(volatile hba_port_t *port, uint32_t lba_low, uint32_t lba_high, uint32_t count, uint16_t *buf) ;

// Основная функция записи секторов
int ahci_write(volatile hba_port_t *port, uint32_t lba_low, uint32_t lba_high, uint32_t count, const uint16_t *buf) ;

// --- ТОЧКА ВХОДА ИЗ ВАШЕГО ЯДРА ---

uint8_t initialize_ahci_and_read(uint32_t abar) ;

uint8_t ahci_read_sector(uint32_t lba, char* buffer) ;

// Функция-обертка для записи одного сектора (аналогично вашей ahci_read_sector)
uint8_t ahci_write_sector(uint32_t lba, const char* buffer) ;
