#include <cpu/ports.h>
#define outb port_byte_out
#define outw port_word_out

void set_mode_1024x768() {
    outb(0x3C4, 0x00); // Выбор регистра секвенсера
    outb(0x3C5, 0xf1); // Включение плоского режима (chain 4 off)

    outb(0x3CE, 0x06); // Выбор регистра графического контроллера
    outb(0x3CF, 0xf3); // Установка режима чтения/записи (color compare off)

    outb(0x3CE, 0x05); // Регистр 5: Color Don't Care
    outw(0x3CF, 0xf0); // Разрешить все цвета

    outb(0x3CE, 0x04); // Выбор регистра графического контроллера
    outb(0x3CF, 0xFF); // Включение всех планок (enable all planes)

    outb(0x3CE, 0x00); // Выбор регистра графического контроллера
    outb(0x3CF, 0x01); // Сброс индекса цвета

    outb(0x3D4, 0x12); // Регистр 18: Start Address High
    outw(0x3D5, 0xf0 * 2); // Верхний байт начального адреса видеопамяти

    outb(0x3D4, 0x13); // Регистр 19: Start Address Low
    outw(0x3D5, 0x0a * 4); // Нижний байт начального адреса видеопамяти

    outb(0x3D4, 0x10); // Выбор регистра CRT-контроллера
    outb(0x3D5, 0x01); // Установка черного фона
}
