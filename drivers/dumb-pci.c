#include "dumb-pci.h"

// Используем ваши внешние функции outl и inl (или реализуйте их через asm)

// Чтение 32-битного значения из конфиг. пространства PCI
uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
 
    // Формируем адрес: Bit 31 (Enable), Bits 23-16 (Bus), Bits 15-11 (Device), Bits 10-8 (Func), Bits 7-2 (Register)
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
 
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
 
    // Формируем адрес (так же, как при чтении)
    // Bit 31: Enable bit, Bits 23-16: Bus, Bits 15-11: Slot, Bits 10-8: Func, Bits 7-2: Register
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
 
    // Пишем адрес в порт управления
    outl(PCI_CONFIG_ADDRESS, address);
    
    // Пишем значение в порт данных
    outl(PCI_CONFIG_DATA, value);
}

// Поиск AHCI контроллера
uint32_t pci_scan_bus() {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            // Читаем Vendor ID и Device ID (смещение 0)
            uint32_t reg0 = pci_read_config_32(bus, dev, 0, 0);
            uint16_t vendor_id = reg0 & 0xFFFF;
            
            if (vendor_id == 0xFFFF) continue; // Устройства нет

            // Читаем Class Code, Subclass и Prog IF (смещение 0x08)
            uint32_t reg8 = pci_read_config_32(bus, dev, 0, 0x08);
            uint8_t class_code = (reg8 >> 24) & 0xFF;
            uint8_t subclass   = (reg8 >> 16) & 0xFF;
            uint8_t prog_if    = (reg8 >> 8) & 0xFF;

            // Проверяем: Class 01h (Storage), Subclass 06h (SATA), Prog IF 01h (AHCI)
            if (class_code == 0x01 && subclass == 0x06) {
                // Мы нашли SATA контроллер!
                
                // Читаем BAR5 (смещение 0x24) - это и есть адрес ABAR (AHCI Base Address)
                uint32_t abar = pci_read_config_32(bus, dev, 0, 0x24);
                
                uint32_t pci_command = pci_read_config_32(bus, dev, 0, 0x04);
                
                // 2. Включаем биты:
                // Bit 2: Bus Master (разрешает DMA)
                // Bit 1: Memory Space (разрешает MMIO / ABAR)
                pci_command |= (1 << 2) | (1 << 1);
                
                // 3. Записываем обратно
                pci_write_config_32(bus, dev, 0, 0x04, pci_command);

                // Теперь вызываем вашу функцию инициализации AHCI
                initialize_ahci_and_read(abar);
            }
        }
    }
}