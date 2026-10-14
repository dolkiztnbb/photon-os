#include "fs.h"

uint32_t dnode_read(Dnode* node, char* buffer, uint32_t length, uint32_t offset) {
    if (!node) return 0;
    
    // Если переопределен кастомный обработчик (псевдофайл)
    if (node->read != 0) {
        return node->read(node, buffer, length, offset);
    }

    // Стандартное чтение из ОЗУ (обычный файл)
    if (!node->data || offset >= node->size) return 0;
    
    uint32_t bytes_to_read = length;
    if (offset + bytes_to_read > node->size) {
        bytes_to_read = node->size - offset;
    }

    memcpy(buffer, node->data + offset, bytes_to_read);
    return bytes_to_read;
}

uint32_t dnode_write(Dnode* node, char* buffer, uint32_t length, uint32_t offset) {
    // 1. Базовые проверки входных параметров
    if (!node || !buffer || length == 0) {
        return 0;
    }

    // Запрещаем прямую запись в директории
    if (node->type == TYPE_DIR) {
        return 0;
    }

    // 2. Делегирование кастомному обработчику (Псевдофайлы / /dev / /proc)
    if (node->write != 0) {
        kprintd("delegating writing to custom handler\n");
        return node->write(node, buffer, length, offset);
    }

    // 3. Запись в обычный RAM-файл
    uint32_t new_required_size = offset + length;

    // Если запись выходит за пределы выделенной памяти — выделяем/расширяем буфер
    if (new_required_size > node->size) {
        
        // Перевыделяем память под новый размер
        // char* new_data = (char*)realloc(node->data, new_required_size);
        char* new_data = (char*)kmalloc(new_required_size);
        if (new_data) {
            if (node->data) {
                memcpy(new_data, node->data, node->size);
                kfree(node->data);
            }
            node->data = new_data;
        }
        // Обработяка ошибки нехватки памяти
        if (!new_data) {
            // Если offset лежит в пределах старого размера, пишем только то, что поместится
            if (offset < node->size) {
                uint32_t available_space = node->size - offset;
                memcpy(node->data + offset, buffer, available_space);
                return available_space;
            }
            return 0; // Не удалось выделить память
        }

        // Если запись начинается ДАЛЬШЕ конца файла (offset > node->size),
        // заполняем образовавшуюся "дыру" нулями (sparse-behavior)
        if (offset > node->size) {
            memset(new_data + node->size, 0, offset - node->size);
        }

        node->data = new_data;
        node->size = new_required_size;
    }

    // 4. Копируем данные из входного буфера в буфер файла
    memcpy(node->data + offset, buffer, length);

    // 5. Помечаем узел как измененный (опционально, если используется системный таймер)
    // node->change_time = get_system_time();

    return length;
}

// /dev/0
uint32_t dnode_dev_0_read(Dnode* node, char* buffer, uint32_t length, uint32_t offset) {
    return 0; // Всегда EOF
}

uint32_t dnode_dev_0_write(Dnode* node, char* buffer, uint32_t length, uint32_t offset) {
    return length; // "Сьедает" все записанные данные
}

// /dev/zero
uint32_t dnode_dev_zero_read(Dnode* node, char* buffer, uint32_t length, uint32_t offset) {
    memset(buffer, 0, length);
    return length; // Бесконечный поток нулей
}