#ifndef PORTS_H
#define PORTS_H

#include <libc/stdint> // Проверьте, что расширение .h на месте

unsigned char port_byte_in (const uint16_t port);
void port_byte_out (const uint16_t port, const uint8_t data);
unsigned short port_word_in (const uint16_t port);
void port_word_out (const uint16_t port, const uint16_t data);

// Тело функций ДОЛЖНО быть в заголовке
void outl(uint16_t port, uint32_t val);
uint32_t inl(uint16_t port);

#define inb port_byte_in
#define outb port_word_out

#endif