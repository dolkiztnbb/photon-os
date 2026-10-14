#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// Порты для доступа к конфигурационному пространству PCI
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// Структура для хранения данных о найденном контроллере
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t abar;      // AHCI Base Address (BAR5)
} ahci_controller_t;

// Прототипы функций
uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint32_t pci_scan_bus();
const char* pci_get_device_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

#endif