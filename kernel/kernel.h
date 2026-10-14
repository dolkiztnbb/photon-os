#ifndef KERNEL_H
#define KERNEL_H
#include <libc/stdint>
void int80h(registers_t);
uint32_t int80(uint32_t,uint32_t,uint32_t,uint32_t);
#endif
