#ifndef TIMER_H
#define TIMER_H

#include <libc/stdint>

void init_timer(const uint32_t freq);

void delay(const uint32_t ms);

#endif
