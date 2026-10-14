#ifndef MP_H
#define MP_H

#include <stdint.h>

// Process states
typedef enum {
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_WAITING,
    PROCESS_STATE_TERMINATED
} process_state_t;

// Process ID type
typedef uint32_t pid_t;

// Process Control Block (PCB)
typedef struct pcb {
    pid_t pid;
    process_state_t state;
    uint32_t* stack_pointer;
    uint32_t* base_stack;
    uint32_t stack_size;
    void (*entry_point)(void);
    struct pcb* next;
    uint32_t quantum_remaining;  // Time slices remaining
    uint32_t priority;
    uint32_t time_slice_counter; // How long this process has been running
} pcb_t;

// Process management functions
pcb_t* create_process(void (*function)(void), uint32_t stack_size, uint32_t priority);
void destroy_process(pcb_t* process);
void schedule_next(void);
void yield_to_scheduler(void);
void init_multiprocessing(void);
void terminate_current_process(void);
int start_process_scheduler(void);  // New function to start the scheduler
pcb_t* get_process_by_pid(pid_t pid);
int kill_process(pid_t pid);
void sleep_process(uint32_t milliseconds); // For current process to sleep

// Global variables
extern pcb_t* current_process;
extern pcb_t* ready_queue;
extern pcb_t* process_list;
extern volatile uint32_t scheduler_started;
extern uint32_t total_processes;

// Context switching (assembly)
void switch_context(uint32_t** old_esp, uint32_t* new_esp);

#endif
