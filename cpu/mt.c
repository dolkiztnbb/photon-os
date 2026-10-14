#include "mt.h"
#include "timer.h"
#include <kernel/debug.h>
#include <libc/mem.h>
#include <kernel/pex/header.h>
#include <kernel/pex/loader.h>

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);
extern Dnode* fs_root;

task_t* tasks = 0;
static int capacity_tasks = 0;
int current_task_idx = 0;
static uint32_t total_tasks = 0;

extern uint32_t page_directory[1024];

void mt_init(void) {
    capacity_tasks = 64;
    tasks = (task_t*)kmalloc(sizeof(task_t) * capacity_tasks);
    memset(tasks, 0, sizeof(task_t) * capacity_tasks);

    tasks[0].esp = 0;
    tasks[0].stack_base = 0;
    tasks[0].stack_size = 0;
    tasks[0].active = 1;
    tasks[0].flags = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].cr3 = (uint32_t)page_directory;
    tasks[0].proc_space.pd_virt = (uint32_t*)page_directory;
    tasks[0].proc_space.cr3_phys = (uint32_t)page_directory;
    tasks[0].cwd = fs_root;
    tasks[0].parent_pid = -1;
    tasks[0].info.pid = 0;
    tasks[0].uid = 0; // Root

    total_tasks = 1;
    current_task_idx = 0;
}

task_t* mt_get_current_task(void) {
    if (tasks == NULL || current_task_idx >= (int)total_tasks) return NULL;
    return &tasks[current_task_idx];
}

task_t* mt_get_task(int pid) {
    if (tasks == NULL || pid < 0 || (uint32_t)pid >= total_tasks) return NULL;
    return &tasks[pid];
}

static void mt_free_task_memory_only(task_t *t) {
    if (!t) return;
    if (t->cr3 != 0 && t->proc_space.pd_virt != NULL) {
        // Освобождаем только реально выделенную часть кучи (heap_curr - heap_start),
        // либо весь диапазон до heap_end
        uint32_t heap_span = t->info.heap_end - t->info.heap_start;
        vmm_destroy_process_space(
            &t->proc_space,
            t->text_size,
            t->stack_size,
            heap_span
        );
    } else {
        if (t->stack_base) kfree((void*)t->stack_base);
        if (t->info.heap_start) kfree((void*)t->info.heap_start);
    }
    t->cr3 = 0;
    t->proc_space.pd_virt = NULL;
    t->proc_space.cr3_phys = 0;
    t->stack_base = 0;
}

static void mt_free_task_resources(task_t *t) {
    if (!t) return;
    mt_free_task_memory_only(t);
    t->active = 0;
    t->state = TASK_KILLED;
}

// Автоматически освобождает физические страницы завершившихся процессов (TASK_ZOMBIE),
// как только процессор переключился на стек и CR3 другой задачи.
static void mt_reap_zombie_pages(void) {
    for (uint32_t i = 1; i < total_tasks; i++) {
        if ((int)i == current_task_idx) continue;
        if (tasks[i].state == TASK_ZOMBIE && tasks[i].cr3 != 0) {
            mt_free_task_memory_only(&tasks[i]);
        }
    }
}

int mt_add_task_ex(void (*entry_point)(void*), uint32_t stack_size, uint32_t heap_size) {
    if (tasks == 0) mt_init();
    mt_reap_zombie_pages();

    if (total_tasks >= (uint32_t)capacity_tasks) {
        int new_capacity = capacity_tasks * 2;
        task_t* new_tasks = (task_t*)kmalloc(sizeof(task_t) * new_capacity);
        if (!new_tasks) return -1;

        memset(new_tasks, 0, sizeof(task_t) * new_capacity);
        for (uint32_t i = 0; i < total_tasks; i++) new_tasks[i] = tasks[i];
        kfree(tasks);
        tasks = new_tasks;
        capacity_tasks = new_capacity;
    }
    if (stack_size == 0) stack_size = DEFAULT_STACK_SIZE;
    if (heap_size == 0)  heap_size  = DEFAULT_HEAP_SIZE;

    int new_pid = total_tasks;
    task_t *t = &tasks[new_pid];

    void* stack_mem = kmalloc(stack_size);
    if (!stack_mem) return -1;

    void* heap_mem = kmalloc(heap_size);
    if (!heap_mem) {
        kfree(stack_mem);
        return -1;
    }

    t->stack_base = (uint32_t)stack_mem;
    t->stack_size = stack_size;
    t->active = 1;
    t->state = TASK_RUNNING;
    t->cr3 = 0;
    t->flags = 0;
    t->cwd = tasks[current_task_idx].cwd ? tasks[current_task_idx].cwd : fs_root;
    t->parent_pid = current_task_idx;

    t->info.pid = new_pid;
    t->info.heap_start = (uint32_t)heap_mem;
    t->info.heap_curr  = (uint32_t)heap_mem;
    t->info.heap_end   = (uint32_t)heap_mem + heap_size;

    uint32_t *stk = (uint32_t*)(t->stack_base + stack_size);

    *(--stk) = (uint32_t)&t->info;
    *(--stk) = (uint32_t)mt_exit_self;
    *(--stk) = (uint32_t)entry_point;
    *(--stk) = 0x202;

    for (int i = 0; i < 8; i++) *(--stk) = 0;

    t->esp = (uint32_t)stk;
    total_tasks++;
    return new_pid;
}

int mt_add_task_ex_vmm(void (*entry_point)(void*), uint32_t stack_size, uint32_t heap_size,
                       process_space_t proc, uint32_t text_size, int argc, char** argv,
                       uint16_t pex_flags) {
    if (tasks == 0) mt_init();
    if (stack_size == 0) stack_size = DEFAULT_STACK_SIZE;
    if (heap_size == 0)  heap_size  = DEFAULT_HEAP_SIZE;

    // Сразу освобождаем страницы всех закрытых (TASK_ZOMBIE) задач
    mt_reap_zombie_pages();

    if (total_tasks >= (uint32_t)capacity_tasks) {
        int new_capacity = capacity_tasks * 2;
        task_t* new_tasks = (task_t*)kmalloc(sizeof(task_t) * new_capacity);
        if (!new_tasks) return -1;

        memset(new_tasks, 0, sizeof(task_t) * new_capacity);
        for (uint32_t i = 0; i < total_tasks; i++) new_tasks[i] = tasks[i];
        kfree(tasks);
        tasks = new_tasks;
        capacity_tasks = new_capacity;
    }

    int new_pid = total_tasks;
    task_t *t = &tasks[new_pid];

    t->active = 1;
    t->state = TASK_RUNNING;
    t->flags = pex_flags;
    t->cr3 = proc.cr3_phys;
    t->proc_space = proc;
    t->text_size = text_size;
    t->stack_size = stack_size;
    t->cwd = tasks[current_task_idx].cwd ? tasks[current_task_idx].cwd : fs_root;
    t->parent_pid = current_task_idx;
    t->info.pid = new_pid;
    t->uid = tasks[current_task_idx].uid;

    if (pex_flags & PEX_FLAG_SUID) {
        t->uid = 0;
    }

    t->info.heap_start = PEX_HEAP_BASE;
    t->info.heap_curr  = PEX_HEAP_BASE;
    t->info.heap_end   = PEX_HEAP_BASE + heap_size;

    uintptr_t phys_stack_page = vmm_get_phys_mapping(proc.pd_virt, PEX_STACK_BASE - PAGE_SIZE);
    if (!phys_stack_page) return -1;

    // --- ФОРМИРОВАНИЕ POSIX СТЕКА (argc / argv) ---
    uint32_t strings_len = 0;
    if (argv) {
        for (int i = 0; i < argc; i++) {
            if (argv[i]) strings_len += strlen(argv[i]) + 1;
        }
    }

    char* phys_base = (char*)(phys_stack_page + PAGE_SIZE);
    uint32_t virt_base = PEX_STACK_BASE;

    phys_base -= strings_len;
    virt_base -= strings_len;

    uint32_t argv_virt[32];
    char* curr_phys = phys_base;
    uint32_t curr_virt = virt_base;

    if (argv) {
        for (int i = 0; i < argc; i++) {
            argv_virt[i] = curr_virt;
            if (argv[i]) {
                int len = strlen(argv[i]) + 1;
                memcpy(curr_phys, argv[i], len);
                curr_phys += len;
                curr_virt += len;
            }
        }
    }
    argv_virt[argc] = 0;

    uint32_t align_offset = (uint32_t)phys_base & 3;
    phys_base -= align_offset;
    virt_base -= align_offset;

    uint32_t array_size = (argc + 1) * sizeof(uint32_t);
    phys_base -= array_size;
    virt_base -= array_size;
    memcpy(phys_base, argv_virt, array_size);
    uint32_t user_argv_ptr = virt_base;

    uint32_t initial_eflags = 0x202; // IF=1
    if (pex_flags & PEX_FLAG_ALLOW_IO) {
        initial_eflags |= (3 << 12); // IOPL=3
    }

    uint32_t* stk = (uint32_t*)phys_base;
    *(--stk) = user_argv_ptr;           // [ESP+8] char** argv
    *(--stk) = argc;                    // [ESP+4] int argc
    *(--stk) = 0x00000000;              // [ESP]   EIP возврата
    *(--stk) = (uint32_t)entry_point;   // EIP
    *(--stk) = initial_eflags;          // EFLAGS
    for (int i = 0; i < 8; i++) *(--stk) = 0;

    t->esp = (uint32_t)stk - phys_stack_page + (PEX_STACK_BASE - PAGE_SIZE);
    total_tasks++;
    return new_pid;
}

int mt_add_task(void (*entry_point)(void*)) {
    return mt_add_task_ex(entry_point, DEFAULT_STACK_SIZE, DEFAULT_HEAP_SIZE);
}

void mt_pause_task(int pid) {
    if (pid > 0 && pid < (int)total_tasks) tasks[pid].state = TASK_PAUSED;
}

void mt_resume_task(int pid) {
    if (pid > 0 && pid < (int)total_tasks) tasks[pid].state = TASK_RUNNING;
}

void mt_kill_task(int pid) {
    if (pid >= 0 && pid < (int)total_tasks) {
        if (tasks[pid].active || tasks[pid].state != TASK_KILLED) {
            if (pid == current_task_idx) {
                mt_exit_self();
            } else {
                mt_free_task_resources(&tasks[pid]);
            }
        }
    }
}

void mt_exit_self_code(int code) {
    // Запрещаем прерывания, чтобы таймер не переключил контекст посередине завершения
    asm volatile("cli");

    tasks[current_task_idx].state = TASK_ZOMBIE;
    tasks[current_task_idx].active = 0;
    tasks[current_task_idx].exit_code = code;

    uint32_t current_tick = get_ticks();
    for (uint32_t i = 0; i < total_tasks; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].active) {
            if (current_tick >= tasks[i].sleep_until_tick) {
                tasks[i].state = TASK_RUNNING;
            }
        }
    }

    int next_idx = -1;
    for (uint32_t i = 0; i < total_tasks; i++) {
        if (i != (uint32_t)current_task_idx && tasks[i].state == TASK_RUNNING && tasks[i].active) {
            next_idx = i;
            break;
        }
    }

    // Если все живые задачи (например, wm) сейчас спят в sleep(), разбудим ближайшую спящую,
    // чтобы не переключаться на мёртвый/неверный стек!
    if (next_idx == -1) {
        for (uint32_t i = 0; i < total_tasks; i++) {
            if (i != (uint32_t)current_task_idx && tasks[i].active && tasks[i].state == TASK_SLEEPING) {
                tasks[i].state = TASK_RUNNING;
                next_idx = i;
                break;
            }
        }
    }

    if (next_idx == -1) {
        next_idx = 0;
        tasks[0].state = TASK_RUNNING;
        tasks[0].active = 1;
    }

    int old_idx = current_task_idx;
    current_task_idx = next_idx;
    mt_switch_context(&tasks[old_idx].esp, tasks[current_task_idx].esp, tasks[current_task_idx].cr3);

    while (1) { asm volatile("cli; hlt"); }
}

void mt_exit_self(void) {
    mt_exit_self_code(0);
}

int mt_get_tasks(void) { return total_tasks; }
uint32_t mt_get_current_pid(void) { return current_task_idx; }

void mt_schedule(void) {
    if (total_tasks < 1) return;

    asm volatile("cli");

    // Освобождаем память процессов, которые завершились на предыдущем кванте
    mt_reap_zombie_pages();

    uint32_t current_tick = get_ticks();

    // Проверяем проснувшиеся задачи
    for (uint32_t i = 0; i < total_tasks; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].active) {
            if (current_tick >= tasks[i].sleep_until_tick) {
                tasks[i].state = TASK_RUNNING;
            }
        }
    }

    int next_idx = current_task_idx;
    int found = 0;

    for (uint32_t i = 0; i < total_tasks; i++) {
        next_idx = (next_idx + 1) % total_tasks;
        if (tasks[next_idx].state == TASK_RUNNING && tasks[next_idx].active) {
            found = 1;
            break;
        }
    }

    if (!found) {
        if (tasks[0].state == TASK_RUNNING && tasks[0].active) next_idx = 0;
        else {
            asm volatile("sti");
            return;
        }
    }

    if (next_idx == current_task_idx) {
        asm volatile("sti");
        return;
    }

    int old_idx = current_task_idx;
    current_task_idx = next_idx;

    mt_switch_context(&tasks[old_idx].esp, tasks[current_task_idx].esp, tasks[current_task_idx].cr3);

    // Уже на новом стеке: если предыдущая задача только что стала ZOMBIE, сразу освобождаем её VMM
    mt_reap_zombie_pages();

    asm volatile("sti");
}

void mt_list_tasks(void) {
    for (uint32_t i = 0; i < total_tasks; i++) {
        if (tasks[i].active) {
            kprintd("T PID:%d ESP:%X CR3:%X S:%d\n", i, tasks[i].esp, tasks[i].cr3, tasks[i].state);
        }
    }
}

int mt_status(uint32_t pid) {
    if (pid >= total_tasks) return 0;
    return tasks[pid].active;
}

void* sys_malloc(uint32_t size) {
    if (size == 0) return NULL;
    task_t* t = &tasks[current_task_idx];
    size = (size + 3) & ~3;

    uint32_t alloc_addr = t->info.heap_curr;
    uint32_t new_heap_curr = alloc_addr + size;

    if (new_heap_curr > t->info.heap_end) return NULL;

    // ВАЖНО: округляем начальный адрес ВНИЗ до границы страницы (4 КБ),
    // чтобы самая первая страница кучи (0x0A000000) гарантированно выделялась!
    uint32_t page_start = alloc_addr & ~(PAGE_SIZE - 1);
    uint32_t page_end   = (new_heap_curr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t page = page_start; page < page_end; page += PAGE_SIZE) {
        if (vmm_get_phys_mapping(t->proc_space.pd_virt, page) == 0) {
            void* phys = kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
            if (!phys) return NULL;
            memset(phys, 0, PAGE_SIZE);
            vmm_map_page(t->proc_space.pd_virt, page, (uintptr_t)phys, 0x07);
            asm volatile("invlpg (%0)" :: "r"(page) : "memory");
        }
    }

    t->info.heap_curr = new_heap_curr;
    return (void*)alloc_addr;
}

void* sys_brk(void* addr) {
    task_t* t = &tasks[current_task_idx];
    uint32_t new_brk = (uint32_t)addr;
    if (new_brk == 0) return (void*)t->info.heap_curr;

    if (new_brk < t->info.heap_start || new_brk > (t->info.heap_start + 0x1000000)) {
        return (void*)-1;
    }

    if (new_brk > t->info.heap_curr) {
        uint32_t p_start = t->info.heap_curr & ~(PAGE_SIZE - 1);
        uint32_t p_end   = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint32_t page = p_start; page < p_end; page += PAGE_SIZE) {
            if (vmm_get_phys_mapping(t->proc_space.pd_virt, page) == 0) {
                void* phys = kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
                if (!phys) return (void*)-1;
                memset(phys, 0, PAGE_SIZE);
                vmm_map_page(t->proc_space.pd_virt, page, (uintptr_t)phys, 0x07);
                asm volatile("invlpg (%0)" :: "r"(page) : "memory");
            }
        }
    }
    t->info.heap_curr = new_brk;
    return (void*)t->info.heap_curr;
}

int sys_waitpid(int pid, int* status) {
    if (pid < 0 || (uint32_t)pid >= total_tasks) return -1;

    while (tasks[pid].state != TASK_ZOMBIE && tasks[pid].state != TASK_KILLED) {
        asm volatile("sti; hlt");
        mt_schedule();
    }

    if (status) *status = tasks[pid].exit_code;
    mt_free_task_resources(&tasks[pid]);
    return pid;
}

void sys_sleep(uint32_t ms) {
    task_t* t = &tasks[current_task_idx];
    t->sleep_until_tick = get_ticks() + ms;
    t->state = TASK_SLEEPING;
    mt_schedule();
}
