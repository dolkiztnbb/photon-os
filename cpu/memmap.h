// simple_alloc.c

// Настройки
#define HEAP_START      0x800000
#define HEAP_SIZE       (1024 * 1024)  // Например, 16 МБ
#define ALIGNMENT       4
#define ALIGN_MASK      (ALIGNMENT - 1)

// Утилиты выравнивания
#define ALIGN_UP(x)      (((x) + ALIGN_MASK) & ~ALIGN_MASK)
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define HDR_SIZE         ALIGN_UP(sizeof(block_header_t))

// Флаги состояния блока
#define BLOCK_USED      0x1
#define BLOCK_SIZE_MASK (~ALIGN_MASK)  // младшие биты — флаги

// Заголовок блока (в начале каждого блока)
typedef struct block_header {
    uint32_t size_and_flags;           // размер + флаги в младших битах
    struct block_header* next_free;    // только если блок свободен
    struct block_header* prev_free;    // для двусвязного списка свободных
} block_header_t;

// Глобальный указатель на начало кучи и свободный список
static block_header_t* heap_start = NULL;
static block_header_t* free_list_head = NULL;

// Вспомогательные функции
static inline uint32_t get_size(block_header_t* b) {
    return b->size_and_flags & BLOCK_SIZE_MASK;
}

static inline int is_used(block_header_t* b) {
    return b->size_and_flags & BLOCK_USED;
}

static inline void set_used(block_header_t* b, int used) {
    if (used)
        b->size_and_flags |= BLOCK_USED;
    else
        b->size_and_flags &= ~BLOCK_USED;
}

static inline block_header_t* next_block(block_header_t* b) {
    return (block_header_t*)((char*)b + get_size(b));
}

// Удаление блока из свободного списка
static void free_list_remove(block_header_t* b) {
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
        free_list_head = b->next_free;

    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
}

// Вставка блока в свободный список (в начало)
static void free_list_insert(block_header_t* b) {
    b->next_free = free_list_head;
    b->prev_free = NULL;
    if (free_list_head)
        free_list_head->prev_free = b;
    free_list_head = b;
}

// Слияние с правым соседом (если свободен)
static void coalesce_right(block_header_t* b) {
    block_header_t* next = next_block(b);
    // Проверяем, не вышли ли за пределы кучи
    if ((uintptr_t)next >= (uintptr_t)heap_start + HEAP_SIZE)
        return;
    if (!is_used(next)) {
        free_list_remove(next);
        b->size_and_flags = (get_size(b) + get_size(next)) | (b->size_and_flags & BLOCK_USED);
    }
}

// Слияние с левым соседом (если свободен) — требует обратного указателя или сканирования
// Упрощённый вариант: не поддерживаем левый коалесцинг без footers.
// Альтернатива: используем footer (но увеличивает overhead). Здесь — только правый коалесцинг.

// Инициализация кучи
void mem_init(void) {
    heap_start = (block_header_t*)HEAP_START;

    // Один большой свободный блок
    uint32_t total_size = ALIGN_DOWN(HEAP_SIZE, ALIGNMENT);
    heap_start->size_and_flags = total_size;  // изначально не used
    set_used(heap_start, 0);

    free_list_head = heap_start;

    // Инициализируем "стражевой" блок в конце, чтобы next_block() не вылетал
    block_header_t* sentinel = next_block(heap_start);
    sentinel->size_and_flags = 0;  // размер 0 = конец
    set_used(sentinel, 1);         // помечаем как used, чтобы не объединялся
}

// Выделение памяти
void* mem_alloc(uint32_t user_size) {
    if (user_size == 0) return NULL;

    uint32_t payload_size = ALIGN_UP(user_size);
    uint32_t total_needed = payload_size + HDR_SIZE;

    block_header_t* best = NULL;
    block_header_t* cur = free_list_head;

    // First-fit поиск
    while (cur) {
        if (!is_used(cur) && get_size(cur) >= total_needed) {
            best = cur;
            break;
        }
        cur = cur->next_free;
    }

    if (!best) return NULL; // нет подходящего блока

    // Удаляем из свободного списка
    free_list_remove(best);

    uint32_t block_size = get_size(best);
    // Разделяем блок, если остаток >= минимальному размеру блока
    uint32_t min_split_size = HDR_SIZE + ALIGN_UP(1); // хотя бы 1 байт + header
    if (block_size >= total_needed + min_split_size) {
        // Создаём новый блок после выделенного
        block_header_t* new_block = (block_header_t*)((char*)best + total_needed);
        new_block->size_and_flags = (block_size - total_needed);
        set_used(new_block, 0);
        free_list_insert(new_block);

        // Обновляем размер текущего блока
        best->size_and_flags = total_needed | BLOCK_USED;
    } else {
        // Используем весь блок
        set_used(best, 1);
    }

    return (void*)((char*)best + HDR_SIZE);
}

// Освобождение памяти
void mem_free(void* ptr) {
    if (!ptr) return;

    block_header_t* block = (block_header_t*)((char*)ptr - HDR_SIZE);
    set_used(block, 0);
    coalesce_right(block);
    free_list_insert(block);
}