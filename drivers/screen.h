#ifndef _FNT_
#define _FNT_

#include <libc/stdint>
#include <macros/macros.h> 

#define MAXCHAR 256
#define MAXSYMX 8
#define MAXSYMY 8
#define DRAW_BG 0b001
#define DRAW_FG 0b010
#define DRAW_THICK 0b100

extern int offsetx, offsety;

void drawchar(const unsigned char font[], const char cNum, const int x, const int y, const unsigned short color, const char attr);

void print_at(const unsigned char font[], const char* str, const int x , const int y, const unsigned short color, const char attr);

void print(const unsigned char font[], const char*  str, const unsigned short color, const char attr);
void print_int(const unsigned char font[],uint32_t x, const unsigned short color, const char attr);
void kprintf(const unsigned char font[], const unsigned short color, const char attr, const char *format, ...);

void swrite(const short x, const short y, const char color);

#endif
