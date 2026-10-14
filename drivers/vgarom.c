#include <stdint.h>
#include <stddef.h>
#include <drivers/pci.h>
#include <drivers/vgarom.h>

/**
 * Функция поиска VGA контроллера на шине PCI
 */
int find_vga_pci(uint8_t *bus, uint8_t *dev, uint8_t *func) {
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t reg0 = pci_read_config_32(b, d, f, 0);
                if (reg0 == 0xFFFFFFFF) continue;

                uint32_t class_reg = pci_read_config_32(b, d, f, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;

                // Class 03h = Display Controller, Subclass 00h = VGA Compatible
                if (class_code == 0x03 && subclass == 0x00) {
                    *bus = (uint8_t)b;
                    *dev = d;
                    *func = f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/**
 * Поиск указателя на шрифт в ROM.
 * В классическом BIOS шрифт 8x16 часто можно найти по сигнатуре
 * или через таблицу векторов параметров видео.
 */
vgafont detect_vga_font() {
    vgafont font;
    font.char_height = 0;
    font.char_width = 8; // Ширина VGA символа всегда 8 бит (1 байт на строку)
    font.vga_rom_ptr = NULL;

    /* 
     * Метод 1: Поиск в VGA BIOS (0xC0000 - 0xC7FFF).
     * Мы ищем известную последовательность байт для символа 'A' (ASCII 65).
     * Для шрифта 8x16 символ 'A' обычно выглядит так:
     * 00 00 18 3C 66 66 7E 66 66 66 66 00 00 00 00 00 (примерные значения)
     * Однако более надежный способ — обратиться к фиксированному адресу в Legacy BIOS
     * или сканировать область.
     */

    // Стандартный адрес шрифта 8x8 в BIOS: 0xFFA6E
    // Но для VGA 8x16 мы попробуем найти его в области 0xC0000
    uint8_t* rom_search_start = (uint8_t*)0xC0000;
    uint8_t* rom_search_end   = (uint8_t*)0xC8000;

    // Ищем паттерн символа 'A' (ASCII 65) шрифта 8x12
    // Это упрощенный поиск. В реальности BIOS хранит указатели в таблице.
    const uint8_t pattern_A_8x16[] = {0x00, 0x00, 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x00};

    for (uint8_t* ptr = rom_search_start; ptr < (rom_search_end - 1024); ptr++) {
        // Проверяем, совпадает ли кусок памяти с паттерном буквы 'A' на позиции 65
        // (65 * 16 байт = 1040 байт от начала таблицы)
        int match = 1;
        for (int i = 0; i < 16; i++) {
            if (ptr[1040 + i] != pattern_A_8x16[i]) {
                match = 0;
                break;
            }
        }

        if (match) {
            font.char_height = 16;
            font.vga_rom_ptr = (uint8_t*)ptr;
            return font;
        }
    }

    // Метод 2: Если не нашли, возвращаем стандартный адрес 8x8 из System BIOS (если доступен)
    // В очень старых PC это 0xFFA6E
    font.char_height = 8;
    font.vga_rom_ptr = (uint8_t*)0xFFA6E;

    return font;
}


void read_vga_font(uint32_t *buffer) {
    // Используем uint32_t* для буфера, так как в ассемблере копия идет через movsd (двойными словами)
    uint32_t *dest = buffer;
    
    // Указатель на VGA память (в 32-битном защищенном режиме DOS/ностальгических ОС)
    const uint32_t *src = (const uint32_t *)0xA0000;

    // --- Сброс режима четный/нечетный (Graphics Controller) ---
    // mov dx, 03ceh; mov ax, 5; out dx, ax
    outw(0x03CE, 0x0005);

    // --- Отображение VGA памяти на 0A0000h ---
    // mov ax, 0406h; out dx, ax
    outw(0x03CE, 0x0406);

    // --- Выбор плоскости 2 (Sequencer) ---
    // mov dx, 03c4h; mov ax, 0402h; out dx, ax
    outw(0x03C4, 0x0402);

    // --- Сброс режима четный/нечетный (Sequencer) ---
    // mov ax, 0604h; out dx, ax
    outw(0x03C4, 0x0604);

    // --- Копирование карты символов ---
    // В ассемблере: цикл на 256 итераций, внутри 4 раза movsd (16 байт) и add esi, 16.
    // Каждый символ в памяти VGA занимает 32 байта, но полезных из них только первые 16.
    for (int i = 0; i < 256; i++) {
        // movsd * 4 (копируем 4 двойных слова = 16 байт)
        *dest++ = *src++;
        *dest++ = *src++;
        *dest++ = *src++;
        *dest++ = *src++;

        // add esi, 16 (пропускаем оставшиеся 16 байт «пустоты» до следующего знака)
        // Так как src — это указатель на uint32_t (4 байта), сдвиг на 4 элемента равен 16 байтам.
        src += 4;
    }

    // --- Восстановление стандартного состояния VGA ---
    // mov dx, 03c4h (он уже остался в DX после предыдущих операций)
    // mov ax, 0302h; out dx, ax
    outw(0x03C4, 0x0302);
    
    // mov ax, 0204h; out dx, ax
    outw(0x03C4, 0x0204);

    // mov dx, 03ceh
    // mov ax, 1005h; out dx, ax
    outw(0x03CE, 0x1005);
    
    // mov ax, 0E06h; out dx, ax
    outw(0x03CE, 0x0E06);
}
static inline uint32_t read_bda_pointer(uint32_t addr) {
    uint16_t* ptr = (uint16_t*)addr;
    uint16_t offset = ptr[0];
    uint16_t segment = ptr[1];
    return (uint32_t)((segment << 4) + offset);
}

static inline void io_wait() {
    outb(0x80, 0); // Порт 0x80 используется для POST-кодов, запись туда дает задержку ~1мкс
}

vgafont get_vga_font_spec() {
    vgafont font;
    font.char_width = 8;

    // --- СПОСОБ 1: Чтение из BDA (Самый безопасный) ---
    // Адрес 0x485 содержит высоту символа (bytes per character)
    uint8_t *bda_char_height = (uint8_t*)0x485;
    
    // Если мы в реальном режиме или имеем доступ к первым 1МБ
    if (*bda_char_height > 0 && *bda_char_height < 33) {
        font.char_height = *bda_char_height;
    } else {
        // --- СПОСОБ 2: Чтение через порты с проверкой базового адреса ---
        
        // Читаем Miscellaneous Output Register (0x3CC)
        // Bit 0: 0 = Mono (0x3B4), 1 = Color (0x3D4)
        uint16_t base_port = (inb(0x3CC) & 0x01) ? 0x3D4 : 0x3B4;

        outb(base_port, 0x09);
        io_wait();
        font.char_height = (inb(base_port + 1) & 0x1F) + 1;
    }

    // --- ПОЛУЧЕНИЕ УКАЗАТЕЛЯ НА ШРИФТ ---
    // В BDA по адресу 0x4A8 лежит указатель на Video Parameter Table.
    // Но проще взять стандартный вектор прерывания видео-функций (INT 43h)
    // Он находится по физическому адресу 0x0000:0x010C
    uint16_t *int43_ptr = (uint16_t*)0x10C;
    uint32_t offset = int43_ptr[0];
    uint32_t segment = int43_ptr[1];
    
    font.vga_rom_ptr = (uint8_t*)((segment << 4) + offset);

    // Если указатель пустой (бывает в некоторых эмуляторах), 
    // используем классический адрес VGA ROM
    if ((uint32_t)font.vga_rom_ptr == 0) {
        font.vga_rom_ptr = (uint8_t*)0xFFA6E; // BIOS 8x8 font
    }

    return font;
}

#define VGA_SEQ_INDEX 0x3C4
#define VGA_SEQ_DATA  0x3C5
#define VGA_GC_INDEX  0x3CE
#define VGA_GC_DATA   0x3CF
/**
 * Перевод предоставленного ASM кода в C
 * Эта функция загружает "мусор" (тестовую последовательность) в память шрифта
 */
void vga_load_test_font(uint8_t *source_font, uint8_t height) {
    // 1. Подготовка контроллера (как в вашем ASM коде)
    outw(0x3C4, 0x0402); // Enable Plane 2
    outw(0x3C4, 0x0003); // Reset Font Select
    outw(0x3C4, 0x0604); // Memory Mode: O/E off, Ext on

    outw(0x3CE, 0x0005); // Graphics Mode: Write Mode 0
    outw(0x3CE, 0x0C06); // Misc: Map to 0xB8000

    // 2. Копирование данных из вашего адреса
    volatile uint8_t *vram = (volatile uint8_t *)0xB8000;

    for (int char_code = 0; char_code < 256; char_code++) {
        for (int row = 0; row < height; row++) {
            // VRAM шаг всегда 32 байта на символ
            // Источник читается последовательно: char_code * height
            vram[char_code * 32 + row] = source_font[char_code * height + row];
        }
    }

    // 3. Восстановление текстового режима
    outw(0x3C4, 0x0302); // Enable Plane 0 & 1
    outw(0x3C4, 0x0304); // Restore Odd/Even
    outw(0x3CE, 0x1005); // Restore Odd/Even
    outw(0x3CE, 0x0E06); // Restore 0xB8000 Text Map
}
/**
 * Функция для восстановления текстового режима после загрузки шрифта
 */
void vga_restore_text_mode() {
    // Восстанавливаем Sequencer
    outw(VGA_SEQ_INDEX, 0x0302); // Enable Plane 0 & 1
    outw(VGA_SEQ_INDEX, 0x0304); // Enable Odd/Even, Ext Memory
    
    // Восстанавливаем Graphics Controller
    outw(VGA_GC_INDEX, 0x1005); // Enable Odd/Even
    outw(VGA_GC_INDEX, 0x0E06); // Map to 0xB8000, Text Mode
}
