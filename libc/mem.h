#ifndef MEM_H
#define MEM_H

#include <libc/stdint>

#define memcpy(d, s, n) memory_copy(s, d, n)
#define memset(s, v, n) memory_set(s, v, n)

void memory_copy(const void *source, void *dest, const int nbytes);
void memory_set(void *dest, const uint8_t val, const uint32_t len);

#endif
