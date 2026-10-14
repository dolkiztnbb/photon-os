#include <cpu/isr.h>
#include <cpu/idt.h>
#include <libc/string.h>

/*


nagiev echpochmak

  X       X
 / \     / \
X   \   /   \
 \   \ /     \
  \   X   X   \
   \     / \   \
    \   /   \   X
     \ /     \ /
      X       X
*/

#define OVERRIDE_SIZE 12

uint32_t cannot_override[OVERRIDE_SIZE] = {
    0x00000000, // You just can't, OKAY??
    0x00000001, // Debug, used by kernel
    0x00000006, // Invalid Opcode
    0x00000008, // Timer interrupt
    0x0000000d, // General Protection Fault
    0x00000020, // Timer IRQ
    0x00000080, // Kernel
};  // Though it's ok to rewrite the keyboard or the mouse
    // if the default ones doesn't work

void nop(){
    return;
}

int load_other_int(char* data){
    // 4 bytes "idrv" magic number, Interrupt Driver
    // 4 bytes interrupt number
    // 4 bytes length of the function
    // ? bytes interrupt function
    // 4 bytes interrupt init function
    // ? bytes interrupt init function

    char* signature = "idrv";
    char compare[4];

    // Copy from data[:4] to compare
    strncpy(compare, data, 4);

    if(strcmp(signature, compare)!=0){
        return 1;
        // Could not load interrupt driver: invalid magic number
    }

    data += 4;

    uint32_t interrupt_n = *(uint32_t*)data;
    data += 4;

    uint32_t interrupt_length = *(uint32_t*)data;
    data += 4; // Code

	isr_t interrupt_function = (isr_t)data;
    data += interrupt_length;

    uint32_t interrupt_init_length = *(uint32_t*)data;
    data += 4; // Code

    void(*interrupt_init_function)() = (void(*)())((void*)(data));
    data += interrupt_init_length;


    for ( int i = 0; i < OVERRIDE_SIZE; i++ ){
        if(interrupt_n == cannot_override[i] )
            return 2;
        // Could not override already set/reserved interrupts
    }
 
    // Run driver init
    interrupt_init_function();
    // Set interrupt handler
    // register_interrupt_handler(interrupt_n,interrupt_function);
    register_interrupt_handler(interrupt_n,interrupt_function);
    return 0;
}

void int128(const registers_t *r){

}
