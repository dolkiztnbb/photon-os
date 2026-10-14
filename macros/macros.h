#include <setup.h>

// The address where OS loads all of the other code
#define mainaddress 0x100000

// TODO: swap & divide
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 480

// For font changes like switching between list and pointer
#define FONT_CL_CH []

// Drawing flags
#define DRAW_BG 0b001
#define DRAW_FG 0b010
#define DRAW_THICK 0b100
#define height_char 6

// Unimplemented
#define bool char // ????????????????

// Compatibility
#define outb port_byte_out
#define inb port_byte_in
#define outw port_word_out
#define inw port_word_in

// Math
#define M_PI 3.141592654
#define CYCLES 40

#define EXTENDED_KERNEL //__attribute__((section(".loaded")))
