#include "timer.h"
#include "isr.h"
#include "ports.h"
#include <libc/function.h>
#include <libc/string.h>
#include <drivers/screen.h>
#include <cpu/mt.h>


uint32_t tick = 0;

extern uint32_t total_tasks;

static void timer_callback(const registers_t *regs) {
    tick++;
    mt_schedule();
}
uint32_t get_ticks() {
    return tick;
}

void init_timer(const uint32_t freq) {
    /* Install the function we just wrote */
    register_interrupt_handler(IRQ0, timer_callback);
    /* Get the PIT value: hardware clock at 1193180 Hz */
    uint32_t divisor = 1193180 / freq;
    uint8_t low  = (uint8_t)(divisor & 0xFF);
    uint8_t high = (uint8_t)( (divisor >> 8) & 0xFF);
    /* Send the command */
    port_byte_out(0x43, 0x36); /* Command port */
    port_byte_out(0x40, low);
    port_byte_out(0x40, high);
    tick = 0;
}

void delay(uint32_t ms) {
    // ��������� 50 �� - ��� 20 �� �� ���, ��������� ���������� �����
    uint32_t ticks_needed = ms / 20;

    uint32_t start_tick = tick;

    while ((tick - start_tick) < ticks_needed) {}
}