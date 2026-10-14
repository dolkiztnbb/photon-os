#include "ksym.h"
#include <kernel/files.h>
#include <cpu/malloc/kmalloc.h>
#include <libc/string.h>

static ksym_t symbols[MAX_KERNEL_SYMBOLS];
static int total_symbols = 0;

static uint32_t parse_hex(const char* s) {
    uint32_t val = 0;
    while (*s && *s != ';') {
        char c = *s++;
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    }
    return val;
}

void ksym_load(void) {
    int fd = kopen("/sys/kernelmapping.txt", 0x01); // READ
    if (fd < 0) return;

    FILE_QUERY q;
    kquery(fd, &q);
    if (q.size == 0) {
        kclose(fd);
        return;
    }

    char* buf = (char*)kmalloc(q.size + 1);
    if (!buf) {
        kclose(fd);
        return;
    }

    kread(fd, buf, q.size);
    buf[q.size] = '\0';
    kclose(fd);

    char* ptr = buf;

    // Пропускаем первую строку с количеством (например, "400;\n")
    while (*ptr && *ptr != '\n') ptr++;
    if (*ptr == '\n') ptr++;

    // Считываем записи вида "00100000;func_name\n"
    total_symbols = 0;
    while (*ptr && total_symbols < MAX_KERNEL_SYMBOLS) {
        while (*ptr == '\r' || *ptr == '\n') ptr++;
        if (!*ptr) break;

        symbols[total_symbols].addr = parse_hex(ptr);

        // Ищем разделитель ';'
        while (*ptr && *ptr != ';') ptr++;
        if (*ptr == ';') ptr++;

        // Копируем имя функции
        int j = 0;
        while (*ptr && *ptr != '\r' && *ptr != '\n' && j < 31) {
            symbols[total_symbols].name[j++] = *ptr++;
        }
        symbols[total_symbols].name[j] = '\0';

        total_symbols++;
        while (*ptr && *ptr != '\n') ptr++;
    }

    kfree(buf);
}

// Бинарный поиск: ищем функцию с наибольшим addr <= target_addr
const char* ksym_resolve(uint32_t addr, uint32_t *offset) {
    if (total_symbols == 0) {
        if (offset) *offset = 0;
        return "?";
    }

    int left = 0, right = total_symbols - 1;
    int best = -1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (symbols[mid].addr <= addr) {
            best = mid;
            left = mid + 1; // пробуем найти ближе
        } else {
            right = mid - 1;
        }
    }

    if (best != -1) {
        if (offset) *offset = addr - symbols[best].addr;
        return symbols[best].name;
    }

    if (offset) *offset = 0;
    return "unknown";
}