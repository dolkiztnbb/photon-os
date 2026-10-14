#include <mp.h>
#include <../../libc/mem.h>  // Adjust path as needed
#include <../../libc/string.h>
#include <../../drivers/screen.h>
#define NULL (void*)0

// Global process management variables
pcb_t* current_process = NULL;
pcb_t* ready_queue = NULL;
pcb_t* process_list = NULL;
uint32_t next_pid = 1;
uint32_t total_processes = 0;
volatile uint32_t scheduler_started = 0;

EXTENDED_KERNEL pcb_t* create_process(void (*function)(void), uint32_t stack_size, uint32_t priority) {
    pcb_t* new_process = (pcb_t*)kmalloc(sizeof(pcb_t));
    if (!new_process) return NULL;

    // Allocate stack
    new_process->base_stack = (uint32_t*)kmalloc(stack_size * sizeof(uint32_t));
    if (!new_process->base_stack) {
        kfree(new_process);
        return NULL;
    }

    new_process->pid = next_pid++;
    new_process->state = PROCESS_STATE_READY;
    new_process->stack_size = stack_size;
    new_process->entry_point = function;
    new_process->priority = priority;
    new_process->quantum_remaining = 10; // Default time slice
    new_process->time_slice_counter = 0;
    new_process->next = NULL;

    // Set up initial stack context
    new_process->stack_pointer = new_process->base_stack + stack_size - 16;
    
    // Initialize minimal stack frame for process execution
    uint32_t* stack_ptr = new_process->stack_pointer;
    
    // Setup return address and flags for process entry
    *(--stack_ptr) = (uint32_t)function;  // EIP - where process starts
    *(--stack_ptr) = 0x202;               // EFLAGS - interrupts enabled
    
    new_process->stack_pointer = stack_ptr;

    // Add to process list
    new_process->next = process_list;
    process_list = new_process;
    total_processes++;

    // Add to ready queue
    if (ready_queue == NULL) {
        ready_queue = new_process;
    } else {
        // Add to end of queue (round robin)
        pcb_t* temp = ready_queue;
        while (temp->next != NULL) {
            temp = temp->next;
        }
        temp->next = new_process;
    }

    return new_process;
}

EXTENDED_KERNEL void destroy_process(pcb_t* process) {
    if (!process || !total_processes) return;

    // Remove from ready queue
    if (ready_queue == process) {
        ready_queue = process->next;
    } else {
        pcb_t* temp = ready_queue;
        while (temp && temp->next != process) {
            temp = temp->next;
        }
        if (temp) temp->next = process->next;
    }

    // Remove from process list
    if (process_list == process) {
        process_list = process->next;
    } else {
        pcb_t* temp = process_list;
        while (temp && temp->next != process) {
            temp = temp->next;
        }
        if (temp) temp->next = process->next;
    }

    // Free resources
    if (process->base_stack) {
        kfree(process->base_stack);
    }
    kfree(process);
    total_processes--;

    if (current_process == process) {
        current_process = NULL;
    }
}

EXTENDED_KERNEL pcb_t* get_process_by_pid(pid_t pid) {
    pcb_t* current = process_list;
    while (current) {
        if (current->pid == pid) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

EXTENDED_KERNEL int kill_process(pid_t pid) {
    pcb_t* process = get_process_by_pid(pid);
    if (!process) return -1; // Process not found
    
    if (process == current_process) {
        terminate_current_process();
    } else {
        destroy_process(process);
    }
    return 0; // Success
}

EXTENDED_KERNEL void schedule_next(void) {
    if (!ready_queue || total_processes <= 1) return;

    pcb_t* old_process = current_process;

    // Decrement quantum for current process and increment time slice counter
    if (current_process) {
        if (current_process->quantum_remaining > 0) {
            current_process->quantum_remaining--;
        }
        current_process->time_slice_counter++;
    }

    // Find next process in ready queue
    pcb_t* next_process = ready_queue;
    
    if (ready_queue->next) {
        // Move head to tail (round robin)
        pcb_t* temp = ready_queue;
        while (temp->next) {
            temp = temp->next;
        }
        // Move current head to tail
        temp->next = ready_queue;
        ready_queue = ready_queue->next;
        temp->next->next = NULL;
        next_process = ready_queue;
    }

    // Reset quantum for next process
    next_process->quantum_remaining = 10;

    // Update process states
    if (old_process) {
        old_process->state = PROCESS_STATE_READY;
    }
    next_process->state = PROCESS_STATE_RUNNING;

    // Perform context switch if necessary
    if (old_process != next_process) {
        pcb_t* old_current = current_process;
        current_process = next_process;
        
        switch_context(&old_current->stack_pointer, current_process->stack_pointer);
    }
}

EXTENDED_KERNEL void yield_to_scheduler(void) {
    if (scheduler_started && ready_queue && total_processes > 1) {
        schedule_next();
    }
}

EXTENDED_KERNEL void init_multiprocessing(void) {
    current_process = NULL;
    ready_queue = NULL;
    process_list = NULL;
    next_pid = 1;
    total_processes = 0;
    scheduler_started = 0;
}

EXTENDED_KERNEL void terminate_current_process(void) {
    if (current_process) {
        pcb_t* to_destroy = current_process;
        current_process = NULL;
        
        // Schedule next process which will handle the cleanup
        schedule_next();
        
        // Actually destroy the process
        destroy_process(to_destroy);
    }
}

EXTENDED_KERNEL int start_process_scheduler(void) {
    if (!ready_queue) {
        return -1; // No processes to schedule
    }

    // Set up the first process
    current_process = ready_queue;
    current_process->state = PROCESS_STATE_RUNNING;
    ready_queue = ready_queue->next;
    
    scheduler_started = 1;
    
    // Now jump to the first process - this is where the magic happens!
    // We use a special context switch that doesn't return to the caller
    uint32_t* dummy_stack = NULL;
    switch_context(&dummy_stack, current_process->stack_pointer);
    
    // We should never reach here if context switching works properly
    return 0;
}
