#include <cpu/ports.h>     // <-- Сначала ports.h

#define COM1 0x3F8 

// Для VGA текстового режима
#define VGA_MEMORY 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

// Глобальные переменные для позиции курсора
int cursor_x = 0;
int cursor_y = 0;

uint8_t is_transmit_empty() {
    return port_byte_in(COM1 + 5) & 0x20;  // 0x3F8 + 5 = LSR register
}

int serial_received() {
    return port_byte_in(COM1 + 5) & 1;  // Bit 0 регистра LSR (Line Status Register)
}

void write_serial(char c) {
    while (!is_transmit_empty()); // Ждём освобождения буфера
    port_byte_out(COM1, c);
    // write_vga_char(c, 0x0f); // Белый текст на черном фоне
}
// Функция для очистки экрана VGA
void clear_vga_screen() {
    volatile unsigned short *vga = (volatile unsigned short*)VGA_MEMORY;
    for(int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = ' ' | (0x07 << 8);
    }
    cursor_x = 0;
    cursor_y = 0;
}

// Улучшенная функция вывода символа (дублирует в COM и VGA)
void write_char(char c) {
    // Вывод в COM-порт
    write_serial(c);
    
    // Вывод в VGA текстовый режим
    // write_vga_char(c, 0x07); // Белый текст на черном фоне
}


// Функция для вывода строки
void write_string(const char* str) {
    while(*str) {
        write_char(*str++);
    }
}

int port(){
    return COM1;
}

void init_serial() {
    port_byte_out(COM1 + 1, 0x00); // Отключаем прерывания
    port_byte_out(COM1 + 3, 0x80); // Включаем DLAB
    port_byte_out(COM1 + 0, 0x03); // Младший байт делителя
    port_byte_out(COM1 + 1, 0x00); // Старший байт делителя
    port_byte_out(COM1 + 3, 0x03); // 8N1
    port_byte_out(COM1 + 4, 0x0B); // Управление RTS/DSR
    port_byte_out(COM1 + 2, 0xC7); // Включаем FIFO
}

char read_serial() {
    while (!serial_received());  // Ждём, пока данные станут доступны
    return port_byte_in(COM1);   // Читаем из регистра данных (RBR)
}

// // Инициализация VGA
// void init_vga() {
//     clear_vga_screen();
// }
