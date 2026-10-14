#ifndef MT_H
#define MT_H

#include <stdint.h>
#include <stddef.h>
#include <cpu/vmm.h>
#include <kernel/datslaf/fs.h>

#define DEFAULT_STACK_SIZE 0x4000
#define DEFAULT_HEAP_SIZE  0x10000
#define PAGE_SIZE 4096

typedef enum {
    TASK_RUNNING,
    TASK_PAUSED,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_KILLED
} task_state_t;

typedef struct {
    int pid;
    uint32_t heap_start;
    uint32_t heap_curr;
    uint32_t heap_end;
} task_context_info_t;

typedef struct {
    uint32_t esp;
    uint32_t cr3;
    uint32_t stack_base;
    uint32_t stack_size;
    uint32_t text_size;
    uint16_t flags;            // <-- ДОБАВИТЬ: флаги привилегий процесса (PEX flags)
    uint8_t active;
    task_state_t state;
    uint32_t sleep_until_tick; // Для SYS_SLEEP
    int uid;                   // <-- 0 для root, 1000 для обычного пользователя
    int exit_code;             // Для SYS_WAITPID / SYS_EXIT
    int parent_pid;            // PID родительского процесса
    Dnode* cwd;                // Текущая рабочая директория
    task_context_info_t info;
    process_space_t proc_space;
} task_t;

void mt_init(void);
int  mt_add_task_ex(void (*entry_point)(void*), uint32_t stack_size, uint32_t heap_size);
int  mt_add_task(void (*entry_point)(void*));
int  mt_add_task_ex_vmm(void (*entry_point)(void*), uint32_t stack_size, uint32_t heap_size, 
                       process_space_t proc, uint32_t text_size, int argc, char** argv, 
                       uint16_t pex_flags);
void mt_schedule(void);
void mt_pause_task(int pid);
void mt_resume_task(int pid);
void mt_kill_task(int pid);
void mt_exit_self_code(int code);
void mt_exit_self(void);

uint32_t mt_get_current_pid(void);
task_t*  mt_get_current_task(void);
task_t*  mt_get_task(int pid);
int      mt_get_tasks(void);
void     mt_list_tasks(void);
int      mt_status(uint32_t pid);
void*    sys_malloc(uint32_t size);
void*    sys_brk(void* addr);
int      sys_waitpid(int pid, int* status);
void     sys_sleep(uint32_t ms);

void mt_switch_context(uint32_t *old_esp, uint32_t new_esp, uint32_t new_cr3);

#endif