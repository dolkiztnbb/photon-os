#include "keyboard.h"
#include <cpu/ports.h>
#include <cpu/isr.h>
#include <libc/string.h>
#include <libc/function.h>
#include <kernel/kernel.h>
#include <libc/stdint>
#include <libc/mem.h>
#include <macros/macros.h>

char kbutton[256];
char isCaps = 0;
char shift = 0;
char ctrl = 0;

#define KBD_BUF_SIZE 256
static char kbd_buf[KBD_BUF_SIZE];
static int kbd_head = 0;
static int kbd_tail = 0;

const char scancodeToKey[128] = {
    [0x00] = '\0', [0x01] = '\0', [0x02] = '!',  [0x03] = '@',  [0x04] = '#',  [0x05] = '$',
    [0x06] = '%',  [0x07] = '^',  [0x08] = '&',  [0x09] = '*',  [0x0A] = '(',  [0x0B] = ')',
    [0x0C] = '_',  [0x0D] = '+',  [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'Q',  [0x11] = 'W',
    [0x12] = 'E',  [0x13] = 'R',  [0x14] = 'T',  [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I',
    [0x18] = 'O',  [0x19] = 'P',  [0x1A] = '{',  [0x1B] = '}',  [0x1C] = '\n', [0x1E] = 'A',
    [0x1F] = 'S',  [0x20] = 'D',  [0x21] = 'F',  [0x22] = 'G',  [0x23] = 'H',  [0x24] = 'J',
    [0x25] = 'K',  [0x26] = 'L',  [0x27] = ':',  [0x28] = '\"', [0x29] = '~',  [0x2A] = '\x98',
    [0x2B] = '|',  [0x2C] = 'Z',  [0x2D] = 'X',  [0x2E] = 'C',  [0x2F] = 'V',  [0x30] = 'B',
    [0x31] = 'N',  [0x32] = 'M',  [0x33] = '<',  [0x34] = '>',  [0x35] = '?',  [0x37] = '*',
    [0x39] = ' ',  [0x4A] = '-',  [0x4E] = '+',  [0x53] = '.'
};

static void kbd_push(char c) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

static void keyboard_callback(const registers_t *regs) {
    asm volatile("cli");

    uint8_t status = inb(0x64);
    
    // Бит 0 = 0 (нет данных) ИЛИ Бит 5 = 1 (данные от МЫШИ) -> выходим!
    if (!(status & 0x01) || (status & 0x20)) {
        return; 
    }

    uint8_t scancode = inb(0x60);
    if (scancode == 0) return;

    // Префикс 0xE0 просто пропускаем, чтобы не сбивать состояние
    if (scancode == 0xE0) return;

    // Модификаторы Shift
    if (scancode == 0x2A || scancode == 0x36) { shift = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift = 0; return; }

    // Модификаторы Control
    if (scancode == 0x1D) { ctrl = 1; return; }
    if (scancode == 0x9D) { ctrl = 0; return; }

    // CapsLock
    if (scancode == 0x3A) { isCaps = !isCaps; return; }

    // Отпускание клавиши (Break code)
    if (scancode & 0x80) {
        uint8_t make = scancode & 0x7F;
        kbutton[make] = 0;
        return;
    }

    // Нажатие клавиши (Make code)
    kbutton[scancode] = 1;

    char key = 0;

    // Стрелки и клавиша Windows (Left Win = 0x5B, Right Win = 0x5C, Menu = 0x5D)
    if (scancode == 0x01)      key = 27; // ESC
    else if (scancode == 0x48) key = KEY_UP;
    else if (scancode == 0x50) key = KEY_DOWN;
    else if (scancode == 0x4B) key = KEY_LEFT;
    else if (scancode == 0x4D) key = KEY_RIGHT;
    else if (scancode == 0x5B || scancode == 0x5C || scancode == 0x5D) key = KEY_WIN;
    else if (scancode < 128) {
        key = scancodeToKey[scancode];
        if (key) {
            if (ctrl) {
                if (key >= 'A' && key <= 'Z') key = key - 'A' + 1;
            } else {
                if (key >= 'A' && key <= 'Z') {
                    if (!(shift ^ isCaps)) key = toSmall(key);
                } else {
                    if (!shift) key = toSmall(key);
                }
            }
        }
    }

    if (key != 0) {
        kbd_push(key);
    }

    UNUSED(regs);
}

void init_keyboard(void) {
    memset(kbutton, 0, sizeof(kbutton));
    kbd_head = 0;
    kbd_tail = 0;
    register_interrupt_handler(IRQ1, keyboard_callback); 
}

char poll_key(void) {
    if (kbd_head != kbd_tail) {
        char c = kbd_buf[kbd_tail];
        kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
        return c;
    }
    return 0;
}

char wtc(void) {
    while (1) {
        char k = poll_key();
        if (k != 0) return k;
        asm volatile("hlt");
    }
}

char getch(void) {
    char c;
    do {
        c = wtc();
    } while (c == '\r' || c == '\0');
    return c;
}
