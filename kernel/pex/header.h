#ifndef PEX_HEADER_H
#define PEX_HEADER_H

#include <stdint.h>

#define PEX_MAGIC 0x71414550 // Сигнатура "PE\x41\x71" в Little-Endian

#define PEX_FLAG_USER       0x0000 // Обычное приложение
#define PEX_FLAG_ALLOW_IO   0x0001 // Прямой доступ к портам (IOPL 3)
#define PEX_FLAG_RING0      0x0002 // Запуск в Ring 0
#define PEX_FLAG_SUID       0x0004 // Запуск от имени root (SetUID)

typedef struct __attribute__((packed)) {
    uint32_t magic;          // Сигнатура ("PE\x41\x71")
    uint32_t checksum;       // CRC32 заголовка и кода
    uint16_t header_size;    // Размер этого заголовка
    uint16_t flags;          // Флаги прав (Direct IO, Drivers и т.д.)
    
    uint32_t min_kernel_ver; // Минимальная версия ядра
    uint32_t entry_point;    // Смещение до main/start от конца заголовка
    uint32_t stack_size;     // Запрашиваемый размер стека
    uint32_t heap_size;      // Запрашиваемый размер кучи
    
    uint32_t text_size;      // Размер секции кода (.text)
    uint32_t data_size;      // Размер секции данных (.data)
    uint32_t bss_size;       // Размер секции неинициализированных данных (.bss)
} pex_header_t;

#endif