#include <libc/stdint>
#include <cpu/ports.h>
#include <cpu/isr.h>
#include <cpu/timer.h>

volatile int32_t mouse_delta_x = 0;
volatile int32_t mouse_delta_y = 0;
volatile uint8_t mouse_buttons = 0;

static void mouse_wait_input(void) {
    uint32_t timeout = 100000;
    while ((inb(0x64) & 2) && --timeout);
}

static void mouse_wait_output(void) {
    uint32_t timeout = 100000;
    while (!(inb(0x64) & 1) && --timeout);
}

static void mouse_write(uint8_t val) {
    mouse_wait_input();
    outb(0x64, 0xD4);
    mouse_wait_input();
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait_output();
    return inb(0x60);
}

static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[3];

static void mouse_callback(const registers_t *regs) {
    uint32_t eflags;
    asm volatile("pushfl; popl %0; cli" : "=r"(eflags));

    uint8_t status = inb(0x64);
    // Бит 0 = есть данные, Бит 5 (0x20) = данные именно от мыши (AUX)
    while ((status & 0x01) && (status & 0x20)) {
        uint8_t data = inb(0x60);

        if (mouse_cycle == 0) {
            if (data & 0x08) {
                mouse_byte[0] = data;
                mouse_cycle = 1;
            }
        } else if (mouse_cycle == 1) {
            mouse_byte[1] = data;
            mouse_cycle = 2;
        } else if (mouse_cycle == 2) {
            mouse_byte[2] = data;
            mouse_cycle = 0;

            if (!(mouse_byte[0] & 0xC0)) {
                int32_t dx = (int32_t)mouse_byte[1] - ((mouse_byte[0] & 0x10) ? 256 : 0);
                int32_t dy = (int32_t)mouse_byte[2] - ((mouse_byte[0] & 0x20) ? 256 : 0);

                mouse_delta_x += dx;
                mouse_delta_y += dy;
                mouse_buttons = mouse_byte[0] & 0x07;
            }
        }

        status = inb(0x64);
    }

    if (eflags & 0x200) asm volatile("sti");
}

void init_mouse(void) {
    asm volatile("cli");

    // 1. Вычищаем мусор из буфера 0x60
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    // 2. Включаем вспомогательный порт мыши
    mouse_wait_input();
    outb(0x64, 0xA8);

    // 3. Читаем Command Byte
    mouse_wait_input();
    outb(0x64, 0x20);
    uint8_t status = mouse_read();

    // 4. Включаем прерывание IRQ12 (бит 1) и разрешаем тактирование (сброс бита 5)
    status |= 0x02;
    status &= ~(1 << 5);
    mouse_wait_input();
    outb(0x64, 0x60);
    mouse_wait_input();
    outb(0x60, status);

    // 5. Установка стандартных настроек мыши
    mouse_write(0xF6);
    mouse_read(); // ACK 0xFA

    // 6. Включаем передачу пакетов
    mouse_write(0xF4);
    mouse_read(); // ACK 0xFA

    // 7. Снова вычищаем всё лишнее
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    // 8. Сбрасываем переменные
    mouse_delta_x = 0;
    mouse_delta_y = 0;
    mouse_buttons = 0;

    register_interrupt_handler(IRQ12, mouse_callback);

    asm volatile("sti");
}