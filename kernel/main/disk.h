#include <stdint.h>

// Установленные функции для работы с портами
uint8_t pbi(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void pbo(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

// Задержка для ожидания готовности диска
void disk_wait() {
    while (1) {
        uint8_t status = pbi(0x1F7); // Считываем статус с порта 0x1F7
        if (!(status & 0x80)) {               // Проверяем флаг готовности
            if (status & 0x01) {              // Проверяем флаг ошибки
                // Обработка ошибки
                uint8_t error = pbi(0x1F1); // Читаем код ошибки
                return;                       // Возвращаемся при ошибке
            }
            return;                           // Если все нормально, выходим
        }
    }
}

// Функция для получения размера сектора диска
uint16_t get_sector_size(uint8_t drive) {
    // Отправляем команду IDENTIFY DEVICE (0xEC)
    pbo(0x1F6, 0xA0 | (drive << 4)); // Выбираем диск
    pbo(0x1F7, 0xEC);               // Команда IDENTIFY DEVICE
    disk_wait();

    // Создаем буфер для чтения данных
    uint16_t buffer[256];
    for (int i = 0; i < 256; i++) {
        buffer[i] = pbi(0x1F0) | (pbi(0x1F0) << 8);
    }

    // Сектор 0xBА содержит размер сектора
    return buffer[0xBA];
}

// Функция записи данных на диск

