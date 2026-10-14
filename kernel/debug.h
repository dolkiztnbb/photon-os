#include <stdarg.h>
#include <libc/stdint>

extern uint32_t tps;

void kprintd(const char *format, ...); 
void kprintw(const char *format, ...); 
void kprinti(const char *format, ...); 
void kprinte(const char *format, ...); 
void hexdump(char* ptr);
