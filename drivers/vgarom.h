#ifndef __VGAROM_H__
#define __VGAROM_H__

#include <stdint.h>
#include <stddef.h>
#include "pci.h"
#include <cpu/ports.h>

#define outw port_word_out

typedef struct {
    uint8_t char_height;
    uint8_t char_width;
    uint8_t* vga_rom_ptr; // Указатель на начало таблицы символов
} vgafont;
int find_vga_pci(uint8_t *bus, uint8_t *dev, uint8_t *func);
vgafont detect_vga_font();
static inline uint32_t read_bda_pointer(uint32_t addr);
vgafont get_vga_font_spec();
void vga_load_test_font(uint8_t *source_font, uint8_t height);
void vga_restore_text_mode();

#endif