#include "screen.h"
#include "serial.h"
#include <libc/mem.h>
#include <libc/stdint>
#include <libc/string.h>
#include "videomodes.h"
#include <cpu/ports.h>
#include <macros/macros.h>
#include <stdarg.h>

int offsetx = 0, offsety = 0;
int SCREEN_SCROLL = 1;
char silent = 0;
char higher_video_mode;
char overflowignore = 0;

#define VGA_VA 0xb8000
volatile uint8_t *VGA = (uint8_t*)VGA_VA;

char* font = (char*)0;

char size_x = 1, size_y = 1;
int spacingx = MAXSYMX, spacingy = MAXSYMY;

// Читаем глобальные цвета STDOUT, чтобы использовать их как базу
extern uint8_t stdout_fg;
extern uint8_t stdout_bg;

static unsigned char current_color = 0x07;
static unsigned char last_com_color = 0xFF;

// Функция для ручного переключения цвета терминала (вызывается из ksetiocolor)
void sync_screen_color(uint8_t bg, uint8_t fg) {
    current_color = (bg << 4) | (fg & 0x0F);
}

void write_string_com(const char *s) {
    // while (*s) {
    //     write_char(*s++);
    // }
}

void update_com_color(unsigned char vga_color) {
    const char* ansi_text_colors[] = {"30", "34", "32", "36", "31", "35", "33", "37"};
    const char* ansi_text_bright[] = {"90", "94", "92", "96", "91", "95", "93", "97"};
    
    unsigned char text = vga_color & 0x0F;
    unsigned char bg   = (vga_color & 0xF0) >> 4;

    write_string_com("\x1b[0m");

    write_string_com("\x1b[");
    if (text & 8) {
        write_string_com(ansi_text_bright[text & 7]);
    } else {
        write_string_com(ansi_text_colors[text]);
    }
    write_string_com("m");

    if ((bg & 7) != 0) {
        write_string_com("\x1b[");
        const char* ansi_bg_colors[] = {"40", "44", "42", "46", "41", "45", "43", "47"};
        write_string_com(ansi_bg_colors[bg & 7]);
        write_string_com("m");
    }
}

typedef enum { 
    STATE_NORMAL, 
    STATE_ESC, 
    STATE_BRACKET 
} ansi_state_t;

static ansi_state_t ansi_state = STATE_NORMAL;
static int ansi_param = 0;

// ==============================================================
// УПРАВЛЕНИЕ АППАРАТНЫМ КУРСОРСОРМ
// ==============================================================

void setcursorpos(const int x, const int y) {
    if (y != -1) offsety = y;
    if (x != -1) offsetx = x;

    if (offsetx >= 80) offsetx = 79;
    if (offsety > 24) offsety = 24;
    if (offsetx < 0) offsetx = 0;
    if (offsety < 0) offsety = 0;

    uint16_t pos = offsetx + offsety * 80;
    port_byte_out(0x3D4, 0x0F);
    port_byte_out(0x3D5, (uint8_t)(pos & 0xFF));
    port_byte_out(0x3D4, 0x0E);
    port_byte_out(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void scroll_screen(void) {
    for (int i = 0; i < 24 * 80 * 2; i++) {
        VGA[i] = VGA[i + 80 * 2];
    }
    for (int i = 24 * 80 * 2; i < 25 * 80 * 2; i += 2) {
        VGA[i] = ' '; 
        VGA[i + 1] = current_color;
    }
}

// ==============================================================

void kputc_attr(char ch, const char attr) {
    if (attr & 0x08) return;

    if (ansi_state == STATE_ESC) {
        if (ch == '[') { 
            ansi_state = STATE_BRACKET; 
            ansi_param = 0; 
        } else { 
            ansi_state = STATE_NORMAL; 
        }
        return;
    }

    if (ansi_state == STATE_BRACKET) {
        if (ch >= '0' && ch <= '9') {

            ansi_param = ansi_param * 10 + (ch - '0');
        } else if (ch == ';') {
            if (ansi_param == 0) {
                // Сброс ANSI (\033[0m) теперь возвращает цвет STDOUT
                current_color = (stdout_bg << 4) | (stdout_fg & 0x0F);
            } else if (ansi_param >= 30 && ansi_param <= 37) {
                const unsigned char ansi_to_vga[] = {0, 4, 2, 6, 1, 5, 3, 7};
                current_color = (current_color & 0xF0) | ansi_to_vga[ansi_param - 30];
            } else if (ansi_param >= 90 && ansi_param <= 97) {
                const unsigned char ansi_to_vga_bright[] = {8, 12, 10, 14, 9, 13, 11, 15};
                current_color = (current_color & 0xF0) | ansi_to_vga_bright[ansi_param - 90];
            } else if (ansi_param >= 40 && ansi_param <= 47) {
                const unsigned char ansi_to_vga[] = {0, 4, 2, 6, 1, 5, 3, 7};
                current_color = (current_color & 0x0F) | (ansi_to_vga[ansi_param - 40] << 4);
            }
            ansi_param = 0; 
        } else if (ch == 'm') {
            if (ansi_param == 0) {
                // Сброс ANSI (\033[0m) теперь возвращает цвет STDOUT
                current_color = (stdout_bg << 4) | (stdout_fg & 0x0F);
            } else if (ansi_param >= 30 && ansi_param <= 37) {
                const unsigned char ansi_to_vga[] = {0, 4, 2, 6, 1, 5, 3, 7};
                current_color = (current_color & 0xF0) | ansi_to_vga[ansi_param - 30];
            } else if (ansi_param >= 90 && ansi_param <= 97) {
                const unsigned char ansi_to_vga_bright[] = {8, 12, 10, 14, 9, 13, 11, 15};
                current_color = (current_color & 0xF0) | ansi_to_vga_bright[ansi_param - 90];
            } else if (ansi_param >= 40 && ansi_param <= 47) {
                const unsigned char ansi_to_vga[] = {0, 4, 2, 6, 1, 5, 3, 7};
                current_color = (current_color & 0x0F) | (ansi_to_vga[ansi_param - 40] << 4);
            }

            ansi_state = STATE_NORMAL;
        } else {
            ansi_state = STATE_NORMAL;
        }
        return;
    }

    if (ch == '\x1b') {
        ansi_state = STATE_ESC;
        return;
    }

    if (ch == '\n' || ch == '\r') {
        if (ch == '\n') {
            offsetx = 0;
            offsety++;
            if (offsety > 24) {
                scroll_screen();
                offsety = 24;
            }
        } else {
            offsetx = 0; 
        }
        
        // if (!(attr & 0x04)) write_char(ch);
        setcursorpos(offsetx, offsety); 
        return;
    }

    if (ch == '\b') {
        if (offsetx > 0) {
            offsetx--;
        } else if (offsety > 0) {
            offsetx = 79;
            offsety--;
        }
        // if (!(attr & 0x04)) write_char('\b');
        setcursorpos(offsetx, offsety); 
        return;
    }

    if (offsetx >= 80) { 
        offsetx = 0; 
        offsety++; 
        if (offsety > 24) {
            scroll_screen();
            offsety = 24;
        }
    }

    int vga_idx = offsetx * 2 + offsety * 160;
    VGA[vga_idx] = ch;
    VGA[vga_idx + 1] = current_color;

    if (current_color != last_com_color) {
        if (!(attr & 0x04)) update_com_color(current_color);
        last_com_color = current_color;
    }

    offsetx++;

    // Проверка переноса и прокрутки сразу после инкремента
    if (offsetx >= 80) { 
        offsetx = 0; 
        offsety++; 
        if (offsety > 24) {
            scroll_screen();
            offsety = 24;
        }
    }

    setcursorpos(offsetx, offsety);
}

void kputc(char ch) {
    kputc_attr(ch, 0);
}

void kprint_at(const unsigned char font[], const char *str, const int x, const int y, const unsigned short color, const char attr) {
    if(attr & 0x08) return;
    if (x != -1) offsetx = x;
    if (y != -1) offsety = y;

    // ВАЖНО: Сохраняем текущий цвет, чтобы kprintf не "пачкал" глобальное состояние (эхо STDIN и т.д.)
    unsigned char saved_color = current_color;

#ifdef __PHT_COLOR__
    current_color = (color & 0x0f) | ((color & 0x0f00) >> 4);
#endif

    int i = 0;
    while (str[i] != '\0') {
        kputc_attr(str[i], attr);
        i++;
    }

    // Возвращаем глобальный цвет на место
    current_color = saved_color;
}

void kprint(const unsigned char font[], const char *str, const unsigned short color, const char attr) {
    kprint_at(font, str, offsetx, offsety, color, attr);
}

int vsnkprintf(char *buffer, int n, const char *format, va_list args) {
    if (!buffer || n <= 0) return 0;

    buffer[0] = '\0';
    char temp[80];
    const char *ptr = format;
    int current_len = 0;

    while (*ptr && current_len < n - 1) {
        if (*ptr != '%') {
            append(buffer, *ptr);
            current_len++;
            ptr++;
        } else {
            ptr++; // Пропускаем '%'

            int width = 0;
            while (*ptr >= '0' && *ptr <= '9') {
                width = width * 10 + (*ptr - '0');
                ptr++;
            }

            switch (*ptr) {
                case 'd': {
                    int d = va_arg(args, int);
                    base_to_string(d, 10, 0, width, temp);
                    strncat(buffer, temp);
                    break;
                }
                case 'x': {
                    unsigned int x = va_arg(args, unsigned int);
                    base_to_string(x, 16, 0, width, temp);
                    strncat(buffer, temp);
                    break;
                }
                case 'o': {
                    unsigned int x = va_arg(args, unsigned int);
                    base_to_string(x, 8, 0, width, temp);
                    strncat(buffer, temp);
                    break;
                }
                case 'X': {
                    unsigned int x = va_arg(args, unsigned int);
                    base_to_string(x, 16, 1, width, temp);
                    strncat(buffer, temp);
                    break;
                }
                case 'f': {
                    double f = va_arg(args, double);
                    clear(temp, 80);
                    float_to_ascii(f, 6, temp);
                    strncat(buffer, temp);
                    break;
                }
                case 'b': {
                    int b = va_arg(args, int);
                    base_to_string(b, 2, 0, width ? width : 32, temp);
                    strncat(buffer, temp);
                    break;
                }
                case 's': {
                    char *s = va_arg(args, char*);
                    if (!s) s = "(null)";
                    int len = strlen(s);
                    
                    for (int j = 0; j < len; j++) append(buffer, s[j]);
                    if (len < width) {
                        for (int j = 0; j < (width - len); j++) append(buffer, ' ');
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    append(buffer, c);
                    break;
                }
                case '%': {
                    append(buffer, '%');
                    break;
                }
                default:
                    append(buffer, *ptr);
                    break;
            }
            ptr++;
            current_len = strlen(buffer); 
        }
    }
    return current_len;
}

void comprintf(const char *format, ...){
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnkprintf(buffer, 256, format, args);
    va_end(args);
}

void snkprintf(char *buffer, const char *format, ...){
    va_list args;
    va_start(args, format);
    vsnkprintf(buffer, 256, format, args);
    va_end(args);
}

void vkprintf(const unsigned char font[], const unsigned short color, const char attr, const char *format, va_list args) {
    if (silent) return;
    char buffer[512] = {0};
    vsnkprintf(buffer, 512, format, args);
    kprint(font, buffer, color, attr);
}

void kprintf(const unsigned char font[], const unsigned short color, const char attr, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vkprintf(font, color, attr, format, args);
    va_end(args);
}

void setspacing(const int sx, const int sy) {
    // legacy API placeholder
}

void getcursorpos(int* x, int* y) {
    if (x) *x = offsetx;
    if (y) *y = offsety;
}

void cls(const short col) {
    for(int i = 0; i < 25 * 80 * 2; i += 2){
        VGA[i] = ' '; 
        VGA[i+1] = col;
    }
    setcursorpos(0, 0); 
}

void bit9(int en) {
    uint8_t misc_reg;
    uint8_t seq_reg;
    
    asm volatile ("inb %1, %b0" : "=a" (misc_reg) : "Nd" (0x3CC));
    port_byte_out(0x3C4, 0x01);
    asm volatile ("inb %1, %b0" : "=a" (seq_reg) : "Nd" (0x3C5));

    if (en) {
        misc_reg &= ~0x0C; 
        seq_reg &= ~0x01;
    } else {
        misc_reg |= 0x04;  
        seq_reg |= 0x01;
    }

    port_byte_out(0x3C2, misc_reg);
    port_byte_out(0x3C4, 0x01); 
    port_byte_out(0x3C5, seq_reg);       
}