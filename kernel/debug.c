#include <stdarg.h>
#include <libc/stdint>
#include <macros/macros.h>
#include <drivers/screen.h>

extern uint32_t tps;

void kprintd(const char *format, ...) {
#ifdef __PHT_DEBUG__
    va_list args;
    va_start(args, format);
    setcursorpos(0,-1);
    kprintf(0, 0xf, 2, "%f", get_ticks()/((float)(tps)));
    setcursorpos(11,-1);
    vkprintf(0, 0x7, 2, format, args);
    va_end(args);
#endif
}

void kprinte(const char *format, ...) {
#ifdef __PHT_DEBUG__
    va_list args;
    va_start(args, format);
    setcursorpos(0,-1);
    // kprintf(0, 0x40e, 2, "ERROR    ");
    for(int i=0;i<80;i++)kprintf(0, 0x40f, 2, " ");
    setcursorpos(0,-1);
    kprintf(0, 0x40f, 2, "%f", get_ticks()/((float)(tps)));
    setcursorpos(11,-1);
    vkprintf(0, 0x40f, 2, format, args);
    va_end(args);
#endif
}
void kprintw(const char *format, ...) {
#ifdef __PHT_DEBUG__
    va_list args;
    va_start(args, format);
    setcursorpos(0,-1);
    // kprintf(0, 0x600, 2, "WARNING  ");
    for(int i=0;i<80;i++)kprintf(0, 0x60e, 2, " ");
    setcursorpos(0,-1);
    kprintf(0, 0x60e, 2, "%f", get_ticks()/((float)(tps)));
    setcursorpos(11,-1);
    vkprintf(0, 0x60e, 2, format, args);
    va_end(args);
#endif
}

void kprinti(const char *format, ...) {
#ifdef __PHT_DEBUG__
    va_list args;
    va_start(args, format);
    setcursorpos(0,-1);
    // kprintf(0, 0x10f, 2, "INFO     ");
    for(int i=0;i<80;i++)kprintf(0, 0x10f, 2, " ");
    setcursorpos(0,-1);
    kprintf(0, 0x10f, 2, "%f", get_ticks()/((float)(tps)));
    setcursorpos(11,-1);
    vkprintf(0, 0x10f, 2, format, args);
    va_end(args);
#endif
}
void hexdump(char* ptr){
    for (int i = 0; i < 32; i++) {
        // 1. Выводим Hex-байты
        for (int j = 0; j < 16; j++) {
            // Изменили на %02x и добавили пробел в конце
            char c = ptr[j + i * 16]&0xff;
            if (c==0)
                kprintf(0, 0x8, 2, "%02x", c);
            else if (c==0xff)
                kprintf(0, 0xc, 2, "%02x", c);
            else if (32<c && c<128)
                kprintf(0, 0xa, 2, "%02x", c);
            else
                kprintf(0, 0xf, 2, "%02x", c);

        }

        kprintf(0, 0xf, 2, "|"); // Добавили пробел для визуального отделения

        // 2. Выводим ASCII-символы
        for (int j = 0; j < 16; j++) {
            uint8_t c = ptr[j + i * 16]; // Запоминаем байт, чтобы не дублировать индекс

            // Обычно проверяют диапазон от 32 до 126 (печатаемые ASCII)
            if (c >= 32 && c <= 126) {
                kprintf(0, 0xf, 2, "%c", c);
            } else {
                kprintf(0, 0xf, 2, "."); // Убрали лишний аргумент
            }
        }

        kprintf(0, 0x7, 2, "\n");
    }

}
