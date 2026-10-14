#ifndef KSYM_H
#define KSYM_H

#include <stdint.h>

#define MAX_KERNEL_SYMBOLS 1024

typedef struct {
    uint32_t addr;
    char name[32];
} ksym_t;

// Загрузка таблицы из /sys/kernelmapping.txt (вызывается 1 раз при старте ядра)
void ksym_load(void);

// Поиск имени функции по адресу (возвращает имя и смещение в байтах)
const char* ksym_resolve(uint32_t addr, uint32_t *offset);

#endif