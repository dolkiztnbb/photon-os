#include "pci.h"
#include <cpu/ports.h>
#include <cpu/vmm.h> // Содержит vmm_identity_map_page и PAGE_SIZE

extern void kprintd(char* msg, ...);
extern char* font;

// Доступ к каталогу страниц ядра и функции маппинга
extern uint32_t page_directory[1024];
extern void vmm_identity_map_page(uint32_t* pd, uintptr_t phys_addr, uint32_t flags);

// Чтение 32-битного значения из конфиг. пространства PCI
uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
 
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
 
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
 
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

// Поиск AHCI контроллера и маппинг его памяти
uint32_t pci_scan_bus() {
    uint32_t found_abar = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t reg0 = pci_read_config_32(bus, dev, 0, 0);
            uint16_t vendor_id = reg0 & 0xFFFF;
            if (vendor_id == 0xFFFF) continue;

            uint32_t reg8 = pci_read_config_32(bus, dev, 0, 0x08);
            uint8_t class_code = (reg8 >> 24) & 0xFF;
            uint8_t subclass   = (reg8 >> 16) & 0xFF;

            // Класс 0x01 (Mass Storage), Подкласс 0x06 (SATA)
            if (class_code == 0x01 && subclass == 0x06) {
                // Читаем BAR5 (ABAR)
                found_abar = pci_read_config_32(bus, dev, 0, 0x24);
                
                // 1. ОЧИСТКА: отсекаем флаги PCI (младшие 4 бита / выравнивание по 4KB)
                found_abar &= 0xFFFFF000;

                // 2. МАППИНГ: маппим 4 страницы (16 КБ) MMIO-регистров контроллера 1:1 в память ядра
                // Флаг 0x03 = Present (0x01) | Read/Write (0x02)
                for (int i = 0; i < 4; i++) {
                    vmm_identity_map_page(page_directory, found_abar + (i * 4096), 0x03);
                }

                // 3. ВКЛЮЧАЕМ Memory Space и Bus Mastering в PCI Command Register
                uint32_t pci_command = pci_read_config_32(bus, dev, 0, 0x04);
                pci_command |= (1 << 2) | (1 << 1); // Bit 2 = Bus Master, Bit 1 = Memory Space
                pci_write_config_32(bus, dev, 0, 0x04, pci_command);

                kprintd("SATA Controller enabled with Bus Mastering. ABAR mapped: %X\n", found_abar);
                return found_abar; // Возвращаем чистый замапленный адрес
            }
        }
    }
    return 0;
}

uint32_t pci_scan() {
    kprintf(font, 0xF, 2, "Bus Dev Fun Vendor Device Class Sub  Name\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            check_pci_device(bus, dev);
        }
    }
    return 0;
}

void check_pci_device(uint8_t bus, uint8_t dev) {
    uint32_t reg0 = pci_read_config_32(bus, dev, 0, 0);
    uint16_t vendor_id = reg0 & 0xFFFF;

    if (vendor_id == 0xFFFF) return;

    check_pci_function(bus, dev, 0);

    uint32_t regC = pci_read_config_32(bus, dev, 0, 0x0C);
    uint8_t header_type = (regC >> 16) & 0xFF;

    if (header_type & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            uint32_t f_reg0 = pci_read_config_32(bus, dev, func, 0);
            if ((f_reg0 & 0xFFFF) != 0xFFFF) {
                check_pci_function(bus, dev, func);
            }
        }
    }
}

void check_pci_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t reg0 = pci_read_config_32(bus, dev, func, 0);
    uint16_t vendor_id = reg0 & 0xFFFF;
    uint16_t device_id = (reg0 >> 16) & 0xFFFF;

    uint32_t reg8 = pci_read_config_32(bus, dev, func, 0x08);
    uint8_t class_code = (reg8 >> 24) & 0xFF;
    uint8_t subclass   = (reg8 >> 16) & 0xFF;
    uint8_t prog_if    = (reg8 >> 8) & 0xFF;

    const char* device_name = pci_get_device_name(class_code, subclass, prog_if);

    kprintf(font, 0x7, 2, "%02X  %02X  %1X   %04X   %04X   %02X    %02X   %s\n", 
            bus, dev, func, vendor_id, device_id, class_code, subclass, device_name);
}

const char* pci_get_device_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    switch (class_code) {
        case 0x00:
            switch (subclass) {
                case 0x00: return "Non-VGA Unclassified Device";
                case 0x01: return "VGA-Compatible Unclassified Device";
                default:  return "Unknown Unclassified Device";
            }
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI Bus Controller";
                case 0x01: return "IDE Controller";
                case 0x02: return "Floppy Disk Controller";
                case 0x03: return "IPI Bus Controller";
                case 0x04: return "RAID Controller";
                case 0x05: return "ATA Controller (ADMA)";
                case 0x06:
                    switch (prog_if) {
                        case 0x00: return "SATA Controller (Vendor)";
                        case 0x01: return "SATA Controller (AHCI 1.0)";
                        case 0x02: return "SATA Controller (Serial Storage Bus)";
                        default:  return "SATA Controller";
                    }
                case 0x07: return "Serial Attached SCSI Controller";
                case 0x08:
                    switch (prog_if) {
                        case 0x01: return "NVMe Controller";
                        case 0x02: return "NVMe I/O Controller";
                        default:  return "Non-Volatile Memory Controller";
                    }
                default: return "Mass Storage Controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "Ethernet Controller";
                case 0x01: return "Token Ring Controller";
                case 0x02: return "FDDI Controller";
                case 0x03: return "ATM Controller";
                case 0x04: return "ISDN Controller";
                case 0x05: return "WorldFip Controller";
                case 0x06: return "PICMG Controller";
                case 0x07: return "Infiniband Controller";
                case 0x08: return "Fabric Controller";
                default:  return "Network Controller";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA Compatible Controller/PnP";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller (Non VGA)";
                default:  return "Display Controller/PnP";
            }
        case 0x04:
            switch (subclass) {
                case 0x00: return "Multimedia Video Controller";
                case 0x01: return "Multimedia Audio Controller";
                case 0x02: return "Computer Telephony Device";
                case 0x03: return "Audio Device (HDA)";
                default:  return "Multimedia Controller";
            }
        case 0x05:
            switch (subclass) {
                case 0x00: return "RAM Controller";
                case 0x01: return "Flash Controller";
                default:  return "Memory Controller";
            }
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x02: return "EISA Bridge";
                case 0x03: return "MCA Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                case 0x05: return "PCMCIA Bridge";
                case 0x06: return "NuBus Bridge";
                case 0x07: return "CardBus Bridge";
                case 0x08: return "RACEway Bridge";
                case 0x09: return "Semi-Transparent PCI-to-PCI Bridge";
                case 0x0A: return "InfiniBand-to-PCI Host Bridge";
                default:  return "Bridge Device";
            }
        case 0x07:
            switch (subclass) {
                case 0x00: return "Serial Controller";
                case 0x01: return "Parallel Controller";
                case 0x02: return "Multiport Serial Controller";
                case 0x03: return "Modem";
                case 0x04: return "GPIB Controller";
                case 0x05: return "Smart Card Controller";
                default:  return "Communication Controller";
            }
        case 0x08:
            switch (subclass) {
                case 0x00: return "PIC";
                case 0x01: return "DMA Controller";
                case 0x02: return "Timer";
                case 0x03: return "RTC Controller";
                case 0x04: return "PCI Hot-Plug Controller";
                case 0x05: return "SD Host Controller";
                case 0x06: return "IOMMU";
                default:  return "System Peripheral";
            }
        case 0x09:
            switch (subclass) {
                case 0x00: return "Keyboard Controller";
                case 0x01: return "Digitizer Pen";
                case 0x02: return "Mouse Controller";
                case 0x03: return "Scanner Controller";
                case 0x04: return "Gameport Controller";
                default:  return "Input Device Controller";
            }
        case 0x0A: return "Docking Station";
        case 0x0B:
            switch (subclass) {
                case 0x00: return "386 Processor";
                case 0x01: return "486 Processor";
                case 0x02: return "Pentium Processor";
                case 0x10: return "Alpha Processor";
                case 0x20: return "PowerPC Processor";
                case 0x30: return "MIPS Processor";
                case 0x40: return "Co-Processor";
                default:  return "Processor";
            }
        case 0x0C:
            switch (subclass) {
                case 0x00: return "FireWire (IEEE 1394) Controller";
                case 0x01: return "ACCESS Bus Controller";
                case 0x02: return "SSA Controller";
                case 0x03:
                    switch (prog_if) {
                        case 0x00: return "USB UHCI Controller";
                        case 0x10: return "USB OHCI Controller";
                        case 0x20: return "USB EHCI Controller (USB2)";
                        case 0x30: return "USB XHCI Controller (USB3)";
                        case 0x80: return "USB (No specific IF)";
                        case 0xFE: return "USB Device (not host)";
                        default:  return "USB Controller";
                    }
                case 0x04: return "Fibre Channel Controller";
                case 0x05: return "SMBus Controller";
                case 0x06: return "InfiniBand Controller";
                case 0x07: return "IPMI Interface";
                case 0x08: return "SERCOS Interface";
                case 0x09: return "CAN Bus Controller";
                default:  return "Serial Bus Controller";
            }
        case 0x0D:
            switch (subclass) {
                case 0x00: return "iRDA Controller";
                case 0x01: return "Consumer IR Controller";
                case 0x10: return "RF Controller";
                case 0x11: return "Bluetooth Controller";
                case 0x12: return "Broadband Controller";
                case 0x20: return "Ethernet Controller (802.1a)";
                case 0x21: return "Ethernet Controller (802.1b)";
                default:  return "Wireless Controller";
            }
        case 0x0E: return "Intelligent Controller (I2O)";
        case 0x0F: return "Satellite Communication Controller";
        case 0x10: return "Encryption Controller";
        case 0x11: return "Signal Processing Controller";
        case 0x12: return "Processing Accelerator";
        case 0x13: return "Non-Essential Instrumentation";
        case 0xFF: return "Unassigned / Vendor-Specific";
        default:  return "Unknown Device";
    }
}